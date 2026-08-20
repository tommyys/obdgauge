#include "vehicle.h"
#include <cstring>
#include <cctype>
#include <algorithm>
#include <map>

namespace gauge {
namespace {

const char* kVinAlphabet = "0123456789ABCDEFGHJKLMNPRSTUVWXYZ";
const char* kYearLetters = "ABCDEFGHJKLMNPRSTVWXY";
const char* kYearDigits  = "123456789";

const std::map<std::string, std::string>& wmi_table() {
    static const std::map<std::string, std::string> t = {
        {"19U", "Acura"},
        {"19X", "Honda"},
        {"1C4", "Chrysler"},
        {"1C6", "Ram"},
        {"1FA", "Ford"},
        {"1FM", "Ford"},
        {"1FT", "Ford"},
        {"1G1", "Chevrolet"},
        {"1GC", "Chevrolet"},
        {"1HG", "Honda"},
        {"1J4", "Jeep"},
        {"1N4", "Nissan"},
        {"1N6", "Nissan"},
        {"2C3", "Chrysler"},
        {"2HG", "Honda"},
        {"2HK", "Honda"},
        {"2T2", "Lexus"},
        {"3FA", "Ford"},
        {"3MZ", "Mazda"},
        {"4F2", "Mazda"},
        {"4F4", "Mazda"},
        {"4S3", "Subaru"},
        {"4S4", "Subaru"},
        {"4T1", "Toyota"},
        {"58A", "Lexus"},
        {"5FN", "Honda"},
        {"5J6", "Honda"},
        {"5N1", "Nissan"},
        {"5TD", "Toyota"},
        {"JA3", "Mitsubishi"},
        {"JA4", "Mitsubishi"},
        {"JAA", "Isuzu"},
        {"JF1", "Subaru"},
        {"JF2", "Subaru"},
        {"JHG", "Honda"},
        {"JHL", "Honda"},
        {"JHM", "Honda"},
        {"JM0", "Mazda"},
        {"JM1", "Mazda"},
        {"JM3", "Mazda"},
        {"JM6", "Mazda"},
        {"JM7", "Mazda"},
        {"JMZ", "Mazda"},
        {"JN1", "Nissan"},
        {"JN6", "Nissan"},
        {"JN8", "Nissan"},
        {"JS2", "Suzuki"},
        {"JS3", "Suzuki"},
        {"JT2", "Toyota"},
        {"JT3", "Toyota"},
        {"JT4", "Toyota"},
        {"JTD", "Toyota"},
        {"JTE", "Toyota"},
        {"JTG", "Toyota"},
        {"JTH", "Lexus"},
        {"JTJ", "Lexus"},
        {"JTK", "Toyota"},
        {"JTL", "Toyota"},
        {"JTM", "Toyota"},
        {"JTN", "Toyota"},
        {"KM8", "Hyundai"},
        {"KMH", "Hyundai"},
        {"KNA", "Kia"},
        {"KND", "Kia"},
        {"KNE", "Kia"},
        {"KNM", "Renault Samsung"},
        {"L6T", "Zotye"},
        {"LB3", "Geely"},
        {"LC0", "BYD"},
        {"LFV", "FAW-VW"},
        {"LGX", "BYD"},
        {"LSJ", "MG"},
        {"LSV", "SAIC Volkswagen"},
        {"LVS", "Ford China"},
        {"LVV", "Chery"},
        {"LYV", "Volvo"},
        {"MA3", "Suzuki"},
        {"ML3", "Mitsubishi"},
        {"MMB", "Mitsubishi"},
        {"MP1", "Isuzu"},
        {"MPA", "Isuzu"},
        {"MR0", "Toyota"},
        {"MRH", "Honda"},
        {"PL1", "Proton"},
        {"PM2", "Perodua"},
        {"SAJ", "Jaguar"},
        {"SAL", "Land Rover"},
        {"SCC", "Lotus"},
        {"SHH", "Honda"},
        {"TMB", "Skoda"},
        {"TRU", "Audi"},
        {"VF1", "Renault"},
        {"VF3", "Peugeot"},
        {"VF7", "Citroen"},
        {"VSK", "Nissan"},
        {"VSS", "SEAT"},
        {"W1K", "Mercedes-Benz"},
        {"W1N", "Mercedes-Benz"},
        {"WA1", "Audi"},
        {"WAU", "Audi"},
        {"WBA", "BMW"},
        {"WBS", "BMW"},
        {"WBY", "BMW"},
        {"WDB", "Mercedes-Benz"},
        {"WDC", "Mercedes-Benz"},
        {"WDD", "Mercedes-Benz"},
        {"WF0", "Ford"},
        {"WMW", "MINI"},
        {"WP0", "Porsche"},
        {"WP1", "Porsche"},
        {"WV1", "Volkswagen"},
        {"WV2", "Volkswagen"},
        {"WVG", "Volkswagen"},
        {"WVW", "Volkswagen"},
        {"YV1", "Volvo"},
        {"YV4", "Volvo"},
        {"ZAR", "Alfa Romeo"},
        {"ZFA", "Fiat"},
    };
    return t;
}

const std::map<std::string, DialProfile>& profiles() {
    static const std::map<std::string, DialProfile> t = {
        {"BMW", {7000, 6500, 250}},
        {"Honda", {7000, 6500, 150}},
        {"Honda Civic", {7000, 6500, 130}},
        {"Lexus", {6500, 6000, 190}},
        {"Mazda", {7000, 6200, 150}},
        {"Mazda MX-5", {8000, 7000, 140}},
        {"Perodua", {7000, 6000, 100}},
        {"Porsche", {8000, 7200, 300}},
        {"Proton", {7000, 6000, 130}},
        {"Toyota", {6500, 6000, 150}},
    };
    return t;
}

const std::map<std::string, std::string>& model_hints() {
    static const std::map<std::string, std::string> t = {
    };
    return t;
}

std::string upper(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return o;
}

}  // namespace

std::string clean_vin(const std::string& text) {
    std::string out;
    for (char c : upper(text)) {
        for (const char* p = kVinAlphabet; *p; ++p) {
            if (c == *p) { out += c; break; }
        }
    }
    return out;
}

bool valid_vin(const std::string& vin) { return vin.size() == 17; }

std::optional<int> model_year(const std::string& vin, int now_year) {
    if (!valid_vin(vin)) return std::nullopt;
    char c = vin[9];
    int year;
    const char* pos = std::strchr(kYearLetters, c);
    if (c != '\0' && pos != nullptr) {
        year = 2010 + static_cast<int>(pos - kYearLetters);
    } else {
        const char* dpos = std::strchr(kYearDigits, c);
        if (c != '\0' && dpos != nullptr) {
            year = 2031 + static_cast<int>(dpos - kYearDigits);
        } else {
            return std::nullopt;
        }
    }
    // Each code has two readings 30 years apart; take the recent one.
    // now_year + 1 is allowed: cars are sold ahead of their model year.
    while (year > now_year + 1) year -= 30;
    return year;
}

std::string make_of(const std::string& vin) {
    if (vin.size() < 3) return "";
    auto it = wmi_table().find(vin.substr(0, 3));
    return it == wmi_table().end() ? "" : it->second;
}

std::string model_hint(const std::string& vin) {
    if (vin.empty()) return "";
    std::string best_prefix, best_name;
    for (const auto& kv : model_hints()) {
        if (vin.rfind(kv.first, 0) == 0 && kv.first.size() > best_prefix.size()) {
            best_prefix = kv.first;
            best_name = kv.second;
        }
    }
    return best_name;
}

DialProfile profile_for(const std::string& make, const std::string& model) {
    if (!make.empty() && !model.empty()) {
        auto it = profiles().find(make + " " + model);
        if (it != profiles().end()) return it->second;
    }
    if (!make.empty()) {
        auto it = profiles().find(make);
        if (it != profiles().end()) return it->second;
    }
    return kDefaultProfile;
}

Identity identify(const std::string& vin_in, const std::string& make_in,
                  const std::string& model_in, std::optional<int> year_in,
                  const std::string& source, int now_year) {
    std::string vin = clean_vin(vin_in);
    bool ok = valid_vin(vin);
    std::string wmi = ok ? vin.substr(0, 3) : "";

    std::string make = make_in, model = model_in;
    std::optional<int> year = year_in;
    if (make.empty() && ok) make = make_of(vin);
    if (model.empty() && ok) model = model_hint(vin);
    if (!year && ok) year = model_year(vin, now_year);

    // An unrecognised WMI is still information - show the code rather than
    // pretending we have no idea what car this is.
    std::string label;
    if (!make.empty())      label = upper(make);
    else if (!wmi.empty())  label = "WMI " + wmi;
    else                    label = "OBD-II";
    if (!model.empty()) label += " " + upper(model);

    Identity out;
    DialProfile p = profile_for(make, model);
    out.rpm_max = p.rpm_max;
    out.rpm_red = p.rpm_red;
    out.power_max = p.power_max;
    out.vin = ok ? vin : "";
    out.wmi = wmi;
    out.make = make;
    out.model = model;
    out.year = year;
    out.label = label;
    out.source = source;
    out.known = !make.empty();
    return out;
}

}  // namespace gauge
