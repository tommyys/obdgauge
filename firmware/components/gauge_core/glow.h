// The rpm-reactive backdrop, shared by the board and the simulator.
//
// The display warms up as the engine does: near-black at idle, a dim ember
// through the mid-range, an intense red vignette approaching the redline
// (SPEC.md section 4). The ramp lives here, in core, for the same reason
// power_kw does -- the firmware and the simulator have to agree on what a
// given rpm looks like, and two copies of a curve drift.
//
// The shape of the gradient is a rendering decision and stays in the UI. What
// core owns is the single number behind it: how hot, from 0 to 1.
#pragma once
#include <cstdint>
#include <optional>

namespace gauge {

// Everything the backdrop needs for one rpm reading.
struct Glow {
    // Quantisation of the ramp, 0..kGlowSteps. The board repaints only when
    // this changes: the glow is a full-screen object, so a colour it writes
    // every frame costs the whole panel. The simulator quantises the gradient
    // string on the same grid for the same reason (rebuilding it is not free
    // there either), which is why the constant is shared rather than tuned
    // twice.
    int step;
    uint32_t colour;   // 0xRRGGBB
    uint8_t  opa;      // 0..255
};

// Colour endpoints and the ramp's shape. Named so a test can state the
// expected value in the same terms the code computes it.
constexpr int kGlowSteps = 40;
constexpr uint32_t kGlowEmber = 0x681A0C;   // low intensity, deep ember
constexpr uint32_t kGlowRed   = 0xFF2216;   // full intensity, hot red
constexpr double kGlowStart = 0.22;         // fraction of redline before any tint
// Fraction of redline at which the glow is already fully hot. Tying full
// intensity to the redline itself meant the top of the ramp was somewhere you
// essentially never go on a road -- 5000 rpm in an MX-5 read as a little over
// half brightness, so the effect spent its life in the dull half. 0.72 of a
// 7000 redline is 5040 rpm: reachable in second on any decent road, and the
// ramp below it still has the whole usable range to play with. A fraction, so
// a car with a different redline scales with it.
constexpr double kGlowFull  = 0.72;
constexpr double kGlowGamma = 1.35;         // hold back the mid-range, bite near the top
constexpr double kGlowMaxOpa = 0.92;        // never fully opaque; the rim keeps some depth

// How hot, 0 to 1, with no quantisation. This is the curve itself; Glow is the
// curve rounded off for something that has to repaint when it moves.
//
// Anything STATIC wants this one. The tacho's rim band is coloured once at
// startup and never changes, so it has no repaint to save and no reason to
// step: it reads the ramp at full precision and the gradient comes out smooth.
//
// No reading, or a car with no known redline, means no heat at all -- the dial
// degrades to cold rather than to a plausible-looking ember (SPEC.md 4).
double glow_heat(std::optional<double> rpm, double rpm_red);

// The same curve, rounded to kGlowSteps. For anything that must repaint when
// it changes, and so must not change more often than it has to.
Glow glow_for(std::optional<double> rpm, double rpm_red);

}  // namespace gauge
