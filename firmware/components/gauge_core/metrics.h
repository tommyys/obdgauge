// Derived metrics: fuel economy, trip accumulators and the driving score.
// Ported from mx5gauge/metrics.py — pure, no I/O, host-testable.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "gforce.h"

namespace gauge {

// --- tunables (SPEC.md section 5). B3 changes these, in one place. ---------
// RM per litre. 1.99 is BUDI95's subsidised RON95 price from 30 September
// 2025, replacing the 2.05 of the old blanket subsidy -- every cost this gauge
// has ever shown was about 3% high. Must match FUEL_PRICE_RM in
// mx5gauge/metrics.py: tools/verify_port.sh cross-checks the derived cost.
inline constexpr double kFuelPriceRm = 1.99;
// --- driving-score tuning (SPEC.md B3, decided 2026-08-29) ----------------
// The score answers one question: how tidily did you do whatever you were
// doing? It deliberately does not answer "were you driving gently?", because
// the old score did, and that made a good backroad drive look like bad
// driving. Two poles, one formula: intensity picks the pole, the pole picks
// the bands, the weights never change. Fuel economy is not in the score at
// all any more -- it is a readout on the Trip view.
// Every constant here mirrors mx5gauge/metrics.py and is cross-checked by
// tools/verify_port.sh. Change one, change both, in the same commit.
inline constexpr double kWThrottle   = 0.30;   // score weights, sum to 1.0
inline constexpr double kWBraking    = 0.30;
inline constexpr double kWCornering  = 0.25;
inline constexpr double kWCare       = 0.15;
// What counts as good economy on this car, in km/L, and where the ring's
// colour ramp starts. Green is still 11, the measured average; the bottom of
// the ramp moved from 9 to 6 because two km/L of range put the whole fade
// inside the wobble of a trip average -- the ring read as green or red with
// nothing in between. Not physical constants: change them when the car's own
// average moves.
inline constexpr double kEconGoodKmL = 11.0;
inline constexpr double kEconPoorKmL = 6.0;
inline constexpr double kEcoRpmLo    = 1200;   // efficient cruising band
inline constexpr double kEcoRpmHi    = 2600;
// The poles. The gap between them is deliberate: a single threshold would
// flutter every time a number sat on it, so a drive has to mean it to change
// pole -- and mean it for kPoleHoldS as well.
inline constexpr double kNiceBelow     = 0.35;
inline constexpr double kSpiritedAbove = 0.55;
inline constexpr double kPoleHoldS     = 5.0;
// Intensity is a rolling read of the last half-minute, not an instant one.
// The dot on the g view is what you just did; the pole is how you have been
// driving. They are meant to move at different speeds.
inline constexpr double kIntensityTauS = 30.0;
inline constexpr double kRpmCalmFrac   = 0.30;   // fractions of redline...
inline constexpr double kRpmHotFrac    = 0.85;   // ...that map intensity 0->1
inline constexpr double kGHot          = 0.45;   // fully committed, in g
inline constexpr double kThrCalm       = 25.0;   // throttle %, cruising...
inline constexpr double kThrHot        = 70.0;   // ...to meaning it

// The bands. Only these change between poles.
inline constexpr double kThrRateNice     = 12.0;   // %/s for a full demerit
inline constexpr double kThrRateSpirited = 40.0;
// A change of throttle direction bigger than this is a reversal: on, off, on
// again. Big inputs are fine; changing your mind about them is not.
inline constexpr double kThrReversalPct   = 4.0;
inline constexpr double kThrReversalCostS = 0.5;
// Braking. Nice scores the size of the stop; Spirited scores its edges,
// because a good hard stop ramps in and bleeds off.
inline constexpr double kBrakeGNice        = 0.25;
inline constexpr double kBrakeJerkSpirited = 1.5;   // g per second
// Cornering. Same split: Nice scores the g, Spirited scores the sawing.
inline constexpr double kCornerGNice        = 0.30;
inline constexpr double kCornerJerkSpirited = 1.2;  // g per second
inline constexpr double kCornerActiveG      = 0.20; // no corner to spoil below
// Mechanical care: what replaced economy as the guard on the Spirited pole.
// Revving a cold engine is the one thing that is wrong however tidily it is
// done.
inline constexpr double kCareColdC       = 80.0;
inline constexpr double kCareColdRpm     = 3500.0;
inline constexpr double kCareColdRpmFull = 5500.0;
inline constexpr double kCareHotC        = 105.0;

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

enum class Pole { Nice, Spirited };

// Something worth remembering about the drive. `value` carries whatever the
// kind needs -- the throttle position at a reversal, nothing for a pole
// change.
struct ScoreEvent {
    double      t;
    std::string kind;    // "reversal" or "pole"
    std::string label;   // "NICE"/"SPIRITED" for a pole change, else empty
    double      value = 0.0;
};

// 0-100 "how tidily am I driving" from four sub-scores.
//
// Everything is measured in demerit-seconds. Each sample contributes
// demerit * dt, where demerit runs 0 (faultless) to 1 (as bad as the band
// goes), and an event like a throttle reversal contributes a fixed number of
// seconds outright. A sub-score is then 100 * (1 - demerit_s / observed_s).
// One currency for rates and events alike makes the maths sample-rate
// independent by construction -- the bug this class was rewritten around
// once already -- and lets this file be a transcription of the Python.
//
// Missing channel means missing sub-score, not a zero: a car with no coolant
// channel has no care score and the other three renormalise. Same honesty
// rule as gauge::view_available.
class DrivingScore {
  public:
    // The IMU is fed separately from the OBD channels because on the board it
    // is read far faster than any of them, and jerk is the whole reason for
    // using it.
    void imu(double t, Opt ax, Opt ay, Opt az) { g.update(t, ax, ay, az); }
    void update(double t, Opt speed_kph, Opt rpm, Opt throttle_pct,
                Opt fuel_rate_lph, Opt coolant_c = std::nullopt);

    Opt         throttle() const { return sub(thr_bad, thr_s); }
    Opt         braking() const { return sub(brake_bad, brake_s); }
    Opt         cornering() const { return sub(corner_bad, corner_s); }
    Opt         care() const { return sub(care_bad, care_s); }
    Opt         total() const;
    std::string coach() const;

    GForce g;

    double thr_s = 0.0, thr_bad = 0.0;
    double brake_s = 0.0, brake_bad = 0.0;
    double corner_s = 0.0, corner_bad = 0.0;
    double care_s = 0.0, care_bad = 0.0;
    double rev_s = 0.0;

    double intensity = 0.0;
    Pole   pole      = Pole::Nice;
    double nice_s = 0.0, spirited_s = 0.0;
    // Overwritten from the car profile. 7000 is the MX-5's.
    double rpm_red = 7000.0;
    std::vector<ScoreEvent> events;

  private:
    static Opt sub(double bad, double seconds);
    void       rebase(double t, Opt throttle_pct);
    void       step_intensity(double dt, Opt rpm, Opt throttle_pct);
    void       step_pole(double t, double dt);
    void       step_throttle(double t, Opt throttle_pct, bool spirited);
    void       step_braking(double t, double dt, bool spirited);
    void       step_cornering(double t, double dt, bool spirited);
    void       step_care(double dt, Opt rpm, Opt coolant_c);

    Opt last_t_, thr_, thr_t_, lon_, lon_t_, lat_, lat_t_;
    int thr_dir_ = 0;                 // +1 opening, -1 closing, 0 unknown
    Opt thr_pivot_;                   // where the pedal last changed direction
    Pole cand_ = Pole::Nice;
    Opt  cand_since_;
};

}  // namespace gauge
