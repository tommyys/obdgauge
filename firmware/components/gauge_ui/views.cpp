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
            Layout::Grid,
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
                {"INTAKE AIR", [](const Model& m) { return chan(m, "intake", "%.0f\xC2\xB0"); }},
                {"CATALYST",   [](const Model& m) {
                    auto c = m.st.get("catalyst");
                    if (!c) c = m.st.get("cat_b1s1");
                    return num(c, "%.0f\xC2\xB0"); }},
                {"BATT",       [](const Model& m) { return num(volts(m), "%.1fv"); }},
                {nullptr, nullptr},
            },
        },
        // 3 --- Fuel economy. Quoted in km/L, which is the unit the display
        // uses; the score keeps working in L/100km where its band is tuned.
        {
            "FUEL ECONOMY",
            Instrument::None,
            Layout::Rows,
            {"fuel_rate", "NO FUEL RATE",
             "this car does not report engine fuel rate over OBD"},
            [](const Model& m) {
                return num(gauge::km_per_l(gauge::instant_econ(m.st.get("speed"),
                                                               m.st.get("fuel_rate"))),
                           "%.1f");
            },
            "KM/L \xE2\x80\xA2 NOW",
            nullptr,
            { nullptr, nullptr, nullptr, nullptr },
            {
                {"avg",  [](const Model& m) { return num(m.trip.econ_km_per_l(), "%.1f"); }},
                {"L/h",  [](const Model& m) { return chan(m, "fuel_rate", "%.1f"); }},
                {"USED", [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.2f L", m.trip.fuel_l); return std::string(b); }},
                {"COST", [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "RM %.2f", m.trip.cost_rm()); return std::string(b); }},
            },
        },
        // 4 --- Driving score. The weights are still untuned guesses (B3), so
        // the coach word sits in the unit slot rather than the number alone.
        // The ring is on the rim, where the tacho's is. The simulator insets
        // it to 68% because it has a whole square viewport to spend; on a round
        // panel that diameter runs straight through the rows underneath.
        {
            "DRIVING",
            Instrument::ScoreRing,
            Layout::Rows,
            {"rpm,speed,throttle", "NO DRIVE DATA",
             "scoring needs rpm, speed or throttle"},
            [](const Model& m) { return num(m.score.total(), "%.0f"); },
            nullptr,
            [](const Model& m, uint32_t* colour) -> std::string {
                auto t = m.score.total();
                if (!t)        { *colour = 0x808080; return m.score.coach(); }
                if (*t >= 85)  { *colour = 0x5BD97A; return m.score.coach(); }
                if (*t >= 70)  { *colour = 0xFFC24A; return m.score.coach(); }
                *colour = 0xFF6B4A; return m.score.coach();
            },
            { [](const Model&) { return 0.0; }, [](const Model&) { return 100.0; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = m.score.total(); if (!v) return false; *o = *v; return true; } },
            {
                {"sm",   [](const Model& m) { return num(m.score.smooth(), "%.0f"); }},
                {"eco",  [](const Model& m) { return num(m.score.econ(), "%.0f"); }},
                {"calm", [](const Model& m) { return num(m.score.calm(), "%.0f"); }},
                {nullptr, nullptr},
            },
        },
        // 5 --- Trip. Four totals in a grid: they are read one at a time when
        // you glance down, not scanned as a list.
        {
            "TRIP",
            Instrument::None,
            Layout::Grid,
            {"speed", "NO SPEED", "this car does not report vehicle speed"},
            [](const Model& m) {
                char b[24]; snprintf(b, sizeof b, "%.1f", m.trip.dist_km); return std::string(b); },
            "KM",
            nullptr,
            { nullptr, nullptr, nullptr, nullptr },
            {
                {"TIME",  [](const Model& m) {
                    int s = static_cast<int>(m.trip.elapsed_s);
                    char b[24]; snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
                    return std::string(b); }},
                {"AVG KM/H", [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.0f", m.trip.avg_speed_kph());
                    return std::string(b); }},
                {"FUEL",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.2f L", m.trip.fuel_l); return std::string(b); }},
                {"COST",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "RM%.2f", m.trip.cost_rm()); return std::string(b); }},
            },
        },
        // 6 --- Power. Derived in gauge_core from torque % x reference torque
        // x rpm, so the firmware and the simulator agree to the last digit.
        {
            "POWER",
            Instrument::Power,
            Layout::Rows,
            {"act_torque,ref_torque", "NO TORQUE DATA",
             "power needs actual + reference torque, which this car does not report"},
            [](const Model& m) { return chan(m, "power_kw", "%.0f"); },
            nullptr,
            // The unit slot carries the peak, as the simulator does: a number
            // you are chasing belongs next to the one you are making.
            [](const Model& m, uint32_t* colour) -> std::string {
                *colour = 0x9A9A9A;
                if (!m.st.get("power_kw")) return "no torque data";
                char b[32];
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
                {nullptr, nullptr},
                {nullptr, nullptr},
            },
        },
        // 7 --- Electrical. A bar rather than a rim arc: charging is read as a
        // position between "flat" and "overcharging", which a line with two
        // ends says better than a ring that meets itself.
        {
            "ELECTRICAL",
            Instrument::Bar,
            Layout::Rows,
            {"ctrl_volt,volts", "NO VOLTAGE",
             "no control-module voltage from this car or adapter"},
            [](const Model& m) { return num(volts(m), "%.1f"); },
            "V",
            [](const Model& m, uint32_t* colour) -> std::string {
                auto v = volts(m);
                if (!v)         { *colour = 0x808080; return ""; }
                if (*v < 12.2)  { *colour = 0xFF6B4A; return "LOW"; }
                if (*v < 13.0)  { *colour = 0xFFC24A; return "RESTING"; }
                *colour = 0x5BD97A; return "CHARGING";
            },
            { nullptr, nullptr, nullptr, nullptr },
            { {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr} },
        },

        // 8 --- Drives. The one view that shows the past rather than the
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
    };
    *count = static_cast<int>(sizeof views / sizeof views[0]);
    return views;
}

}  // namespace gauge_ui
