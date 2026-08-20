// VehicleState: latest readings plus the plausibility gate that makes
// "views degrade honestly" (SPEC.md section 4) true rather than aspirational.
#include "check.h"
#include "state.h"
#include <cmath>
using gauge_test::check;
using D = std::optional<double>;

int main() {
    gauge::VehicleState s;
    check("fresh state has no coolant", s.has("coolant"), false);
    check("absent channel reads as nullopt", s.get("oil"), D{});

    s.set("coolant", 88.0);
    check("plausible coolant is kept", s.get("coolant"), D{88.0});
    s.set("coolant", 900.0);
    check("implausible coolant is rejected", s.get("coolant"), D{88.0});
    check("rejection is counted", s.rejected(), 1);

    s.set("rpm", 1726.0);
    check("rpm kept", s.get("rpm"), D{1726.0});
    s.set("rpm", -5.0);
    check("negative rpm rejected", s.get("rpm"), D{1726.0});

    check("plausible accepts mid-scale coolant", gauge::plausible("coolant", 88.0), true);
    check("plausible rejects 900C coolant",      gauge::plausible("coolant", 900.0), false);
    check("plausible rejects NaN", gauge::plausible("rpm", std::nan("")), false);
    check("plausible rejects inf", gauge::plausible("rpm", INFINITY), false);
    // Unknown keys get a permissive default so new channels still get logged.
    check("unknown key accepts a sane value",   gauge::plausible("brand_new", 42.0), true);
    check("unknown key rejects absurd values",  gauge::plausible("brand_new", 1e9), false);

    // 0 degC is a real reading, not an absence.
    gauge::VehicleState z;
    z.set("coolant", 0.0);
    check("zero is a real reading", z.has("coolant"), true);
    check("zero reads back as zero", z.get("coolant"), D{0.0});

    // Peak rpm tracking, and the power estimate derived from torque x rpm.
    gauge::VehicleState p;
    p.set("rpm", 3000.0);
    p.set("ref_torque", 200.0);
    p.set("act_torque", 50.0);
    double nm = 200.0 * 50.0 / 100.0;
    double kw = nm * 3000.0 * 2 * 3.14159 / 60.0 / 1000.0;
    check("power_kw derived from torque and rpm", p.get("power_kw"), D{kw});
    p.set("rpm", 1000.0);
    check("peak rpm remembered", p.peak_rpm(), 3000.0);
    return gauge_test::check_report();
}
