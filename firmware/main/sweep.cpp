#include "sweep.h"

#include "esp_timer.h"

namespace {

int64_t g_end_us = 0;
int64_t g_t0_us  = 0;
double  g_lo = 0, g_hi = 0;

// Ten seconds end to end. Slow enough to see a seam or a colour step as the
// needle passes it, quick enough that a whole up-and-down fits in the time
// somebody will stand and watch.
constexpr double kTravelS = 10.0;

}  // namespace

void sweep_start(double seconds, double lo, double hi) {
    g_t0_us  = esp_timer_get_time();
    g_end_us = g_t0_us + static_cast<int64_t>(seconds * 1e6);
    g_lo = lo;
    g_hi = hi;
}

bool sweep_rpm(double* out) {
    if (!g_end_us) return false;
    const int64_t now = esp_timer_get_time();
    if (now > g_end_us) { g_end_us = 0; return false; }

    const double t = (now - g_t0_us) / 1e6;
    // Triangle: up over kTravelS, down over the next kTravelS.
    double phase = t / kTravelS;
    phase -= static_cast<int>(phase / 2) * 2;          // fold to [0,2)
    const double f = (phase <= 1.0) ? phase : 2.0 - phase;
    *out = g_lo + (g_hi - g_lo) * f;
    return true;
}
