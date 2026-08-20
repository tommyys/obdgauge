// Cross-validate the C++ core against the Python simulator: both consume the
// same capture, and every sample must agree. Turns "did the port stay
// faithful?" from a judgement call into a diff.
//
//   .venv/bin/python tools/dump_python_states.py logs/<drive>.csv > ref.txt
//   ./build/replay_check logs/<drive>.csv ref.txt
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "metrics.h"
#include "state.h"

namespace {

constexpr double kTol = 1e-6;

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

std::optional<double> parse_ref(const std::string& tok) {
    if (tok == "None" || tok.empty()) return std::nullopt;
    return std::strtod(tok.c_str(), nullptr);
}

bool agree(const std::optional<double>& a, const std::optional<double>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    double d = std::fabs(*a - *b);
    return d <= kTol || d <= kTol * std::fabs(*b);
}

std::string show(const std::optional<double>& v) {
    if (!v) return "None";
    char b[32];
    snprintf(b, sizeof b, "%.9g", *v);
    return b;
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

    char line[1024];
    std::vector<std::string> names;
    if (fgets(line, sizeof line, ref)) names = split(line, ',');   // header
    fgets(line, sizeof line, cap);                                  // header

    gauge::VehicleState st;
    gauge::Trip trip;
    gauge::DrivingScore score;

    long samples = 0, divergences = 0;
    while (fgets(line, sizeof line, cap)) {
        auto row = split(line, ',');
        if (row.size() < 4) continue;
        const std::string& key = row[2];
        char* end = nullptr;
        double value = std::strtod(row[3].c_str(), &end);
        if (end == row[3].c_str()) continue;   // non-numeric, as Python skips
        double t = std::strtod(row[1].c_str(), nullptr);

        // Same order as state.Gauge.sample: an implausible reading is dropped
        // and does not advance the metrics.
        if (!gauge::plausible(key, value)) { st.set(key, value); continue; }
        st.set(key, value);
        trip.update(t, st.get("speed"), st.get("fuel_rate"));
        score.update(t, st.get("speed"), st.get("rpm"), st.get("throttle"),
                     st.get("fuel_rate"));

        if (!fgets(line, sizeof line, ref)) {
            fprintf(stderr, "reference ended early at sample %ld\n", samples);
            return 1;
        }
        auto want = split(line, ',');
        const std::optional<double> got[] = {
            st.get("rpm"), st.get("speed"), st.get("coolant"),
            st.get("throttle"), st.get("fuel_rate"), st.get("power_kw"),
            trip.dist_km, trip.fuel_l, static_cast<double>(score.harsh),
            score.smooth(), score.econ(), score.calm(), score.total(),
            static_cast<double>(st.rejected()), st.peak_rpm()};
        const size_t n = sizeof got / sizeof got[0];
        for (size_t i = 0; i < n && i < want.size(); ++i) {
            auto expect = parse_ref(want[i]);
            if (!agree(got[i], expect)) {
                if (++divergences <= 10) {
                    fprintf(stderr,
                            "sample %ld  %-10s got %-14s want %-14s  (key=%s)\n",
                            samples, i < names.size() ? names[i].c_str() : "?",
                            show(got[i]).c_str(), show(expect).c_str(), key.c_str());
                }
            }
        }
        ++samples;
    }
    fclose(cap);
    fclose(ref);
    printf("%s: %ld samples, %ld divergences\n", argv[1], samples, divergences);
    return divergences == 0 ? 0 : 1;
}
