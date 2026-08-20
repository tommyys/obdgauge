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

int main() {
    // --- the bug this file exists for ------------------------------------
    std::vector<double> gentle;
    for (int i = 0; i < 20; ++i) gentle.push_back(40 - i);
    check("gentle decel, speed only          -> no events", drive(gentle).harsh, 0);
    check("gentle decel, 4 other channels    -> still none", drive(gentle, 4).harsh, 0);
    check("gentle decel, 20 other channels   -> still none", drive(gentle, 20).harsh, 0);

    std::vector<double> hard = {60, 55, 48, 40, 31, 21, 10, 0};
    check("hard stop is caught, speed only", drive(hard).harsh > 0, true);
    check("hard stop is caught with 20 channels interleaved", drive(hard, 20).harsh > 0, true);
    check("and reports the same count either way",
          drive(hard).harsh == drive(hard, 20).harsh, true);

    // --- ties: two channels in the same millisecond -----------------------
    gauge::DrivingScore sc;
    sc.update(0.000, 15.0, 2000.0, 20.0, 6.0);
    for (int i = 0; i < 6; ++i) sc.update(0.100, 15.0, 2000.0, 20.0, 6.0);
    sc.update(0.430, 13.0, 2000.0, 20.0, 6.0);
    check("a tied timestamp invents no harsh event", sc.harsh, 0);

    // --- gaps -------------------------------------------------------------
    gauge::DrivingScore g;
    g.update(0.0, 100.0, 3000.0, 50.0, 12.0);
    g.update(30.0, 0.0, 800.0, 0.0, 1.0);
    check("a 30s gap invents no harsh event", g.harsh, 0);

    // --- smoothness is a rate over time ----------------------------------
    std::vector<double> flat50(30, 50.0), thr20(30, 20.0);
    auto steady = drive(flat50, 0, &thr20);
    near("steady throttle -> smooth 100", steady.smooth(), 100.0);
    auto steady_dense = drive(flat50, 10, &thr20);
    near("steady throttle, densely sampled -> still 100", steady_dense.smooth(), 100.0);

    std::vector<double> flat40(40, 50.0), saw;
    for (int i = 0; i < 40; ++i) saw.push_back(i % 2 ? 20.0 : 40.0);
    auto a = drive(flat40, 0, &saw);
    auto b = drive(flat40, 9, &saw);
    check("jerky throttle scores the same at both sample rates",
          a.smooth() && b.smooth() && std::fabs(*a.smooth() - *b.smooth()) < 1.0, true);
    check("jerky throttle scores worse than steady",
          a.smooth() && steady.smooth() && *a.smooth() < *steady.smooth(), true);

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
