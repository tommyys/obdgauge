// Cross-validate the C++ core against the Python simulator: both consume the
// same capture, and every sample must agree on EVERY channel and EVERY
// derived value. A field one side has and the other lacks is a divergence
// too, so this cannot quietly miss a channel nobody listed.
//
//   .venv/bin/python tools/dump_python_states.py logs/<drive>.csv > ref.txt
//   ./build/replay_check logs/<drive>.csv ref.txt
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "metrics.h"
#include "state.h"

namespace {

// Tight enough to catch a constant drifting in the last few digits -
// 3.14159 vs M_PI is a relative error of ~8e-7 and slipped under 1e-6.
constexpr double kTol = 1e-12;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else if (c != '\r' && c != '\n') { cur += c; }
    }
    out.push_back(cur);
    return out;
}

std::map<std::string, std::string> parse_fields(const std::string& s) {
    std::map<std::string, std::string> out;
    for (const auto& kv : split(s, ';')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        out[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return out;
}

std::string fmt(const std::optional<double>& v) {
    if (!v) return "None";
    char b[32];
    snprintf(b, sizeof b, "%.17g", *v);
    return b;
}

// Numeric where both sides are numeric, textual otherwise (the coach word).
bool agree(const std::string& got, const std::string& want) {
    if (got == want) return true;
    if (got == "None" || want == "None") return false;
    char* ge = nullptr;
    char* we = nullptr;
    double g = std::strtod(got.c_str(), &ge);
    double w = std::strtod(want.c_str(), &we);
    if (ge == got.c_str() || we == want.c_str()) return false;   // not numeric
    double d = std::fabs(g - w);
    return d <= kTol || d <= kTol * std::fabs(w);
}

long compare(const std::map<std::string, std::string>& got,
             const std::map<std::string, std::string>& want,
             const char* section, long sample, const std::string& key,
             long* shown) {
    long bad = 0;
    for (const auto& kv : want) {
        auto it = got.find(kv.first);
        if (it == got.end()) {
            ++bad;
            if ((*shown)++ < 20)
                fprintf(stderr, "sample %ld  %s.%s MISSING in C++ (want %s) [key=%s]\n",
                        sample, section, kv.first.c_str(), kv.second.c_str(), key.c_str());
            continue;
        }
        if (!agree(it->second, kv.second)) {
            ++bad;
            if ((*shown)++ < 20)
                fprintf(stderr, "sample %ld  %s.%-14s got %-14s want %-14s [key=%s]\n",
                        sample, section, kv.first.c_str(), it->second.c_str(),
                        kv.second.c_str(), key.c_str());
        }
    }
    for (const auto& kv : got) {
        if (want.find(kv.first) == want.end()) {
            ++bad;
            if ((*shown)++ < 20)
                fprintf(stderr, "sample %ld  %s.%s EXTRA in C++ (got %s) [key=%s]\n",
                        sample, section, kv.first.c_str(), kv.second.c_str(), key.c_str());
        }
    }
    return bad;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: replay_check <capture.csv> <reference.txt>\n");
        return 2;
    }
    FILE* cap = fopen(argv[1], "r");
    FILE* ref = fopen(argv[2], "r");
    if (!cap || !ref) { fprintf(stderr, "cannot open inputs\n"); return 2; }

    char line[8192];
    fgets(line, sizeof line, cap);   // capture header

    gauge::VehicleState st;
    gauge::Trip trip;
    gauge::DrivingScore score;

    long samples = 0, divergences = 0, shown = 0;
    long channels_seen = 0;
    while (fgets(line, sizeof line, cap)) {
        auto row = split(line, ',');
        if (row.size() < 4) continue;
        const std::string key = row[2];
        char* end = nullptr;
        double value = std::strtod(row[3].c_str(), &end);
        if (end == row[3].c_str()) continue;   // non-numeric, as Python skips
        double t = std::strtod(row[1].c_str(), nullptr);

        // Same order as state.Gauge.sample: an implausible reading is dropped
        // and does not advance the metrics.
        bool ok = gauge::plausible(key, value);
        st.set(key, value);
        if (ok) {
            trip.update(t, st.get("speed"), st.get("fuel_rate"));
            // The IMU is fed on its own arrival, not folded into the OBD
            // update: on the board it is read far faster than any OBD
            // channel, and jerk is the whole reason for using it. Mirrors
            // state.IMU_KEYS in the Python.
            if (key == "imu_ax" || key == "imu_ay" || key == "imu_az")
                score.imu(t, st.get("imu_ax"), st.get("imu_ay"),
                          st.get("imu_az"));
            score.update(t, st.get("speed"), st.get("rpm"), st.get("throttle"),
                         st.get("fuel_rate"), st.get("coolant"));
        }

        if (!fgets(line, sizeof line, ref)) {
            fprintf(stderr, "reference ended early at sample %ld\n", samples);
            return 1;
        }
        auto halves = split(line, '\t');
        if (halves.size() < 2) { fprintf(stderr, "malformed reference line\n"); return 2; }

        std::map<std::string, std::string> got_ch;
        for (const auto& kv : st.values()) got_ch[kv.first] = fmt(kv.second);
        channels_seen = static_cast<long>(got_ch.size());

        std::map<std::string, std::string> got_dv = {
            {"dist_km", fmt(trip.dist_km)},
            {"fuel_l", fmt(trip.fuel_l)},
            {"moving_s", fmt(trip.moving_s)},
            {"elapsed_s", fmt(trip.elapsed_s)},
            {"cost_rm", fmt(trip.cost_rm())},
            {"econ_l_per_100", fmt(trip.econ_l_per_100())},
            {"econ_km_per_l", fmt(trip.econ_km_per_l())},
            {"avg_speed_kph", fmt(trip.avg_speed_kph())},
            {"thr_s", fmt(score.thr_s)},
            {"thr_bad", fmt(score.thr_bad)},
            {"brake_s", fmt(score.brake_s)},
            {"brake_bad", fmt(score.brake_bad)},
            {"corner_s", fmt(score.corner_s)},
            {"corner_bad", fmt(score.corner_bad)},
            {"care_s", fmt(score.care_s)},
            {"care_bad", fmt(score.care_bad)},
            {"rev_s", fmt(score.rev_s)},
            {"intensity", fmt(score.intensity)},
            {"pole", score.pole == gauge::Pole::Spirited ? "SPIRITED" : "NICE"},
            {"nice_s", fmt(score.nice_s)},
            {"spirited_s", fmt(score.spirited_s)},
            {"n_events", fmt(static_cast<double>(score.events.size()))},
            // The Python dump writes a bool through the same numeric
            // formatter, so it lands as 0 or 1. Match it.
            {"g_ready", fmt(static_cast<double>(score.g.ready()))},
            {"g_lat", fmt(score.g.lat)},
            {"g_lon", fmt(score.g.lon)},
            {"g_peak_lat", fmt(score.g.peak_lat)},
            {"g_peak_brake", fmt(score.g.peak_lon_brake)},
            {"throttle", fmt(score.throttle())},
            {"braking", fmt(score.braking())},
            {"cornering", fmt(score.cornering())},
            {"care", fmt(score.care())},
            {"total", fmt(score.total())},
            {"coach", score.coach()},
            {"rejected", fmt(static_cast<double>(st.rejected()))},
            {"peak_rpm", fmt(st.peak_rpm())},
            {"peak_kw", fmt(st.peak_kw())},
            {"peak_speed", fmt(st.peak("speed"))},
            {"peak_coolant", fmt(st.peak("coolant"))},
            {"peak_intake", fmt(st.peak("intake"))},
            {"peak_catalyst", fmt(st.peak("catalyst"))},
        };

        divergences += compare(got_ch, parse_fields(halves[0]), "chan", samples, key, &shown);
        divergences += compare(got_dv, parse_fields(halves[1]), "derived", samples, key, &shown);
        ++samples;
    }
    fclose(cap);
    fclose(ref);
    printf("%s: %ld samples, %ld channels, %ld derived fields, %ld divergences\n",
           argv[1], samples, channels_seen, 29L, divergences);
    return divergences == 0 ? 0 : 1;
}
