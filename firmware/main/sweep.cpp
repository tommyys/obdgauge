#include "sweep.h"

#include "esp_timer.h"

namespace {

// One sweeper per thing being swept. There are two: the tacho's rpm and the
// trip ring's economy, and they have to be able to run independently.
struct Sweep {
    int64_t end_us = 0;
    int64_t t0_us  = 0;
    double  lo = 0, hi = 0;
};
Sweep g_rpm;
Sweep g_econ;

// Ten seconds end to end. Slow enough to see a seam or a colour step as the
// needle passes it, quick enough that a whole up-and-down fits in the time
// somebody will stand and watch.
constexpr double kTravelS = 10.0;

void start(Sweep& s, double seconds, double lo, double hi) {
    s.t0_us  = esp_timer_get_time();
    s.end_us = s.t0_us + static_cast<int64_t>(seconds * 1e6);
    s.lo = lo;
    s.hi = hi;
}

bool read(Sweep& s, double* out) {
    if (!s.end_us) return false;
    const int64_t now = esp_timer_get_time();
    if (now > s.end_us) { s.end_us = 0; return false; }

    const double t = (now - s.t0_us) / 1e6;
    // Triangle: up over kTravelS, down over the next kTravelS.
    double phase = t / kTravelS;
    phase -= static_cast<int>(phase / 2) * 2;          // fold to [0,2)
    const double f = (phase <= 1.0) ? phase : 2.0 - phase;
    *out = s.lo + (s.hi - s.lo) * f;
    return true;
}

}  // namespace

bool g_demo = false;

void demo_request(void) { g_demo = true; }
bool demo_wanted(void)  { return g_demo; }

void sweep_start(double seconds, double lo, double hi) { start(g_rpm, seconds, lo, hi); }
bool sweep_rpm(double* out)                            { return read(g_rpm, out); }

void sweep_econ_start(double seconds, double lo, double hi) {
    start(g_econ, seconds, lo, hi);
}
bool sweep_econ(double* out) { return read(g_econ, out); }

