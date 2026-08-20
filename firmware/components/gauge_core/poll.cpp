#include "poll.h"
#include <algorithm>
#include <map>

namespace gauge {
namespace {

// Wrappers for the decoders that return int in the Python, so every table
// entry has one uniform signature.
std::optional<double> dec_temp_d(const Bytes& d) {
    auto v = dec_temp(d);
    return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
}
std::optional<double> dec_torque_pct_d(const Bytes& d) {
    auto v = dec_torque_pct(d);
    return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
}
std::optional<double> dec_torque_demand_d(const Bytes& d) {
    auto v = dec_torque_demand(d);
    return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
}

const std::map<uint8_t, PidInfo>& table() {
    static const std::map<uint8_t, PidInfo> t = {
    {0x03, {"fuel_status", "Fuel system status", "", dec_u8}},
    {0x04, {"load", "Engine load", "%", dec_percent}},
    {0x05, {"coolant", "Coolant temp", "C", dec_temp_d}},
    {0x06, {"stft1", "Short fuel trim B1", "%", dec_fuel_trim}},
    {0x07, {"ltft1", "Long fuel trim B1", "%", dec_fuel_trim}},
    {0x08, {"stft2", "Short fuel trim B2", "%", dec_fuel_trim}},
    {0x09, {"ltft2", "Long fuel trim B2", "%", dec_fuel_trim}},
    {0x0A, {"fuel_press", "Fuel pressure", "kPa", dec_fuel_pressure}},
    {0x0B, {"map", "Intake manifold", "kPa", dec_pressure_kpa}},
    {0x0C, {"rpm", "Engine RPM", "rpm", dec_rpm}},
    {0x0D, {"speed", "Vehicle speed", "km/h", dec_speed}},
    {0x0E, {"timing", "Timing advance", "deg", dec_timing}},
    {0x0F, {"intake", "Intake air temp", "C", dec_temp_d}},
    {0x10, {"maf", "MAF flow", "g/s", dec_maf}},
    {0x11, {"throttle", "Throttle position", "%", dec_percent}},
    {0x14, {"o2_b1s1", "O2 B1S1 voltage", "V", dec_o2_voltage}},
    {0x15, {"o2_b1s2", "O2 B1S2 voltage", "V", dec_o2_voltage}},
    {0x1F, {"run_time", "Run time since start", "s", dec_u16}},
    {0x21, {"dist_mil", "Distance with MIL", "km", dec_u16}},
    {0x22, {"rail_press", "Fuel rail pressure", "kPa", dec_rail_pressure}},
    {0x23, {"rail_gauge", "Fuel rail gauge", "kPa", dec_rail_gauge}},
    {0x2C, {"egr_cmd", "Commanded EGR", "%", dec_percent}},
    {0x2D, {"egr_err", "EGR error", "%", dec_egr_error}},
    {0x2E, {"evap_purge", "Commanded evap purge", "%", dec_percent}},
    {0x2F, {"fuel_level", "Fuel tank level", "%", dec_percent}},
    {0x30, {"warmups", "Warm-ups since clear", "", dec_u8}},
    {0x31, {"dist_clear", "Distance since clear", "km", dec_u16}},
    {0x32, {"evap_press", "Evap vapour pressure", "Pa", dec_evap_pressure}},
    {0x33, {"baro", "Barometric press.", "kPa", dec_pressure_kpa}},
    {0x3C, {"cat_b1s1", "Catalyst temp B1S1", "C", dec_catalyst_temp}},
    {0x3D, {"cat_b2s1", "Catalyst temp B2S1", "C", dec_catalyst_temp}},
    {0x3E, {"cat_b1s2", "Catalyst temp B1S2", "C", dec_catalyst_temp}},
    {0x3F, {"cat_b2s2", "Catalyst temp B2S2", "C", dec_catalyst_temp}},
    {0x42, {"ctrl_volt", "Control module V", "V", dec_control_voltage}},
    {0x43, {"abs_load", "Absolute load", "%", dec_percent}},
    {0x44, {"equiv_ratio", "Commanded lambda", "", dec_equiv_ratio}},
    {0x45, {"rel_thr", "Relative throttle", "%", dec_percent}},
    {0x46, {"ambient", "Ambient air temp", "C", dec_temp_d}},
    {0x47, {"thr_b", "Absolute throttle B", "%", dec_percent}},
    {0x48, {"thr_c", "Absolute throttle C", "%", dec_percent}},
    {0x49, {"pedal_d", "Accel pedal D", "%", dec_percent}},
    {0x4A, {"pedal_e", "Accel pedal E", "%", dec_percent}},
    {0x4B, {"pedal_f", "Accel pedal F", "%", dec_percent}},
    {0x4C, {"thr_actuator", "Commanded throttle", "%", dec_percent}},
    {0x4D, {"time_mil", "Time with MIL on", "min", dec_u16}},
    {0x4E, {"time_clear", "Time since clear", "min", dec_u16}},
    {0x52, {"ethanol", "Ethanol fuel", "%", dec_percent}},
    {0x59, {"rail_abs", "Fuel rail abs press.", "kPa", dec_rail_gauge}},
    {0x5A, {"pedal", "Accelerator pedal", "%", dec_percent}},
    {0x5B, {"hybrid_soc", "Hybrid batt. life", "%", dec_percent}},
    {0x5C, {"oil", "Oil temp", "C", dec_temp_d}},
    {0x5D, {"inject_timing", "Fuel injection timing", "deg", dec_inject_timing}},
    {0x5E, {"fuel_rate", "Engine fuel rate", "L/h", dec_fuel_rate}},
    {0x61, {"torque_demand", "Driver demand torque", "%", dec_torque_demand_d}},
    {0x62, {"act_torque", "Actual torque", "%", dec_torque_pct_d}},
    {0x63, {"ref_torque", "Reference torque", "Nm", dec_ref_torque}},
    };
    return t;
}

// PIDs the gauge draws - polled between every other reading so the needle
// stays responsive. 0x1F (run time) is not a display channel at all: it is
// how ignition catches an engine that restarted while we were disconnected.
const std::vector<uint8_t> kPollFast = {0x0C, 0x0D, 0x11, 0x1F};
// Preferred order for the rest, so display-relevant ones refresh soonest.
const std::vector<uint8_t> kPollPriority = {0x05, 0x0F, 0x5E, 0x04, 0x42,
                                            0x63, 0x62, 0x5C, 0x3C, 0x33};

bool contains(const std::vector<uint8_t>& v, uint8_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

}  // namespace

const PidInfo* pid_info(uint8_t pid) {
    auto it = table().find(pid);
    return it == table().end() ? nullptr : &it->second;
}

std::optional<Reading> decode(uint8_t pid, const Bytes& data) {
    const PidInfo* info = pid_info(pid);
    if (!info) return std::nullopt;
    auto v = info->decoder(data);
    if (!v) return std::nullopt;
    return Reading{info->key, *v};
}

std::set<std::string> keys_for(const std::set<uint8_t>& supported) {
    std::set<std::string> out;
    for (uint8_t pid : supported) {
        const PidInfo* info = pid_info(pid);
        if (info) out.insert(info->key);
    }
    return out;
}

std::set<uint8_t> parse_supported(const Bytes& data, uint8_t base) {
    std::set<uint8_t> out;
    if (data.size() < 4) return out;
    for (int i = 0; i < 4; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            if (data[static_cast<size_t>(i)] & (0x80 >> bit)) {
                out.insert(static_cast<uint8_t>(base + i * 8 + bit + 1));
            }
        }
    }
    return out;
}

std::vector<uint8_t> build_poll_cycle(const std::set<uint8_t>& supported, bool log_all) {
    std::vector<uint8_t> fast;
    for (uint8_t p : kPollFast) {
        if (supported.count(p)) fast.push_back(p);
    }
    std::vector<uint8_t> rest;
    for (uint8_t p : kPollPriority) {
        if (supported.count(p) && !contains(fast, p)) rest.push_back(p);
    }
    if (log_all) {
        for (uint8_t p : supported) {   // std::set iterates sorted, as Python's sorted()
            if (pid_info(p) && !contains(fast, p) && !contains(rest, p)) rest.push_back(p);
        }
    }
    if (fast.empty() && rest.empty()) return {};
    if (rest.empty()) return fast;
    std::vector<uint8_t> cycle;
    for (uint8_t pid : rest) {
        cycle.insert(cycle.end(), fast.begin(), fast.end());
        cycle.push_back(pid);
    }
    return cycle;
}

}  // namespace gauge
