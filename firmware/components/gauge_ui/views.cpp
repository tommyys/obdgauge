// What each view shows. SPEC.md section 6 lists nine; eight are here.
//
// Every title, unit, label and message below is the simulator's
// (mx5gauge/web/index.html), copied rather than reinvented -- the two are meant
// to be the same gauge on different screens, and a board that renamed ECONOMY
// or dropped a row would be a second design to keep in step.
//
// View 9 (Drives) is deliberately absent rather than present-and-empty: it
// browses a library of recorded drives, and nothing on the board records yet.
// Section 4's rule is that a view whose channels are all missing says so
// outright -- and the honest version of that for a whole feature is not to ship
// the view until it has data.
#include "views.h"
#include <cstdio>
#include <string>

#include <cmath>

namespace gauge_ui {
namespace {

// Non-ASCII in these strings is limited to two characters, and not by taste.
// LVGL's built-in Montserrat fonts are generated with `-r 0x20-0x7F,0xB0,0x2022`
// at every size this project uses, so the ONLY characters above ASCII that
// exist are the degree sign (\xC2\xB0) and the bullet (\xE2\x80\xA2).
// Anything else renders as an empty box -- which is what the middle dot the
// simulator separates with (\xC2\xB7, U+00B7) did after "COOLANT". Use the
// bullet as the separator; adding a glyph means regenerating the fonts.
//
// Views degrade honestly: '--' for a channel the car is not reporting, never a
// plausible-looking zero (SPEC.md section 4).
std::string dash() { return "--"; }

// One step of a two-colour blend, per channel. The rim arc is a single object
// and restyling it repaints only that object, so a colour that moves with the
// reading costs a ring repaint on the frames it actually changes -- not the
// panel repaint a coloured backdrop would cost (5-20 fps, measured).
uint32_t blend(uint32_t from, uint32_t to, double f) {
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const double a = (from >> sh) & 0xFF, b = (to >> sh) & 0xFF;
        out |= static_cast<uint32_t>(a + (b - a) * f + 0.5) << sh;
    }
    return out;
}

// The economy verdict, as a colour. One function, used by TRIP's ring and by
// its KM/L number, so the two can never disagree about how the drive is going.
//
// Quantised to a quarter of a km/L first: a trip average moves on every speed
// sample, and an unquantised target moved the colour every frame.
uint32_t econ_colour(const Model& m) {
    auto e = m.trip.econ_km_per_l();
    if (!e) return 0x5A5F6A;
    const double v = std::round(*e * 4.0) / 4.0;
    const double lo = gauge::kEconPoorKmL, hi = gauge::kEconGoodKmL;
    const double mid = (lo + hi) / 2.0;
    if (v <= lo) return 0xFF3B30;
    if (v >= hi) return 0x35E06B;
    return v < mid ? blend(0xFF3B30, 0xFFC53D, (v - lo) / (mid - lo))
                   : blend(0xFFC53D, 0x35E06B, (v - mid) / (hi - mid));
}

std::string num(std::optional<double> v, const char* fmt) {
    if (!v) return dash();
    char b[32];
    snprintf(b, sizeof b, fmt, *v);
    return b;
}

std::string chan(const Model& m, const char* key, const char* fmt) {
    return num(m.st.get(key), fmt);
}

// Battery voltage arrives as either the control-module PID or the adapter's
// ATRV reading; the views ask for whichever turned up.
std::optional<double> volts(const Model& m) {
    auto v = m.st.get("ctrl_volt");
    return v ? v : m.st.get("volts");
}

}  // namespace

