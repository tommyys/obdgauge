#include "ease.h"

#include <cmath>

namespace gauge {

double Ease::step(double target, double dt_s, double tau_s) {
    // The first reading is taken whole. Easing into it would sweep the needle
    // up from zero every time the car connects, which reads as the engine
    // revving rather than as the gauge waking up.
    if (!primed) {
        primed = true;
        value  = target;
        return value;
    }
    // A frame that took no time moves nothing; easing off jumps.
    if (!(dt_s > 0.0) || !(tau_s > 0.0)) {
        if (!(tau_s > 0.0)) value = target;
        return value;
    }
    // Exponential approach, sampled at dt. Written this way rather than as a
    // fixed fraction per frame so that the needle takes the same time to
    // arrive whether the frame rate is 20 or 60 -- a fraction-per-frame ease
    // gets faster as the gauge gets busier, which is exactly backwards.
    const double k = 1.0 - std::exp(-dt_s / tau_s);
    value += (target - value) * k;
    return value;
}

}  // namespace gauge
