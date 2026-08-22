#include "glow.h"
#include <cmath>

namespace gauge {
namespace {

// One channel of the ember-to-red mix, rounded the way the simulator's
// Math.round does it.
uint8_t mix_channel(uint32_t from, uint32_t to, int shift, double f) {
    double a = static_cast<double>((from >> shift) & 0xFF);
    double b = static_cast<double>((to   >> shift) & 0xFF);
    return static_cast<uint8_t>(std::floor(a + (b - a) * f + 0.5));
}

}  // namespace

double glow_heat(std::optional<double> rpm, double rpm_red) {
    if (!rpm || rpm_red <= 0) return 0.0;
    double f = (*rpm / rpm_red - kGlowStart) / (kGlowFull - kGlowStart);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return std::pow(f, kGlowGamma);
}

Glow glow_for(std::optional<double> rpm, double rpm_red) {
    const double f = glow_heat(rpm, rpm_red);

    Glow g{};
    g.step = static_cast<int>(std::floor(f * kGlowSteps + 0.5));
    // Colour and opacity both come off the quantised value rather than the
    // continuous one. On the board the two are equally expensive to change --
    // either repaints the whole backdrop -- so there is nothing to gain by
    // letting opacity move between steps, and a single `step` then says
    // everything about whether a repaint is needed.
    const double fq = static_cast<double>(g.step) / kGlowSteps;
    g.colour = (static_cast<uint32_t>(mix_channel(kGlowEmber, kGlowRed, 16, fq)) << 16) |
               (static_cast<uint32_t>(mix_channel(kGlowEmber, kGlowRed,  8, fq)) <<  8) |
               (static_cast<uint32_t>(mix_channel(kGlowEmber, kGlowRed,  0, fq)));
    g.opa = static_cast<uint8_t>(std::floor(fq * kGlowMaxOpa * 255.0 + 0.5));
    return g;
}

}  // namespace gauge
