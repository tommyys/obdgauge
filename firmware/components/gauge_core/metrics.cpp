#include "metrics.h"
#include <algorithm>
#include <cmath>

namespace gauge {
namespace {
double clamp100(double v) { return std::max(0.0, std::min(100.0, v)); }
double round2(double v) { return std::round(v * 100.0) / 100.0; }
}  // namespace

Opt instant_econ(Opt speed_kph, Opt fuel_rate_lph) {
    if (!speed_kph || !fuel_rate_lph) return std::nullopt;
    if (*speed_kph < 3.0) return std::nullopt;
    return *fuel_rate_lph / *speed_kph * 100.0;
}

Opt km_per_l(Opt l_per_100km) {
    if (!l_per_100km || *l_per_100km <= 0) return std::nullopt;
    return 100.0 / *l_per_100km;
}

// --- Trip -----------------------------------------------------------------

void Trip::update(double t, Opt speed_kph, Opt fuel_rate_lph) {
    if (!last_t_) {
        last_t_ = t; last_speed_ = speed_kph; last_rate_ = fuel_rate_lph;
        return;
    }
    double dt = t - *last_t_;
    // ignore pauses/gaps - they'd otherwise integrate garbage
    if (dt <= 0 || dt > 5.0) {
        last_t_ = t; last_speed_ = speed_kph; last_rate_ = fuel_rate_lph;
        return;
    }
    elapsed_s += dt;
    if (speed_kph && last_speed_) {
        double avg = (*speed_kph + *last_speed_) / 2.0;
        dist_km += avg * dt / 3600.0;
        if (avg > 1.0) moving_s += dt;
    }
    if (fuel_rate_lph && last_rate_) {
        fuel_l += (*fuel_rate_lph + *last_rate_) / 2.0 * dt / 3600.0;
    }
    last_t_ = t;
    if (speed_kph) last_speed_ = speed_kph;
    if (fuel_rate_lph) last_rate_ = fuel_rate_lph;
}

Opt Trip::econ_l_per_100() const {
    if (dist_km < 0.2) return std::nullopt;
    return fuel_l / dist_km * 100.0;
}

double Trip::avg_speed_kph() const {
    if (moving_s < 5) return 0.0;
    return dist_km / (moving_s / 3600.0);
}

// --- DrivingScore ---------------------------------------------------------

void DrivingScore::rebase(double t, Opt speed_kph, Opt throttle_pct) {
    last_t_ = t;
    thr_ = throttle_pct; thr_t_ = t;
    spd_ = speed_kph;    spd_t_ = t;
}

void DrivingScore::update(double t, Opt speed_kph, Opt rpm, Opt throttle_pct,
                          Opt fuel_rate_lph) {
    if (!last_t_) { rebase(t, speed_kph, throttle_pct); return; }
    double dt = t - *last_t_;
    // Two channels sampled in the same millisecond - a tie, not a gap.
    // Rebasing here dragged the per-channel baselines forward while their
    // values stayed put, shrinking the next real delta's dt.
    if (dt == 0) return;
    if (dt < 0 || dt > 5.0) { rebase(t, speed_kph, throttle_pct); return; }
    last_t_ = t;

    // Throttle movement per second: total travel over total time, so holding
    // a steady throttle contributes time but no travel at any sample rate.
    if (throttle_pct) {
        if (!thr_) { thr_ = throttle_pct; thr_t_ = t; }
        else {
            double d = t - *thr_t_;
            if (d > 0) {
                thr_travel += std::fabs(*throttle_pct - *thr_);
                thr_seconds += d;
                thr_ = throttle_pct; thr_t_ = t;
            }
        }
    }

    // time in the efficient rev band - integrating time, so the every-sample
    // dt is the right one here
    if (rpm && *rpm > 400) {
        if (kEcoRpmLo <= *rpm && *rpm <= kEcoRpmHi) eco_s += dt;
        rev_s += dt;
    }

    // economy, weighted by time so a densely-sampled channel cannot outvote
    // a sparse one
    Opt ie = instant_econ(speed_kph, fuel_rate_lph);
    if (ie && *ie < 40) { econ_sum += *ie * dt; econ_s += dt; }

    // Harsh accel / braking, from one speed reading to the next actual change
    // - never from a stale value against a fresh timestamp.
    if (speed_kph) {
        if (!spd_) { spd_ = speed_kph; spd_t_ = t; }
        else if (*speed_kph != *spd_) {
            double d = t - *spd_t_;
            if (d > 0) {
                double a = (*speed_kph - *spd_) / 3.6 / d;
                if (a > kHarshAccel)      { ++harsh; events.push_back({t, "accel", round2(a)}); }
                else if (a < kHarshBrake) { ++harsh; events.push_back({t, "brake", round2(a)}); }
            }
            spd_ = speed_kph; spd_t_ = t;
        }
    }
}

Opt DrivingScore::smooth() const {
    if (thr_seconds < 5) return std::nullopt;
    double rate = thr_travel / thr_seconds;     // %/s of throttle movement
    return clamp100(100.0 - rate * 8.0);        // 0 %/s -> 100 ; 12 %/s -> 0
}

Opt DrivingScore::econ() const {
    std::vector<double> parts;
    if (rev_s > 5) parts.push_back(eco_s / rev_s * 100.0);
    if (econ_s > 5) {
        double avg = econ_sum / econ_s;         // L/100km, time-weighted
        parts.push_back(clamp100((15.0 - avg) * 10.0));  // 5 -> 100 ; 15 -> 0
    }
    if (parts.empty()) return std::nullopt;
    double sum = 0.0;
    for (double p : parts) sum += p;
    return sum / static_cast<double>(parts.size());
}

Opt DrivingScore::calm() const {
    double mins = std::max(rev_s, 1.0) / 60.0;
    double rate = static_cast<double>(harsh) / mins;   // events per minute
    return clamp100(100.0 - rate * 25.0);
}

Opt DrivingScore::total() const {
    const std::pair<Opt, double> subs[] = {
        {smooth(), kWSmooth}, {econ(), kWEcon}, {calm(), kWCalm}};
    double wsum = 0.0, acc = 0.0;
    bool any = false;
    for (const auto& s : subs) {
        if (!s.first) continue;
        any = true;
        wsum += s.second;
        acc += *s.first * s.second;
    }
    if (!any) return std::nullopt;
    return acc / wsum;
}

std::string DrivingScore::coach() const {
    Opt t = total();
    if (!t) return "WARMING UP";
    if (*t >= 85) return "SMOOTH";
    if (*t >= 70) return "GOOD";
    // Python takes min() over (value, label) tuples: lowest score wins, and
    // the label breaks a tie alphabetically.
    const std::pair<Opt, std::string> subs[] = {
        {smooth(), "JERKY"}, {econ(), "THIRSTY"}, {calm(), "HARSH"}};
    bool have = false;
    double best_v = 0.0;
    std::string best_l;
    for (const auto& s : subs) {
        if (!s.first) continue;
        if (!have || *s.first < best_v || (*s.first == best_v && s.second < best_l)) {
            best_v = *s.first; best_l = s.second; have = true;
        }
    }
    return have ? best_l : "OK";
}

}  // namespace gauge
