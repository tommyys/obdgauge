#include "state.h"
#include <cmath>

namespace gauge {
namespace {

// Physically plausible range per channel. Machine-extracted from
// mx5gauge/state.py RANGES, including the percent/catalyst/pressure groups
// that the Python fills in with loops.
const std::map<std::string, std::pair<double, double>>& ranges() {
    static const std::map<std::string, std::pair<double, double>> r = {
        {"abs_load", {0.0, 400.0}},
        {"act_torque", {-125.0, 130.0}},
        {"ambient", {-50.0, 100.0}},
        {"baro", {0.0, 200.0}},
        {"cat_b1s1", {-40.0, 1400.0}},
        {"cat_b1s2", {-40.0, 1400.0}},
        {"cat_b2s1", {-40.0, 1400.0}},
        {"cat_b2s2", {-40.0, 1400.0}},
        {"catalyst", {0.0, 1400.0}},
        {"coolant", {-50.0, 200.0}},
        {"ctrl_volt", {4.0, 20.0}},
        {"egr_cmd", {-100.0, 100.0}},
        {"egr_err", {-100.0, 100.0}},
        {"equiv_ratio", {0.0, 4.0}},
        {"ethanol", {0.0, 100.0}},
        {"evap_press", {-9000.0, 9000.0}},
        {"evap_purge", {-100.0, 100.0}},
        {"fuel_level", {0.0, 100.0}},
        {"fuel_press", {0.0, 800000.0}},
        {"fuel_rail_temp", {-50.0, 250.0}},
        {"fuel_rate", {0.0, 120.0}},
        {"hybrid_soc", {-100.0, 100.0}},
        {"inject_timing", {-220.0, 302.0}},
        {"intake", {-50.0, 200.0}},
        {"load", {0.0, 100.0}},
        {"ltft1", {-100.0, 100.0}},
        {"ltft2", {-100.0, 100.0}},
        {"maf", {0.0, 700.0}},
        {"map", {0.0, 400.0}},
        {"o2_b1s1", {0.0, 1.3}},
        {"o2_b1s2", {0.0, 1.3}},
        {"oil", {-50.0, 250.0}},
        {"pedal", {-100.0, 100.0}},
        {"pedal_d", {-100.0, 100.0}},
        {"pedal_e", {-100.0, 100.0}},
        {"pedal_f", {-100.0, 100.0}},
        {"power_kw", {0.0, 400.0}},
        {"rail_abs", {0.0, 800000.0}},
        {"rail_gauge", {0.0, 800000.0}},
        {"rail_press", {0.0, 800000.0}},
        {"ref_torque", {0.0, 2000.0}},
        {"rel_thr", {-100.0, 100.0}},
        {"rpm", {0.0, 9000.0}},
        {"speed", {0.0, 320.0}},
        {"stft1", {-100.0, 100.0}},
        {"stft2", {-100.0, 100.0}},
        {"thr_actuator", {-100.0, 100.0}},
        {"thr_b", {-100.0, 100.0}},
        {"thr_c", {-100.0, 100.0}},
        {"throttle", {0.0, 100.0}},
        {"timing", {-70.0, 70.0}},
        {"torque_demand", {-125.0, 130.0}},
        {"volts", {4.0, 20.0}},
    };
    return r;
}

// The catalyst temperature can arrive on any of several channels depending on
// which banks and sensors the car reports. A peak takes the hottest of
// whichever turned up: the drive only got as hot as it got, whatever bank
// happened to measure it.
const std::map<std::string, std::string>& peak_fields() {
    static const std::map<std::string, std::string> f = {
        {"rpm", "rpm"}, {"speed", "speed"}, {"coolant", "coolant"},
        {"intake", "intake"},
        {"catalyst", "catalyst"}, {"cat_b1s1", "catalyst"}, {"cat_b2s1", "catalyst"},
        {"cat_b1s2", "catalyst"}, {"cat_b2s2", "catalyst"},
    };
    return f;
}

}  // namespace

bool plausible(const std::string& key, double value) {
    if (!std::isfinite(value)) return false;
    auto it = ranges().find(key);
    double lo = -1e7, hi = 1e7;   // permissive default for unknown channels
    if (it != ranges().end()) { lo = it->second.first; hi = it->second.second; }
    return lo <= value && value <= hi;
}

void VehicleState::set(const std::string& key, double value) {
    if (!plausible(key, value)) { ++rejected_; return; }
    values_[key] = value;
    // High-water marks, deliberately below the plausibility gate: a peak is
    // the most memorable number on the summary card, and one bad frame would
    // otherwise pin it there for the whole drive.
    auto it = peak_fields().find(key);
    if (it != peak_fields().end()) {
        auto cur = peaks_.find(it->second);
        if (cur == peaks_.end() || value > cur->second) peaks_[it->second] = value;
    }
    derive();
}

double VehicleState::peak_rpm() const {
    auto it = peaks_.find("rpm");
    return it == peaks_.end() ? 0.0 : it->second;
}

std::optional<double> VehicleState::peak(const std::string& field) const {
    auto it = peaks_.find(field);
    if (it == peaks_.end()) return std::nullopt;
    return it->second;
}

void VehicleState::derive() {
    // Engine power estimate (kW) from torque % x reference torque x rpm.
    // The 3.14159 literal matches state.py exactly; using M_PI here would
    // put a small drift between the firmware and the simulator.
    auto at = values_.find("act_torque");
    auto rt = values_.find("ref_torque");
    auto rp = values_.find("rpm");
    if (at == values_.end() || rt == values_.end() || rp == values_.end()) return;
    double nm = rt->second * std::fmax(0.0, at->second) / 100.0;
    double kw = nm * rp->second * 2 * 3.14159 / 60.0 / 1000.0;
    values_["power_kw"] = kw;
    if (kw > peak_kw_) peak_kw_ = kw;
}

std::optional<double> VehicleState::get(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

bool VehicleState::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

}  // namespace gauge
