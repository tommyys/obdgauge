// Latest readings plus the plausibility gate.
// Ported from mx5gauge/state.py — the Python is authoritative.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

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

    int rejected() const { return rejected_; }

    // Highest rpm this drive, or 0.0 before any rev is seen. Zero rather than
    // absent because the tacho and the score footer draw it unconditionally.
    double peak_rpm() const;
    double peak_kw() const { return peak_kw_; }

    // High-water mark for a summary field ("rpm", "speed", "coolant",
    // "intake", "catalyst"), or absent when the channel never arrived -- which
    // is the honest answer for a car that never reported it. A bold 0 would
    // lie where '--' tells the truth.
    std::optional<double> peak(const std::string& field) const;
    const std::map<std::string, double>& peaks() const { return peaks_; }

    const std::map<std::string, double>& values() const { return values_; }

  private:
    void derive();

    std::map<std::string, double> values_;
    std::map<std::string, double> peaks_;
    int    rejected_ = 0;
    double peak_kw_  = 0.0;
};

}  // namespace gauge
