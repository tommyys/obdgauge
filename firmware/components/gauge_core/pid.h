// SAE J1979 Mode 01 decode formulas.
// Ported from mx5gauge/pids.py:12-178 — the Python is authoritative.
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace gauge {

using Bytes = std::vector<uint8_t>;

std::optional<double> dec_rpm(const Bytes& d);
std::optional<int>    dec_temp(const Bytes& d);
std::optional<double> dec_speed(const Bytes& d);
std::optional<double> dec_percent(const Bytes& d);
std::optional<double> dec_fuel_trim(const Bytes& d);
std::optional<double> dec_maf(const Bytes& d);
std::optional<double> dec_timing(const Bytes& d);
std::optional<double> dec_pressure_kpa(const Bytes& d);
std::optional<double> dec_fuel_rate(const Bytes& d);
std::optional<double> dec_control_voltage(const Bytes& d);
std::optional<int>    dec_torque_pct(const Bytes& d);
std::optional<double> dec_ref_torque(const Bytes& d);
std::optional<double> dec_u8(const Bytes& d);
std::optional<double> dec_u16(const Bytes& d);
std::optional<double> dec_fuel_pressure(const Bytes& d);
std::optional<double> dec_rail_pressure(const Bytes& d);
std::optional<double> dec_rail_gauge(const Bytes& d);
std::optional<double> dec_o2_voltage(const Bytes& d);
std::optional<double> dec_equiv_ratio(const Bytes& d);
std::optional<double> dec_evap_pressure(const Bytes& d);
std::optional<double> dec_inject_timing(const Bytes& d);
std::optional<double> dec_egr_error(const Bytes& d);
std::optional<double> dec_catalyst_temp(const Bytes& d);
std::optional<int>    dec_torque_demand(const Bytes& d);

}  // namespace gauge
