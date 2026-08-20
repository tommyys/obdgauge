// Latest readings plus the plausibility gate.
// Ported from mx5gauge/state.py — the Python is authoritative.
#pragma once
#include <map>
#include <optional>
#include <string>

namespace gauge {

// True if `value` is a sane reading for `key`. Unknown keys get a permissive
// default so newly-added channels still get logged; only obvious nonsense
// (NaN, inf, absurd magnitudes) is dropped.
bool plausible(const std::string& key, double value);

class VehicleState {
  public:
    // Implausible values are counted and dropped, leaving any previous good
    // reading in place: a failed poll must not blank the display.
    void set(const std::string& key, double value);
    std::optional<double> get(const std::string& key) const;
    bool has(const std::string& key) const;

    int    rejected() const { return rejected_; }
    double peak_rpm() const { return peak_rpm_; }
    double peak_kw()  const { return peak_kw_; }

    const std::map<std::string, double>& values() const { return values_; }

  private:
    void derive();

    std::map<std::string, double> values_;
    int    rejected_ = 0;
    double peak_rpm_ = 0.0;
    double peak_kw_  = 0.0;
};

}  // namespace gauge
