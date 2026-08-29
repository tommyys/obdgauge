// Ported from tests/test_metrics.py. The score is fed on every sample of ANY
// channel, so its rates must depend on the car's physics, not on how many
// PIDs happen to be polled.
#include "check.h"
#include "metrics.h"
#include <cmath>
#include <vector>
using gauge_test::check;
using gauge_test::near;
using O = std::optional<double>;

// Replay a speed series, optionally with `filler` extra samples of other
// channels between each speed reading.
static gauge::DrivingScore drive(const std::vector<double>& speeds, int filler = 0,
                                 const std::vector<double>* throttle = nullptr,
                                 double rpm = 2000.0, double step = 0.33) {
    gauge::DrivingScore sc;
    double t = 0.0;
    for (size_t i = 0; i < speeds.size(); ++i) {
        double thr = throttle ? (*throttle)[i] : 20.0;
        sc.update(t, speeds[i], rpm, thr, 6.0);
        for (int j = 0; j < filler; ++j) {
            t += step / (filler + 1);
            sc.update(t, speeds[i], rpm, thr, 6.0);
        }
        t += filler ? step / (filler + 1) : step;
    }
    return sc;
}

// Replay a repeating (lat, lon) pattern through the score with the pole
// forced. Forcing it keeps these cases about the bands; whether the intensity
// latch picks the right pole is tested separately.
//
// The g values are injected by restoring a known set of axes and synthesising
// the accelerometer sample that produces them -- the same path the car takes,
// rather than a second way in that only the tests use.
static gauge::DrivingScore g_drive(const std::vector<std::pair<double,double>>& pattern,
                                   int steps, gauge::Pole pole, double step = 0.25) {
    gauge::DrivingScore sc;
    const gauge::Vec3 down{0.0, 0.0, 1.0}, fwd{1.0, 0.0, 0.0};
    const gauge::Vec3 right{0.0, 1.0, 0.0};    // = down x fwd
    sc.g.restore_axes({fwd, down, 1.0});
    sc.update(0.0, 30.0, 2000.0, 20.0, 6.0);
    double t = 0.0;
    // Settle the gravity average on level ground first. The solver seeds it
    // from the very first sample, so starting a case with a held 0.5 g would
    // teach it that 0.5 g is which way down is -- and the residual, which is
    // what braking is measured from, would come out as zero. A car boots
    // parked, so this is what actually happens; the test has to do it too.
    for (int i = 0; i < 400; ++i) {
        t += step;
        sc.imu(t, down.x, down.y, down.z);
        sc.update(t, 30.0, 2000.0, 20.0, 6.0);
    }
    sc.thr_s = sc.thr_bad = 0.0;
    sc.brake_s = sc.brake_bad = 0.0;
    sc.corner_s = sc.corner_bad = 0.0;
    for (int i = 0; i < steps; ++i) {
        const auto& p = pattern[i % pattern.size()];
        t += step;
        // lon is positive under braking, which is a backwards push.
        sc.imu(t, down.x - fwd.x * p.second + right.x * p.first,
                  down.y - fwd.y * p.second + right.y * p.first,
                  down.z - fwd.z * p.second + right.z * p.first);
        sc.pole = pole;
        sc.update(t, 30.0, 2000.0, 20.0, 6.0);
        sc.pole = pole;
    }
    return sc;
}

