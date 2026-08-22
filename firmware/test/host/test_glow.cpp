// The rpm-reactive backdrop's ramp. The expected values here were taken from
// the simulator (mx5gauge/web/index.html, drawGlow) so the two cannot drift:
// if this file needs editing, the simulator changed and both have to move.
#include "check.h"
#include "glow.h"

using namespace gauge;
using gauge_test::check;

namespace {

constexpr double kRed = 7000;   // the MX-5 profile's redline

void check_glow(const char* name, std::optional<double> rpm, double rpm_red,
                int want_step, uint32_t want_colour, int want_opa) {
    Glow g = glow_for(rpm, rpm_red);
    check((std::string(name) + " step").c_str(), g.step, want_step);
    check((std::string(name) + " colour").c_str(), (int)g.colour, (int)want_colour);
    check((std::string(name) + " opa").c_str(), (int)g.opa, want_opa);
}

}  // namespace

int main() {
    // --- the dark end: nothing below the start of the ramp ------------------
    // No rpm at all is not a dark ember, it is no backdrop.
    check_glow("no reading", std::nullopt, kRed, 0, kGlowEmber, 0);
    check_glow("idle 800",   800.0,        kRed, 0, kGlowEmber, 0);
    // 0.22 * 7000 is exactly where the ramp starts, so it is still black.
    check_glow("at start 1540", 1540.0,    kRed, 0, kGlowEmber, 0);

    // --- the ramp -----------------------------------------------------------
    check_glow("2000",  2000.0, kRed,  3, 0x731B0D,  18);
    check_glow("3500",  3500.0, kRed, 18, 0xAC1E11, 106);

    // --- the hot end: full before the redline, and clamped past it ----------
    // 0.72 * 7000 = 5040. Fully hot here, not at the redline, which is the
    // whole point of kGlowFull.
    check_glow("full 5040", 5040.0, kRed, 40, kGlowRed, 235);
    check_glow("redline 7000", 7000.0, kRed, 40, kGlowRed, 235);
    check_glow("over 9000",    9000.0, kRed, 40, kGlowRed, 235);

    // --- a car we know nothing about --------------------------------------
    check_glow("no redline", 4000.0, 0.0, 0, kGlowEmber, 0);

    // --- the continuous curve, which the static rim band reads -------------
    // Quantisation is a concession to repainting; a band that never repaints
    // must not inherit it, or the gradient comes out in visible steps.
    check("heat: no reading", gauge_test::show(glow_heat(std::nullopt, kRed)),
          std::string("0"));
    check("heat: below the ramp", glow_heat(800.0, kRed) == 0.0, true);
    check("heat: full at 5040",   glow_heat(5040.0, kRed) == 1.0, true);
    check("heat: clamped past redline", glow_heat(9000.0, kRed) == 1.0, true);
    // The two agree: Glow is glow_heat rounded, and nothing else.
    bool agrees = true, distinct = true;
    int seen = 0, last_q = -1;
    for (int rpm = 0; rpm <= 8000; rpm += 10) {
        const double h = glow_heat((double)rpm, kRed);
        const int q = (int)(h * kGlowSteps + 0.5);
        if (q != glow_for((double)rpm, kRed).step) agrees = false;
        if (q != last_q) { ++seen; last_q = q; }
    }
    check("heat: Glow is heat, rounded", agrees, true);
    // And the continuous curve really does have more to give than the steps do,
    // which is the whole reason the band reads it directly.
    distinct = seen <= kGlowSteps + 1;
    check("heat: the steps are the coarser of the two", distinct, true);

    // --- the ramp only ever climbs -----------------------------------------
    // A backdrop that dimmed while the engine picked up would read as a fault.
    int prev = -1;
    bool monotonic = true;
    for (int rpm = 0; rpm <= 8000; rpm += 50) {
        int s = glow_for((double)rpm, kRed).step;
        if (s < prev) monotonic = false;
        prev = s;
    }
    check("ramp never dips", monotonic, true);

    return gauge_test::check_report();
}
