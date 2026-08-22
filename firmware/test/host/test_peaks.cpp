// Per-drive high-water marks, ported from the simulator's PEAK_FIELDS.
// Assertions mirror tests/test_peaks.py.
#include "check.h"
#include "state.h"
using gauge_test::check;
using D = std::optional<double>;

int main() {
    gauge::VehicleState s;
    // peak_rpm answers 0.0 before any rev: the tacho draws it unconditionally.
    check("peak_rpm is 0 before any rev", s.peak_rpm(), 0.0);
    // The other peaks answer absent, so the card can show '--' rather than a
    // bold 0 for a sensor the car does not have.
    check("peak speed absent before any reading",    s.peak("speed"), D{});
    check("peak catalyst absent before any reading", s.peak("catalyst"), D{});

    s.set("rpm", 2200.0);
    s.set("rpm", 5400.0);
    s.set("rpm", 1200.0);
    check("peak rpm is the high-water mark", s.peak_rpm(), 5400.0);

    s.set("speed", 60.0);
    s.set("speed", 40.0);
    check("peak speed holds", s.peak("speed"), D{60.0});

    // Catalyst arrives on any of five bank/sensor channels and they collapse
    // into one figure: the hottest of whichever the car reported.
    gauge::VehicleState c;
    c.set("cat_b1s1", 520.0);
    c.set("cat_b2s1", 610.0);
    c.set("cat_b1s2", 480.0);
    check("catalyst peak is the hottest bank", c.peak("catalyst"), D{610.0});
    check("catalyst peak is one entry, not four",
          static_cast<int>(c.peaks().count("catalyst")), 1);

    // A single implausible frame must not pin a peak for the whole drive.
    gauge::VehicleState p;
    p.set("coolant", 88.0);
    p.set("coolant", 900.0);
    check("implausible reading cannot pin a peak", p.peak("coolant"), D{88.0});
    check("...and was counted as rejected", p.rejected(), 1);

    // Channels with no summary field are not tracked as peaks.
    gauge::VehicleState o;
    o.set("throttle", 90.0);
    check("throttle is not a peak field", o.peak("throttle"), D{});
    return gauge_test::check_report();
}
