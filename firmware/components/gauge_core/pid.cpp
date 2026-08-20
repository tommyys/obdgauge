#include "pid.h"

namespace gauge {
namespace {
inline bool have(const Bytes& d, size_t n) { return d.size() >= n; }
inline double u16v(const Bytes& d) { return d[0] * 256.0 + d[1]; }
}  // namespace

// ((A*256)+B)/4 -> rpm
std::optional<double> dec_rpm(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 4.0;
}
// A - 40 -> deg C   (coolant, intake air, oil, ...)
std::optional<int> dec_temp(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<int>(d[0]) - 40;
}
// A -> km/h
std::optional<double> dec_speed(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<double>(d[0]);
}
// A * 100/255 -> %
std::optional<double> dec_percent(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] * 100.0 / 255.0;
}
// (A - 128) * 100/128 -> %
std::optional<double> dec_fuel_trim(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return (static_cast<double>(d[0]) - 128.0) * 100.0 / 128.0;
}
// ((A*256)+B)/100 -> g/s
std::optional<double> dec_maf(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 100.0;
}
// (A/2) - 64 -> deg before TDC
std::optional<double> dec_timing(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return (d[0] / 2.0) - 64.0;
}
// A -> kPa
std::optional<double> dec_pressure_kpa(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<double>(d[0]);
}
// ((A*256)+B)/20 -> L/h
std::optional<double> dec_fuel_rate(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 20.0;
}
// ((A*256)+B)/1000 -> V
std::optional<double> dec_control_voltage(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 1000.0;
}
// A - 125 -> %
std::optional<int> dec_torque_pct(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<int>(d[0]) - 125;
}
// (A*256)+B -> Nm
std::optional<double> dec_ref_torque(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d);
}
std::optional<double> dec_u8(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<double>(d[0]);
}
std::optional<double> dec_u16(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d);
}
// 3*A -> kPa
std::optional<double> dec_fuel_pressure(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] * 3.0;
}
// 0.079 * ((A*256)+B) -> kPa
std::optional<double> dec_rail_pressure(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) * 0.079;
}
// 10 * ((A*256)+B) -> kPa
std::optional<double> dec_rail_gauge(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) * 10.0;
}
// A/200 -> V
std::optional<double> dec_o2_voltage(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return d[0] / 200.0;
}
// ((A*256)+B)/32768 -> ratio (lambda)
std::optional<double> dec_equiv_ratio(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 32768.0;
}
// ((A*256)+B)/4 - 8192 -> Pa
std::optional<double> dec_evap_pressure(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 4.0 - 8192.0;
}
// ((A*256)+B)/128 - 210 -> deg
std::optional<double> dec_inject_timing(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 128.0 - 210.0;
}
// (A-128)*100/128 -> %
std::optional<double> dec_egr_error(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return (static_cast<double>(d[0]) - 128.0) * 100.0 / 128.0;
}
// ((A*256)+B)/10 - 40 -> deg C
std::optional<double> dec_catalyst_temp(const Bytes& d) {
    if (!have(d, 2)) return std::nullopt;
    return u16v(d) / 10.0 - 40.0;
}
// A - 125 -> %
std::optional<int> dec_torque_demand(const Bytes& d) {
    if (!have(d, 1)) return std::nullopt;
    return static_cast<int>(d[0]) - 125;
}

}  // namespace gauge