const ViewSpec* view_table(int* count) {
    static const ViewSpec views[] = {
        // 1 --- Tacho. No title: the dial numbering would collide with it, and
        // a tacho labelled TACHO is redundant when the page dots already say
        // which view you are on. The simulator makes the same call.
        {
            nullptr,
            Instrument::TachoDial,
            Layout::Rows,
            {"rpm", "NO RPM", "this car is not reporting engine speed"},
            [](const Model& m) { return chan(m, "rpm", "%.0f"); },
            "RPM",
            nullptr,
            { [](const Model&) { return 0.0; },
              [](const Model& m) { return m.id.rpm_max; },
              nullptr,
              [](const Model& m, double* o) {
                  auto v = m.st.get("rpm"); if (!v) return false; *o = *v; return true; } },
            {
                {"KM/H", [](const Model& m) { return chan(m, "speed", "%.0f"); }},
                {"THR",  [](const Model& m) { return chan(m, "throttle", "%.0f%%"); }},
                {"PEAK", [](const Model& m) {
                    double p = m.st.peak_rpm();
                    char b[24]; snprintf(b, sizeof b, "%.0f", p); return std::string(b); }},
                {nullptr, nullptr},
            },
            "TACHO",
        },
        // 2 --- Engine (home). Coolant is the hero, not oil: section 2 says the
        // car does not report oil temperature. The rim is a cold-to-hot
        // gradient, so "in the green" is readable without the number.
        //
        // This view absorbed THERMALS on 2026-08-29. The two overlapped on
        // coolant and intake, and a separate screen for three temperatures --
        // one of which was already the hero here -- was a swipe to see a number
        // that was one swipe away. Catalyst moved in as the third cell.
        {
            "ENGINE",
            Instrument::Engine,
            // Rows, not Grid. Grid puts its cells at kSubY0 - 40, which is
            // where the state word sits: COLD/WARMING/READY landed on top of
            // the temperatures. Three small label/value lines start below the
            // word and stay inside the empty bottom of the dial.
            Layout::Rows,
            {"coolant,volts,ctrl_volt,intake,cat_b1s1,catalyst", "NO ENGINE DATA",
             "this car reports no temperatures and no voltage"},
            [](const Model& m) { return chan(m, "coolant", "%.0f\xC2\xB0"); },
            "COOLANT \xE2\x80\xA2 \xC2\xB0" "C",
            [](const Model& m, uint32_t* colour) -> std::string {
                auto c = m.st.get("coolant");
                if (!c) { *colour = 0x808080; return ""; }
                if (*c < 60)  { *colour = 0x4FA3FF; return "COLD"; }
                if (*c < 85)  { *colour = 0xFFC24A; return "WARMING"; }
                *colour = 0x5BD97A; return "READY";
            },
            { nullptr, nullptr, nullptr, nullptr },
            {
                {"IAT",  [](const Model& m) { return chan(m, "intake", "%.0f\xC2\xB0"); }},
                {"CAT",  [](const Model& m) {
                    auto c = m.st.get("catalyst");
                    if (!c) c = m.st.get("cat_b1s1");
                    return num(c, "%.0f\xC2\xB0"); }},
                {"BATT", [](const Model& m) { return num(volts(m), "%.1fv"); }},
                {nullptr, nullptr},
            },
        },
        // 3 --- Driving: the g-ball. Its own file, because a moving dot
        // with a trail is none of the shapes ViewSpec describes -- see
        // gball.h. The title, the score and the coach word are drawn there
        // too, so this row carries only what the carousel itself needs.
        //
        // B3 is settled as of 2026-08-29: the score judges how tidily you
        // drove, not how gently, and fuel economy is out of it entirely.
        {
            nullptr,                       // gball.cpp draws its own title
            Instrument::None,
            Layout::GBall,
            {"rpm,speed,throttle", "NO DRIVE DATA",
             "scoring needs rpm, speed or throttle"},
            nullptr, nullptr, nullptr,
            {nullptr, nullptr, nullptr, nullptr, nullptr},
            {{nullptr, nullptr}, {nullptr, nullptr},
             {nullptr, nullptr}, {nullptr, nullptr}},
            "DRIVING",
        },
        // 4 --- Trip. Four totals in a grid: they are read one at a time when
        // you glance down, not scanned as a list.
        //
        // This view absorbed FUEL ECONOMY on 2026-08-29. The two were the same
        // view with different heroes: both were built from gauge::Trip, and
        // fuel used and cost were on both. Economy joins as the second cell.
        {
            "TRIP",
            // A full ring on the rim that says the economy in colour alone.
            // It does not sweep: a part-filled ring invites you to read how
            // full it is, and the length of it means nothing here -- the hero
            // is distance. Green, amber or red is the whole message.
            //
            // Colouring the BACKGROUND instead was the other idea and is not
            // an option on this board: a backdrop covers the panel, so every
            // change to it repaints the panel. Measured at 5-20 fps however it
            // was drawn (build_under_tacho), which is why the tacho puts its
            // heat on the rim as well.
            Instrument::VerdictRing,
            Layout::Grid,
            {"speed,fuel_rate", "NO TRIP DATA",
             "this car reports neither vehicle speed nor fuel rate"},
            [](const Model& m) {
                char b[24]; snprintf(b, sizeof b, "%.1f", m.trip.dist_km); return std::string(b); },
            "KM",
            nullptr,
            // Always full: lo 0, hi 1, value 1. It reports false until there is
            // an economy figure at all, which is what hides the ring on a
            // stationary car rather than drawing a green circle for a drive
            // that has not started.
            { [](const Model&) { return 0.0; }, [](const Model&) { return 1.0; }, nullptr,
              [](const Model& m, double* o) {
                  if (!m.trip.econ_km_per_l()) return false;
                  *o = 1.0;
                  return true; },
              [](const Model& m, double) { return econ_colour(m); } },
            {
                {"TIME",  [](const Model& m) {
                    int s = static_cast<int>(m.trip.elapsed_s);
                    char b[24]; snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
                    return std::string(b); }},
                // Average, not instant: this view is what the drive came to.
                // The live km/L reading was FUEL ECONOMY's hero and is the one
                // thing the merge drops -- the driving score is where live
                // coaching belongs.
                // The number takes the ring's colour, from the same function
                // and through the same fade, so the two read as one thing.
                {"KM/L",  [](const Model& m) { return num(m.trip.econ_km_per_l(), "%.1f"); },
                 econ_colour},
                {"FUEL",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.2f L", m.trip.fuel_l); return std::string(b); }},
                {"COST",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "RM%.2f", m.trip.cost_rm()); return std::string(b); }},
            },
        },
        // 5 --- Power. Derived in gauge_core from torque % x reference torque
        // x rpm, so the firmware and the simulator agree to the last digit.
        //
        // This view absorbed ELECTRICAL on 2026-08-29. The voltage bar became
        // the third line, carrying its own word with it -- "13.4v CHARGING"
        // says what the bar said, in the space a row already had.
        //
        // The channel list grew with it: a car that reports voltage but not
        // torque still has something to show here, and losing the volts line
        // to a missing torque PID would be the merge taking away a reading
        // that used to have a view of its own.
        {
            // No title, for the tacho's reason: the dial's own kW numbering
            // runs where the banner would sit, and a dial reading in kW does
            // not need the word POWER over it.
            nullptr,
            Instrument::Power,
            Layout::Rows,
            {"act_torque,ref_torque,ctrl_volt,volts", "NO TORQUE DATA",
             "power needs actual + reference torque, which this car does not report"},
            [](const Model& m) { return chan(m, "power_kw", "%.0f"); },
            nullptr,
            // The unit slot carries the peak, as the simulator does: a number
            // you are chasing belongs next to the one you are making.
            // Dashes, like every other view's absent reading. "no torque data"
            // explained itself where the peak goes, which is a sentence on an
            // instrument -- the tacho just shows dashes and is understood.
            [](const Model& m, uint32_t* colour) -> std::string {
                *colour = 0x9A9A9A;
                char b[32];
                if (!m.st.get("power_kw")) return "kW \xE2\x80\xA2 peak --";
                snprintf(b, sizeof b, "kW \xE2\x80\xA2 peak %.0f", m.st.peak_kw());
                return b;
            },
            { [](const Model&) { return 0.0; },
              [](const Model& m) { return m.id.power_max; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = m.st.get("power_kw"); if (!v) return false; *o = *v; return true; } },
            {
                {"RPM",  [](const Model& m) { return chan(m, "rpm", "%.0f"); }},
                {"LOAD", [](const Model& m) { return chan(m, "load", "%.0f%%"); }},
                // The state word travels with the number, because a row has no
                // colour of its own to say it with. Same thresholds the bar
                // used: below 12.2 the battery is going flat, below 13.0 the
                // engine is not charging it.
                // No word: the colour is the word. Green is charging, amber
                // resting, red going flat -- the same thresholds the bar used,
                // in the space the number already occupies.
                {"VOLTS", [](const Model& m) { return num(volts(m), "%.1fv"); },
                 [](const Model& m) -> uint32_t {
                     auto v = volts(m);
                     if (!v)        return 0xD0D0D0;
                     if (*v < 12.2) return 0xFF6B4A;
                     if (*v < 13.0) return 0xFFC24A;
                     return 0x5BD97A; }},
                {nullptr, nullptr},
            },
            "POWER",
        },
        // 6 --- Drives. The one view that shows the past rather than the
        // present: what the recorder wrote to flash, read back with no Mac in
        // the car. It has no hero, no dial and no rows, because none of those
        // come from the Model -- see Layout::Drives.
        //
        // No `avail` entry either. Every other view can be made useless by a
        // car that does not report its channel; this one is about drives on
        // flash, so an empty list is a real answer and says so itself.
        {
            "DRIVES",
            Instrument::None,
            Layout::Drives,
            {nullptr, nullptr, nullptr},
            nullptr,
            nullptr,
            nullptr,
            { nullptr, nullptr, nullptr, nullptr },
            { {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr} },
        },
        // 7 --- Clock. Last, which on a wrapping carousel is one swipe left
        // from the tacho. The only view you give something to rather than
        // read something from.
        //
        // It exists because this board has no real-time clock -- 0x51 is named
        // in the I2C table but is not on the bus -- so the only clock the gauge
        // has is one somebody handed it. Until now that meant a Mac on the USB
        // socket, and a drive recorded without one is stamped "date unknown"
        // for ever. That had happened to every drive recorded so far.
        //
        // No `avail` entry: the clock has nothing to do with what the car
        // reports, so there is no channel whose absence should hide it.
        {
            nullptr,                       // no title: the wheels are the view
            Instrument::None,
            Layout::Clock,
            {nullptr, nullptr, nullptr},
            nullptr,
            nullptr,
            nullptr,
            { nullptr, nullptr, nullptr, nullptr },
            { {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr} },
            "CLOCK",                       // ...but the log still needs a name
        },
    };
    *count = static_cast<int>(sizeof views / sizeof views[0]);
    return views;
}

}  // namespace gauge_ui