int main() {
    // --- the bug this file exists for ------------------------------------
    // The score is fed on every sample of ANY channel, so its rates must
    // depend on the car's physics and not on how many PIDs are polled.
    std::vector<double> flat50(30, 50.0), thr20(30, 20.0);
    auto steady = drive(flat50, 0, &thr20);
    near("steady throttle -> throttle 100", steady.throttle(), 100.0);
    auto steady_dense = drive(flat50, 10, &thr20);
    near("steady throttle, densely sampled -> still 100",
         steady_dense.throttle(), 100.0);

    std::vector<double> flat40(40, 50.0), saw;
    for (int i = 0; i < 40; ++i) saw.push_back(i % 2 ? 20.0 : 40.0);
    auto a = drive(flat40, 0, &saw);
    auto b = drive(flat40, 9, &saw);
    check("jerky throttle scores the same at both sample rates",
          a.throttle() && b.throttle()
              && std::fabs(*a.throttle() - *b.throttle()) < 1.0, true);
    check("jerky throttle scores worse than steady",
          a.throttle() && steady.throttle()
              && *a.throttle() < *steady.throttle(), true);

    // --- ties: two channels in the same millisecond -----------------------
    gauge::DrivingScore sc;
    sc.update(0.000, 15.0, 2000.0, 20.0, 6.0);
    for (int i = 0; i < 6; ++i) sc.update(0.100, 15.0, 2000.0, 20.0, 6.0);
    sc.update(0.430, 13.0, 2000.0, 20.0, 6.0);
    near("a tied timestamp costs no throttle demerit", O{sc.thr_bad}, 0.0, 1e-9);

    // --- gaps -------------------------------------------------------------
    gauge::DrivingScore g;
    g.update(0.0, 100.0, 3000.0, 50.0, 12.0);
    g.update(30.0, 0.0, 800.0, 0.0, 1.0);
    near("a 30s gap costs no demerit at all", O{g.thr_bad}, 0.0, 1e-9);

    // --- throttle reversals ----------------------------------------------
    // Both of these move the pedal 5% every half second, so they earn exactly
    // the same rate demerit. The only difference is direction, which is what
    // isolates the reversal cost.
    gauge::DrivingScore one_way;
    for (int i = 0; i < 21; ++i)
        one_way.update(i * 0.5, 30.0, 3000.0, i * 5.0, 6.0);
    int rev_one = 0;
    for (const auto& e : one_way.events) if (e.kind == "reversal") ++rev_one;
    check("a big single-direction input is not a reversal", rev_one, 0);
    gauge::DrivingScore flutter;
    for (int i = 0; i < 21; ++i)
        flutter.update(i * 0.5, 30.0, 3000.0, 45.0 + (i % 2 ? 5.0 : -5.0), 6.0);
    int rev_flutter = 0;
    for (const auto& e : flutter.events) if (e.kind == "reversal") ++rev_flutter;
    check("fluttering the pedal is caught as reversals", rev_flutter > 5, true);
    check("a reversal costs throttle score",
          *flutter.throttle() < *one_way.throttle(), true);

    // --- braking: Nice scores the size, Spirited scores the edges ---------
    auto soft = g_drive({{0.0, 0.10}}, 40, gauge::Pole::Nice);
    auto firm = g_drive({{0.0, 0.50}}, 40, gauge::Pole::Nice);
    check("a nice segment marks down a hard stop",
          *firm.braking() < *soft.braking(), true);
    near("0.5g held is past the nice band, so braking bottoms out",
         firm.braking(), 0.0, 0.5);

    auto held = g_drive({{0.0, 0.5}}, 40, gauge::Pole::Spirited);
    auto snatch = g_drive({{0.0, 0.5}, {0.0, 0.0}}, 40, gauge::Pole::Spirited);
    // Not exactly 100: over ten seconds of held braking the gravity average
    // starts to absorb a little of it, and a slowly shrinking g is a small
    // jerk. That is real, not an artefact -- a car cannot hold 0.5 g for ten
    // seconds without the reference drifting -- so the tolerance allows it.
    near("a spirited segment does not punish sustained 0.5g braking",
         held.braking(), 100.0, 5.0);
    check("but it does punish snatching the same 0.5g on and off",
          *snatch.braking() < *held.braking() - 20.0, true);
    check("...and the nice band would have failed the sustained one",
          *g_drive({{0.0, 0.5}}, 40, gauge::Pole::Nice).braking()
              < *held.braking(), true);

    // --- cornering --------------------------------------------------------
    auto gentle_turn = g_drive({{0.15, 0.0}}, 40, gauge::Pole::Nice);
    auto hard_turn = g_drive({{0.60, 0.0}}, 40, gauge::Pole::Nice);
    check("a nice segment marks down a fast corner",
          *hard_turn.cornering() < *gentle_turn.cornering(), true);
    auto smooth_fast = g_drive({{0.60, 0.0}}, 40, gauge::Pole::Spirited);
    auto sawing = g_drive({{0.60, 0.0}, {0.25, 0.0}}, 40, gauge::Pole::Spirited);
    near("a spirited segment does not punish a steady 0.6g corner",
         smooth_fast.cornering(), 100.0, 5.0);
    check("but sawing at the wheel inside one is punished",
          *sawing.cornering() < *smooth_fast.cornering() - 20.0, true);

    // --- missing channel means missing sub-score --------------------------
    // Same honesty rule as gauge::view_available. A convincing number for
    // data we do not have is worse than an em-dash.
    gauge::DrivingScore no_g;
    for (int i = 0; i < 60; ++i)
        no_g.update(i * 0.25, 40.0, 2500.0, 25.0, 6.0, 90.0);
    check("no g axes yet -> no braking score", no_g.braking(), O{});
    check("no g axes yet -> no cornering score", no_g.cornering(), O{});
    check("...but the score still exists on what is left",
          no_g.total().has_value(), true);
    gauge::DrivingScore no_cool;
    for (int i = 0; i < 60; ++i)
        no_cool.update(i * 0.25, 40.0, 2500.0, 25.0, 6.0);
    check("no coolant channel -> no care score", no_cool.care(), O{});
    check("with only throttle left, the total is the throttle score",
          std::fabs(*no_cool.total() - *no_cool.throttle()) < 1e-9, true);

    // --- the pole ---------------------------------------------------------
    // Intensity is a rolling read of the last half-minute, so a pole has to
    // be earned over time, not by one loud sample.
    gauge::DrivingScore calm;
    for (int i = 0; i < 600; ++i)
        calm.update(i * 0.25, 40.0, 2000.0, 20.0, 6.0);
    check("a gentle drive stays nice", calm.pole == gauge::Pole::Nice, true);
    gauge::DrivingScore hot;
    for (int i = 0; i < 600; ++i)
        hot.update(i * 0.25, 90.0, 6000.0, 80.0, 20.0);
    check("a hard drive earns the spirited pole",
          hot.pole == gauge::Pole::Spirited, true);
    check("...and banks its seconds there",
          hot.spirited_s > 60.0 && hot.nice_s < hot.spirited_s, true);
    check("a gentle drive banks none", calm.spirited_s, 0.0);
    gauge::DrivingScore blip;
    for (int i = 0; i < 600; ++i) {
        bool on = i >= 100 && i < 104;
        blip.update(i * 0.25, 40.0, on ? 6500.0 : 1800.0, on ? 90.0 : 15.0, 6.0);
    }
    check("one blip of throttle does not flip the pole",
          blip.pole == gauge::Pole::Nice, true);

    // --- economy is out of the score --------------------------------------
    // B3's decision: a score that marks you down for enjoying the car is the
    // thing it was opened to stop.
    gauge::DrivingScore thirsty, frugal;
    for (int i = 0; i < 200; ++i) {
        thirsty.update(i * 0.25, 40.0, 2500.0, 25.0, 30.0, 90.0);
        frugal.update(i * 0.25, 40.0, 2500.0, 25.0, 3.0, 90.0);
    }
    check("fuel rate no longer moves the score",
          std::fabs(*thirsty.total() - *frugal.total()) < 1e-9, true);

    // --- mechanical care --------------------------------------------------
    gauge::DrivingScore cold_revs, warm_revs;
    for (int i = 0; i < 200; ++i) {
        cold_revs.update(i * 0.25, 60.0, 5000.0, 60.0, 12.0, 45.0);
        warm_revs.update(i * 0.25, 60.0, 5000.0, 60.0, 12.0, 90.0);
    }
    check("revving a cold engine costs care", *cold_revs.care() < 50.0, true);
    near("the same revs warm cost nothing", warm_revs.care(), 100.0);
    gauge::DrivingScore overheat;
    for (int i = 0; i < 200; ++i)
        overheat.update(i * 0.25, 60.0, 2000.0, 20.0, 8.0, 110.0);
    near("cooking the engine bottoms out care", overheat.care(), 0.0, 0.5);

    // --- trip accumulators ------------------------------------------------
    gauge::Trip tr;
    for (int i = 0; i <= 100; ++i) tr.update(i * 1.0, 36.0, 7.2);
    near("100s at 36km/h -> 1.0 km", O{tr.dist_km}, 1.0, 0.02);
    near("7.2 L/h for 100s -> 0.2 L", O{tr.fuel_l}, 0.2, 0.01);
    near("cost at RM2.05/L", O{tr.cost_rm()}, 0.2 * gauge::kFuelPriceRm, 0.01);
    near("avg speed", O{tr.avg_speed_kph()}, 36.0, 0.5);

    gauge::Trip gap;
    gap.update(0.0, 100.0, 10.0);
    gap.update(600.0, 100.0, 10.0);
    check("a gap adds no distance", gap.dist_km, 0.0);

    near("instant econ: 36km/h on 7.2L/h -> 20 L/100km",
         gauge::instant_econ(36.0, 7.2), 20.0, 0.01);
    check("instant econ is None when stationary", gauge::instant_econ(0.0, 1.0), O{});

    near("20 L/100km reads as 5 km/L", gauge::km_per_l(20.0), 5.0, 0.001);
    near("5 L/100km reads as 20 km/L", gauge::km_per_l(5.0), 20.0, 0.001);
    check("no reading converts to no reading", gauge::km_per_l(O{}), O{});
    check("zero consumption has no km/L to quote", gauge::km_per_l(0.0), O{});
    check("a negative reading is refused too", gauge::km_per_l(-3.0), O{});

    gauge::Trip tr2;
    for (int i = 0; i <= 100; ++i) tr2.update(i * 1.0, 36.0, 7.2);
    near("trip average in km/L", tr2.econ_km_per_l(), 5.0, 0.05);
    near("...is the reciprocal of the L/100km figure", tr2.econ_km_per_l(),
         100.0 / *tr2.econ_l_per_100(), 1e-9);

    gauge::Trip empty;
    check("too little distance to quote km/L", empty.econ_km_per_l(), O{});
    return gauge_test::check_report();
}
