// Derived metrics: fuel economy, trip accumulators and the driving score.
// Ported from mx5gauge/metrics.py — pure, no I/O, host-testable.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace gauge {

// --- tunables (SPEC.md section 5). B3 changes these, in one place. ---------
// RM per litre. 1.99 is BUDI95's subsidised RON95 price from 30 September
// 2025, replacing the 2.05 of the old blanket subsidy -- every cost this gauge
// has ever shown was about 3% high. Must match FUEL_PRICE_RM in
// mx5gauge/metrics.py: tools/verify_port.sh cross-checks the derived cost.
inline constexpr double kFuelPriceRm = 1.99;
inline constexpr double kWSmooth     = 0.40;   // score weights, sum to 1.0
inline constexpr double kWEcon       = 0.30;
inline constexpr double kWCalm       = 0.30;
// What counts as good economy on this car, in km/L. Measured average is about
// 11, so the TRIP view's km/L reads green at or above it, amber within 2 of
// it, and red below that. Not a physical constant -- change it when the car's
// own average moves.
inline constexpr double kEconGoodKmL = 11.0;
inline constexpr double kEconPoorKmL = 9.0;
inline constexpr double kEcoRpmLo    = 1200;   // efficient cruising band
inline constexpr double kEcoRpmHi    = 2600;
// Harsh-event thresholds (m/s^2). ~2.5 m/s^2 is a firm but normal stop.
// These are guesses tuned against a speed-delta proxy; they must be re-tuned
// against the board's QMI8658 IMU before they mean anything (backlog B3).
inline constexpr double kHarshAccel  = 2.5;
inline constexpr double kHarshBrake  = -3.0;

using Opt = std::optional<double>;

// Instantaneous L/100km. Absent when stationary (it would be infinite).
Opt instant_econ(Opt speed_kph, Opt fuel_rate_lph);
// L/100km -> km/L, the unit the display uses. Absent for non-positive input:
// an engine on overrun cuts the injectors, and 100/0 is not printable.
Opt km_per_l(Opt l_per_100km);

class Trip {
  public:
    void update(double t, Opt speed_kph, Opt fuel_rate_lph);

    double dist_km   = 0.0;
    double fuel_l    = 0.0;
    double moving_s  = 0.0;
    double elapsed_s = 0.0;

    double cost_rm() const { return fuel_l * kFuelPriceRm; }
    Opt    econ_l_per_100() const;
    Opt    econ_km_per_l() const { return km_per_l(econ_l_per_100()); }
    double avg_speed_kph() const;

  private:
    Opt last_t_, last_speed_, last_rate_;
};

struct HarshEvent {
    double      t;
    std::string kind;   // "accel" or "brake"
    double      a;      // m/s^2, rounded to 2dp as the Python does
};

class DrivingScore {
  public:
    void update(double t, Opt speed_kph, Opt rpm, Opt throttle_pct, Opt fuel_rate_lph);

    Opt         smooth() const;
    Opt         econ() const;
    Opt         calm() const;
    Opt         total() const;
    std::string coach() const;

    double thr_travel  = 0.0;
    double thr_seconds = 0.0;
    double eco_s       = 0.0;
    double rev_s       = 0.0;
    double econ_sum    = 0.0;
    double econ_s      = 0.0;
    int    harsh       = 0;
    std::vector<HarshEvent> events;

  private:
    void rebase(double t, Opt speed_kph, Opt throttle_pct);

    Opt last_t_, thr_, thr_t_, spd_, spd_t_;
};

}  // namespace gauge
