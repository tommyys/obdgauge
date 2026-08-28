// Needle easing: the bridge between how often the car answers and how often
// the panel can draw.
//
// The car reports rpm perhaps eight times a second over the BLE adapter, and
// every reading was written straight to the needle. So the needle stepped
// eight times a second no matter how fast the panel was -- which is the
// jerkiness, and it is not a drawing cost. Between two readings the gauge had
// nothing to say and drew nothing.
//
// This gives it something to say: the drawn value chases the reported one
// instead of jumping to it, so the frames between two readings each move the
// needle a little. The eye reads continuous motion; the reading underneath is
// unchanged.
#pragma once

namespace gauge {

// One instrument's drawn value as it chases its reading.
//
// The state is deliberately tiny and copyable: the tacho's needle and the
// shutter arc that hides the unlit part of its heat band keep SEPARATE Ease
// objects, and they must agree to the last bit or the redline shows as a
// hairline at the seam (see face.h on kRimShutterPx). They do agree, because
// both are stepped once per frame with the same target, the same dt and the
// same tau, and both take their first reading whole -- identical inputs to
// identical arithmetic.
struct Ease {
    double value  = 0.0;
    bool   primed = false;

    // Advance toward `target` and return the value to draw.
    //
    // tau_s is the time for the remaining gap to close to 1/e -- about a
    // third. tau_s <= 0 turns easing off: the value jumps, which is what this
    // gauge did before and what the EASE 0 console command restores for a
    // side-by-side on the bench.
    double step(double target, double dt_s, double tau_s);

    // Forget the reading. Used when a channel goes away, so that the needle
    // does not sweep across the dial from a stale value when it comes back.
    void reset() { primed = false; }
};

}  // namespace gauge
