// Vehicle identity and per-car dial profiles (SPEC.md section 10).
// Ported from mx5gauge/vehicle.py — the Python is authoritative.
#pragma once
#include <optional>
#include <set>
#include <string>

namespace gauge {

struct DialProfile {
    double rpm_max;
    double rpm_red;
    double power_max;
};

inline constexpr DialProfile kDefaultProfile{8000, 6800, 160};

// The VIN year code cycles every 30 years, so decoding it needs to know
// roughly what year it is. On the board this should come from an RTC or GPS
// once one exists; until then it is a compile-time constant, and a stale one
// only misreads VINs more than a year in the future.
inline constexpr int kThisYear = 2026;

struct Identity {
    std::string vin;      // empty when absent or invalid
    std::string wmi;
    std::string make;
    std::string model;
    std::string label;    // what the banner renders
    std::string source;
    std::optional<int> year;
    bool known = false;
    double rpm_max   = kDefaultProfile.rpm_max;
    double rpm_red   = kDefaultProfile.rpm_red;
    double power_max = kDefaultProfile.power_max;
};

// Strip a VIN down to legal characters, upper-cased.
std::string clean_vin(const std::string& text);
// True for a plausible 17-character VIN. The check digit is not verified:
// it is only mandatory in North America.
bool valid_vin(const std::string& vin);
// Model year from VIN position 10, or absent.
std::optional<int> model_year(const std::string& vin, int now_year = kThisYear);
// Manufacturer from the WMI, or "" if unrecognised.
std::string make_of(const std::string& vin);
// Model name for a known VIN prefix, or "". Longest match wins.
std::string model_hint(const std::string& vin);
// Dial limits: "Make Model" beats "Make" beats the default.
DialProfile profile_for(const std::string& make, const std::string& model = "");

// Explicit make/model/year always win over what the VIN says.
Identity identify(const std::string& vin = "", const std::string& make = "",
                  const std::string& model = "",
                  std::optional<int> year = std::nullopt,
                  const std::string& source = "", int now_year = kThisYear);

}  // namespace gauge
