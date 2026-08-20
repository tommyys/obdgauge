// Assertions ported from tests/test_pids.py. The Python is authoritative.
#include "check.h"
#include "pid.h"
using gauge_test::check;
using gauge::Bytes;
using D = std::optional<double>;
using I = std::optional<int>;

int main() {
    check("rpm 1A F8 -> 1726",  gauge::dec_rpm(Bytes{0x1A, 0xF8}), D{1726.0});
    check("rpm 0C 60 -> 792",   gauge::dec_rpm(Bytes{0x0C, 0x60}), D{792.0});
    check("rpm 00 00 -> 0",     gauge::dec_rpm(Bytes{0x00, 0x00}), D{0.0});
    check("short payload -> None", gauge::dec_rpm(Bytes{0x1A}), D{});

    check("coolant 0x5A -> 50C",  gauge::dec_temp(Bytes{0x5A}), I{50});
    check("coolant 0x28 -> 0C",   gauge::dec_temp(Bytes{0x28}), I{0});
    check("coolant 0x00 -> -40C", gauge::dec_temp(Bytes{0x00}), I{-40});

    check("speed 0x64 -> 100",     gauge::dec_speed(Bytes{0x64}), D{100.0});
    check("throttle 0xFF -> 100%", gauge::dec_percent(Bytes{0xFF}), D{100.0});
    check("throttle 0x00 -> 0%",   gauge::dec_percent(Bytes{0x00}), D{0.0});
    check("fuel trim 0x80 -> 0%",  gauge::dec_fuel_trim(Bytes{0x80}), D{0.0});
    check("maf 01 F4 -> 5.0 g/s",  gauge::dec_maf(Bytes{0x01, 0xF4}), D{5.0});
    check("timing 0x80 -> 0 deg",  gauge::dec_timing(Bytes{0x80}), D{0.0});
    check("fuel rate 00 64 -> 5 L/h",   gauge::dec_fuel_rate(Bytes{0x00, 0x64}), D{5.0});
    check("ctrl volt 37 6C -> 14.188V", gauge::dec_control_voltage(Bytes{0x37, 0x6C}), D{14.188});
    check("ref torque 00 FA -> 250Nm",  gauge::dec_ref_torque(Bytes{0x00, 0xFA}), D{250.0});

    check("catalyst 0F A0 -> 360.0C", gauge::dec_catalyst_temp(Bytes{0x0F, 0xA0}), D{360.0});
    check("catalyst 00 00 -> -40.0C", gauge::dec_catalyst_temp(Bytes{0x00, 0x00}), D{-40.0});
    check("lambda 80 00 -> 1.0",      gauge::dec_equiv_ratio(Bytes{0x80, 0x00}), D{1.0});
    check("O2 voltage 0x64 -> 0.5V",  gauge::dec_o2_voltage(Bytes{0x64}), D{0.5});
    check("fuel pressure 0x64 -> 300kPa", gauge::dec_fuel_pressure(Bytes{0x64}), D{300.0});
    check("runtime 00 3C -> 60s",     gauge::dec_u16(Bytes{0x00, 0x3C}), D{60.0});

    // decoders not covered by test_pids.py, pinned from the formulas in pids.py
    check("torque pct 0x7D -> 0%",      gauge::dec_torque_pct(Bytes{0x7D}), I{0});
    check("rail pressure 10 00 -> 323.584kPa",
          gauge::dec_rail_pressure(Bytes{0x10, 0x00}), D{4096 * 0.079});
    check("rail gauge 00 0A -> 100kPa", gauge::dec_rail_gauge(Bytes{0x00, 0x0A}), D{100.0});
    check("evap 80 00 -> 0Pa",          gauge::dec_evap_pressure(Bytes{0x80, 0x00}), D{0.0});
    check("inject timing 69 00 -> 0deg",gauge::dec_inject_timing(Bytes{0x69, 0x00}), D{210.0 - 210.0});
    check("egr error 0x80 -> 0%",       gauge::dec_egr_error(Bytes{0x80}), D{0.0});
    check("pressure kpa 0x64 -> 100",   gauge::dec_pressure_kpa(Bytes{0x64}), D{100.0});
    check("u8 0x2A -> 42",              gauge::dec_u8(Bytes{0x2A}), D{42.0});
    return gauge_test::check_report();
}
