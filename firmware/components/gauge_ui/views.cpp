// What each view shows. SPEC.md section 6 lists nine; eight are here.
//
// View 9 (Drives) is deliberately absent rather than present-and-empty: it
// browses a library of recorded drives, and nothing on the board records yet.
// Section 4's rule is that a view whose channels are all missing says so
// outright rather than drawing an empty dial -- and the honest version of that
// for a whole feature is not to ship the view until it has data.
#include "views.h"
#include <cstdio>
#include <string>

namespace gauge_ui {
namespace {

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
        // 1 --- Tacho. Returned to the design because live rpm is the clearest
        // confirmation the link is working (SPEC.md section 4).
        {
            "TACHO",
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
        // car does not report oil temperature.
        {
            "ENGINE",
            [](const Model& m) { return chan(m, "coolant", "%.0f"); },
            "\xC2\xB0" "C",
            [](const Model& m, uint32_t* colour) -> std::string {
                auto c = m.st.get("coolant");
                if (!c) { *colour = 0x808080; return ""; }
                if (*c < 60)  { *colour = 0x4FA3FF; return "COLD"; }
                if (*c < 85)  { *colour = 0xFFC24A; return "WARMING"; }
                *colour = 0x5BD97A; return "READY";
            },
            { [](const Model&) { return 40.0; }, [](const Model&) { return 110.0; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = m.st.get("coolant"); if (!v) return false; *o = *v; return true; } },
            {
                {"INTAKE", [](const Model& m) { return chan(m, "intake", "%.0f\xC2\xB0"); }},
                {"BATT",   [](const Model& m) { return num(volts(m), "%.1fV"); }},
                {"LOAD",   [](const Model& m) { return chan(m, "load", "%.0f%%"); }},
                {nullptr, nullptr},
            },
        },
        // 3 --- Fuel economy. Quoted in km/L, which is the unit the display
        // uses; the score keeps working in L/100km where its band is tuned.
        {
            "ECONOMY",
            [](const Model& m) {
                return num(gauge::km_per_l(gauge::instant_econ(m.st.get("speed"),
                                                               m.st.get("fuel_rate"))),
                           "%.1f");
            },
            "KM/L",
            nullptr,
            { nullptr, nullptr, nullptr, nullptr },
            {
                {"TRIP",  [](const Model& m) { return num(m.trip.econ_km_per_l(), "%.1f km/L"); }},
                {"L/H",   [](const Model& m) { return chan(m, "fuel_rate", "%.1f"); }},
                {"USED",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.2f L", m.trip.fuel_l); return std::string(b); }},
                {"COST",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "RM %.2f", m.trip.cost_rm()); return std::string(b); }},
            },
        },
        // 4 --- Driving score. The weights are still untuned guesses (B3), so
        // the coach word is shown alongside rather than the number alone.
        {
            "SCORE",
            [](const Model& m) { return num(m.score.total(), "%.0f"); },
            "/100",
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
                {"SMOOTH", [](const Model& m) { return num(m.score.smooth(), "%.0f"); }},
                {"ECON",   [](const Model& m) { return num(m.score.econ(), "%.0f"); }},
                {"CALM",   [](const Model& m) { return num(m.score.calm(), "%.0f"); }},
                {"HARSH",  [](const Model& m) {
                    char b[16]; snprintf(b, sizeof b, "%d", m.score.harsh); return std::string(b); }},
            },
        },
        // 5 --- Trip.
        {
            "TRIP",
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
                {"AVG",   [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.0f km/h", m.trip.avg_speed_kph());
                    return std::string(b); }},
                {"FUEL",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.2f L", m.trip.fuel_l); return std::string(b); }},
                {"COST",  [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "RM %.2f", m.trip.cost_rm()); return std::string(b); }},
            },
        },
        // 6 --- Power. Derived in gauge_core from torque % x reference torque
        // x rpm, so the firmware and the simulator agree to the last digit.
        {
            "POWER",
            [](const Model& m) { return chan(m, "power_kw", "%.0f"); },
            "KW",
            nullptr,
            { [](const Model&) { return 0.0; },
              [](const Model& m) { return m.id.power_max; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = m.st.get("power_kw"); if (!v) return false; *o = *v; return true; } },
            {
                {"PEAK",   [](const Model& m) {
                    char b[24]; snprintf(b, sizeof b, "%.0f kW", m.st.peak_kw()); return std::string(b); }},
                {"TORQUE", [](const Model& m) { return chan(m, "act_torque", "%.0f%%"); }},
                {"REF",    [](const Model& m) { return chan(m, "ref_torque", "%.0f Nm"); }},
                {"RPM",    [](const Model& m) { return chan(m, "rpm", "%.0f"); }},
            },
        },
        // 7 --- Thermals. Fuel rail temperature was dropped: across every
        // capture it returned 2 distinct values in 375 samples (section 6).
        {
            "THERMALS",
            [](const Model& m) { return chan(m, "coolant", "%.0f"); },
            "\xC2\xB0" "C",
            nullptr,
            { [](const Model&) { return 40.0; }, [](const Model&) { return 110.0; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = m.st.get("coolant"); if (!v) return false; *o = *v; return true; } },
            {
                {"INTAKE",   [](const Model& m) { return chan(m, "intake", "%.0f\xC2\xB0"); }},
                {"CATALYST", [](const Model& m) {
                    auto c = m.st.get("cat_b1s1");
                    if (!c) c = m.st.get("catalyst");
                    return num(c, "%.0f\xC2\xB0"); }},
                {"PEAK CAT", [](const Model& m) { return num(m.st.peak("catalyst"), "%.0f\xC2\xB0"); }},
                {"AMBIENT",  [](const Model& m) { return chan(m, "ambient", "%.0f\xC2\xB0"); }},
            },
        },
        // 8 --- Electrical.
        {
            "ELECTRICAL",
            [](const Model& m) { return num(volts(m), "%.1f"); },
            "VOLTS",
            [](const Model& m, uint32_t* colour) -> std::string {
                auto v = volts(m);
                if (!v)         { *colour = 0x808080; return ""; }
                if (*v < 12.2)  { *colour = 0xFF6B4A; return "LOW"; }
                if (*v < 13.0)  { *colour = 0xFFC24A; return "RESTING"; }
                *colour = 0x5BD97A; return "CHARGING";
            },
            { [](const Model&) { return 11.0; }, [](const Model&) { return 15.0; }, nullptr,
              [](const Model& m, double* o) {
                  auto v = volts(m); if (!v) return false; *o = *v; return true; } },
            {
                {"LOAD",  [](const Model& m) { return chan(m, "load", "%.0f%%"); }},
                {"RPM",   [](const Model& m) { return chan(m, "rpm", "%.0f"); }},
                {nullptr, nullptr},
                {nullptr, nullptr},
            },
        },
    };
    *count = static_cast<int>(sizeof views / sizeof views[0]);
    return views;
}

}  // namespace gauge_ui
