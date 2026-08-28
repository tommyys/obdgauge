// The needle ease. Every property here is one the instruments rely on:
// the tacho's needle and its shutter arc are two Ease objects that must not
// disagree, and the ease must not change speed when the frame rate does.
#include <cmath>

#include "check.h"
#include "ease.h"

using gauge::Ease;
using gauge_test::check;

namespace {

constexpr double kTau = 0.12;   // the firmware's default, in seconds
constexpr double kFrame = 1.0 / 60.0;

bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
    // --- the first reading is taken whole ----------------------------------
    // A gauge that eases into its first reading sweeps up from zero on every
    // connect, which reads as the engine revving.
    {
        Ease e;
        check("first reading snaps", e.step(3000, kFrame, kTau), 3000.0);
        check("first reading primes", e.primed, true);
    }

    // --- easing off is the old behaviour ------------------------------------
    {
        Ease e;
        e.step(1000, kFrame, kTau);
        check("tau 0 jumps", e.step(4000, kFrame, 0.0), 4000.0);
        check("tau negative jumps", e.step(1500, kFrame, -1.0), 1500.0);
    }

    // --- a frame that took no time moves nothing ---------------------------
    {
        Ease e;
        e.step(1000, kFrame, kTau);
        check("dt 0 holds", e.step(4000, 0.0, kTau), 1000.0);
    }

    // --- it approaches, and never overshoots -------------------------------
    {
        Ease e;
        e.step(1000, kFrame, kTau);
        double prev = 1000.0;
        bool monotonic = true, inside = true;
        for (int i = 0; i < 200; ++i) {
            const double v = e.step(4000, kFrame, kTau);
            if (v < prev - 1e-9) monotonic = false;
            if (v > 4000.0 + 1e-9) inside = false;
            prev = v;
        }
        check("climbs without stepping back", monotonic, true);
        check("never passes the target", inside, true);
        check("arrives within a rev", close(prev, 4000.0, 1.0), true);
    }

    // --- one 100 ms frame lands where ten 10 ms frames do ------------------
    // The property that makes the ease frame-rate independent. Without it the
    // needle would arrive sooner on a busy screen than on an idle one.
    {
        Ease slow, fast;
        slow.step(0, 0.1, kTau);
        fast.step(0, 0.01, kTau);
        const double a = slow.step(5000, 0.1, kTau);
        double b = 0.0;
        for (int i = 0; i < 10; ++i) b = fast.step(5000, 0.01, kTau);
        check("100ms in one step == ten 10ms steps", close(a, b, 1e-9), true);
    }

    // --- two instruments fed the same thing stay identical -----------------
    // The tacho's needle and the shutter that hides its unlit heat band are
    // separate Ease objects. A disagreement of one step puts a red hairline
    // at the seam between them.
    {
        Ease needle, shutter;
        bool same = true;
        const double targets[] = {800, 3200, 3210, 6900, 900, 900, 4000};
        for (double t : targets) {
            for (int i = 0; i < 5; ++i) {
                const double n = needle.step(t, kFrame, kTau);
                const double s = shutter.step(t, kFrame, kTau);
                if (n != s) same = false;
            }
        }
        check("needle and shutter agree exactly", same, true);
    }

    // --- a channel that goes away and comes back does not sweep ------------
    {
        Ease e;
        e.step(6500, kFrame, kTau);
        e.reset();
        check("after reset the next reading snaps", e.step(800, kFrame, kTau), 800.0);
    }

    // --- a long stall does not leave the needle stranded --------------------
    // A frame that took a second (a flash write, a BLE reconnect) must land on
    // the reading rather than a third of the way to it.
    {
        Ease e;
        e.step(1000, kFrame, kTau);
        check("a 1 s frame arrives", close(e.step(4000, 1.0, kTau), 4000.0, 1.0), true);
    }

    return gauge_test::check_report();
}
