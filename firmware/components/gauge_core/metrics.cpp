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

Opt DrivingScore::sub(double bad, double seconds) {
    // Under five seconds of observation the channel has not told us enough to
    // say anything, and an em-dash is more honest than a number.
    if (seconds < 5.0) return std::nullopt;
    return clamp100(100.0 * (1.0 - bad / seconds));
}

void DrivingScore::rebase(double t, Opt throttle_pct) {
    last_t_    = t;
    thr_       = throttle_pct;
    thr_t_     = t;
    thr_pivot_ = throttle_pct;
    thr_dir_   = 0;
    lon_.reset(); lat_.reset(); lon_t_.reset(); lat_t_.reset();
}

void DrivingScore::update(double t, Opt speed_kph, Opt rpm, Opt throttle_pct,
                          Opt /*fuel_rate_lph*/, Opt coolant_c) {
    g.speed(t, speed_kph);
    if (!last_t_) { rebase(t, throttle_pct); return; }
    double dt = t - *last_t_;
    // Two channels sampled in the same millisecond - a tie, not a gap.
    // Rebasing here dragged the per-channel baselines forward while their
    // values stayed put, shrinking the next real delta's dt.
    if (dt == 0) return;
    if (dt < 0 || dt > 5.0) { rebase(t, throttle_pct); return; }
    last_t_ = t;
    if (rpm && *rpm > 400) rev_s += dt;

    step_intensity(dt, rpm, throttle_pct);
    step_pole(t, dt);
    const bool spirited = pole == Pole::Spirited;
    step_throttle(t, throttle_pct, spirited);
    step_braking(t, dt, spirited);
    step_cornering(t, dt, spirited);
    step_care(dt, rpm, coolant_c);
}

// How hard the car is being driven, 0-1. Never scored: it only picks which
// yardstick the sub-scores are measured against.
void DrivingScore::step_intensity(double dt, Opt rpm, Opt throttle_pct) {
    double best = 0.0;
    bool   any  = false;
    auto   take = [&](double v) { if (!any || v > best) best = v; any = true; };
    if (rpm && rpm_red > 0) {
        double lo = rpm_red * kRpmCalmFrac, hi = rpm_red * kRpmHotFrac;
        take((*rpm - lo) / (hi - lo));
    }
    if (throttle_pct) take((*throttle_pct - kThrCalm) / (kThrHot - kThrCalm));
    if (g.ready()) take(g.total() / kGHot);
    if (!any) return;
    // The loudest signal wins. A car held at 6000 rpm through a long sweeper
    // is spirited at a steady throttle, and a car braked at 0.5 g is spirited
    // at any rpm -- averaging would hide both.
    double raw = std::max(0.0, std::min(1.0, best));
    double k   = 1.0 - std::exp(-dt / kIntensityTauS);
    intensity += (raw - intensity) * k;
}

void DrivingScore::step_pole(double t, double dt) {
    Pole want = pole;                    // in the gap, nothing changes
    if (intensity >= kSpiritedAbove)     want = Pole::Spirited;
    else if (intensity <= kNiceBelow)    want = Pole::Nice;
    if (want != cand_) { cand_ = want; cand_since_ = t; }
    if (want != pole && cand_since_ && t - *cand_since_ >= kPoleHoldS) {
        pole = want;
        events.push_back({round2(t), "pole",
                          pole == Pole::Spirited ? "SPIRITED" : "NICE", 0.0});
    }
    if (pole == Pole::Spirited) spirited_s += dt; else nice_s += dt;
}

void DrivingScore::step_throttle(double t, Opt throttle_pct, bool spirited) {
    if (!throttle_pct) return;
    if (!thr_) { thr_ = throttle_pct; thr_t_ = t; thr_pivot_ = throttle_pct; return; }
    double d = t - *thr_t_;
    if (d <= 0) return;
    double move = *throttle_pct - *thr_;
    thr_ = throttle_pct; thr_t_ = t;
    thr_s += d;
    // How fast the pedal is moving, against the band for this pole.
    double band = spirited ? kThrRateSpirited : kThrRateNice;
    thr_bad += std::max(0.0, std::min(1.0, std::fabs(move) / d / band)) * d;
    // A reversal: the pedal turned round by more than the deadband.
    if (std::fabs(move) < 0.05) return;
    int dir = move > 0 ? 1 : -1;
    if (thr_dir_ == 0) { thr_dir_ = dir; thr_pivot_ = throttle_pct; return; }
    if (dir == thr_dir_) { thr_pivot_ = throttle_pct; return; }
    if (thr_pivot_ && std::fabs(*throttle_pct - *thr_pivot_) >= kThrReversalPct) {
        thr_bad += kThrReversalCostS;
        events.push_back({round2(t), "reversal", "",
                          std::round(*throttle_pct * 10.0) / 10.0});
    }
    thr_dir_ = dir; thr_pivot_ = throttle_pct;
}

// Nice scores the size of the stop. Spirited scores its edges.
void DrivingScore::step_braking(double t, double dt, bool spirited) {
    if (!g.ready()) return;
    double lon = g.lon;
    brake_s += dt;
    if (spirited) {
        if (lon_ && lon_t_ && t > *lon_t_) {
            double jerk = std::fabs(lon - *lon_) / (t - *lon_t_);
            brake_bad += std::max(0.0, std::min(1.0, jerk / kBrakeJerkSpirited)) * dt;
        }
    } else {
        brake_bad += std::max(0.0, std::min(1.0, std::fabs(lon) / kBrakeGNice)) * dt;
    }
    lon_ = lon; lon_t_ = t;
}

// Nice scores the lateral g. Spirited scores sawing at the wheel.
void DrivingScore::step_cornering(double t, double dt, bool spirited) {
    if (!g.ready()) return;
    double lat = g.lat;
    corner_s += dt;
    if (spirited) {
        if (lat_ && lat_t_ && t > *lat_t_ && std::fabs(lat) >= kCornerActiveG) {
            double jerk = std::fabs(lat - *lat_) / (t - *lat_t_);
            corner_bad += std::max(0.0, std::min(1.0, jerk / kCornerJerkSpirited)) * dt;
        }
    } else {
        corner_bad += std::max(0.0, std::min(1.0, std::fabs(lat) / kCornerGNice)) * dt;
    }
    lat_ = lat; lat_t_ = t;
}

void DrivingScore::step_care(double dt, Opt rpm, Opt coolant_c) {
    if (!coolant_c || !rpm || *rpm <= 400) return;
    care_s += dt;
    if (*coolant_c >= kCareHotC) { care_bad += dt; return; }
    if (*coolant_c < kCareColdC && *rpm > kCareColdRpm) {
        double span = kCareColdRpmFull - kCareColdRpm;
        care_bad += std::max(0.0, std::min(1.0, (*rpm - kCareColdRpm) / span)) * dt;
    }
}

Opt DrivingScore::total() const {
    const std::pair<Opt, double> subs[] = {
        {throttle(), kWThrottle}, {braking(), kWBraking},
        {cornering(), kWCornering}, {care(), kWCare}};
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
    if (*t >= 85) return "CLEAN";
    if (*t >= 70) return "TIDY";
    const bool spirited = pole == Pole::Spirited;
    // Python takes min() over (value, label) tuples: lowest score wins, and
    // the label breaks a tie alphabetically.
    const std::pair<Opt, std::string> subs[] = {
        {throttle(),  spirited ? "INDECISIVE" : "JERKY"},
        {braking(),   spirited ? "SNATCHY"    : "HARSH"},
        {cornering(), spirited ? "SAWING"     : "FAST IN"},
        {care(),      "COLD REVS"}};
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
