// Ported from tests/test_ignition.py, including the real-drive fixture cut
// from logs/drive-20260816-064348.csv.
#include "check.h"
#include "ignition.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using gauge_test::check;
using gauge::IgnitionEvent;

static const char* name_of(IgnitionEvent e) {
    return e == IgnitionEvent::On ? "on" : e == IgnitionEvent::Off ? "off" : "";
}

// Feed a running-engine stretch: PIDs answering, alternator voltage.
static std::vector<std::string> running(gauge::Ignition& ign, double t0,
                                        double seconds, double volts = 13.9,
                                        double step = 0.077) {
    std::vector<std::string> out;
    double t = t0;
    const std::pair<const char*, double> chans[] = {
        {"rpm", 2200.0}, {"speed", 60.0}, {"throttle", 20.0}};
    while (t < t0 + seconds) {
        for (const auto& c : chans) {
            auto e = ign.update(t, c.first, c.second);
            if (e != IgnitionEvent::None) out.push_back(name_of(e));
            t += step;
        }
        if (static_cast<int>(t - t0) % 5 == 0) {
            auto e = ign.update(t, "volts", volts);
            if (e != IgnitionEvent::None) out.push_back(name_of(e));
        }
    }
    return out;
}

// Feed an ignition-off stretch: nothing but ATRV voltage reads.
static std::vector<std::string> parked(gauge::Ignition& ign, double t0,
                                       double seconds, double volts = 12.5,
                                       double step = 28.5) {
    std::vector<std::string> out;
    double t = t0;
    while (t < t0 + seconds) {
        auto e = ign.update(t, "volts", volts);
        if (e != IgnitionEvent::None) out.push_back(name_of(e));
        t += step;
    }
    return out;
}

static std::vector<std::string> one(gauge::Ignition& ign, double t,
                                    const char* key, double v) {
    std::vector<std::string> out;
    auto e = ign.update(t, key, v);
    if (e != IgnitionEvent::None) out.push_back(name_of(e));
    return out;
}

static double secs(const std::string& hms) {
    return std::stoi(hms.substr(0, 2)) * 3600 + std::stoi(hms.substr(3, 2)) * 60 +
           std::stod(hms.substr(6));
}

int main() {
    using V = std::vector<std::string>;
    // -- the clean case ----------------------------------------------------
    gauge::Ignition ign;
    check("a running engine raises nothing", running(ign, 0.0, 60.0), V{});
    check("...and is not believed to be off", ign.off(), false);

    auto evs = parked(ign, 60.0, 400.0);
    check("silence with resting volts is an ignition-off", evs, V{"off"});
    check("...and the detector holds that belief", ign.off(), true);
    check("the first PID reply back is an ignition-on",
          one(ign, 460.0, "rpm", 780.0), V{"on"});
    check("...and the engine is believed running again", ign.off(), false);

    // -- silence with no volts is a dead link, not ignition -----------------
    gauge::Ignition dead;
    running(dead, 0.0, 60.0);
    check("silence with no volts at all is a dead link, not ignition",
          one(dead, 300.0, "rpm", 780.0), V{});

    // -- silence at alternator voltage is a stalled poll --------------------
    gauge::Ignition stalled;
    running(stalled, 0.0, 60.0);
    check("silence at alternator voltage is a stalled poll, not ignition",
          parked(stalled, 30.0, 200.0, 13.9), V{});

    // -- a low reading while PIDs answer is not ignition --------------------
    gauge::Ignition low;
    check("a low reading with the PIDs still answering is not ignition",
          running(low, 0.0, 60.0, 12.5), V{});

    // -- a brief gap under the threshold ------------------------------------
    gauge::Ignition brief;
    running(brief, 0.0, 30.0);
    V got;
    for (auto& e : one(brief, 35.0, "volts", 12.5)) got.push_back(e);
    for (auto& e : one(brief, 35.5, "rpm", 2200.0)) got.push_back(e);
    check("a brief gap under the silence threshold is not ignition", got, V{});

    // -- run_time -----------------------------------------------------------
    gauge::Ignition rt;
    rt.update(10.0, "run_time", 1245.0);
    check("run_time climbing normally is silent",
          one(rt, 11.0, "run_time", 1246.0), V{});
    check("run_time going backwards is an ignition-on, with no off seen first",
          one(rt, 12.0, "run_time", 25.0), V{"on"});
    gauge::Ignition first;
    check("the first run_time of a session is not a reset",
          one(first, 0.0, "run_time", 500.0), V{});

    // -- the real drive -----------------------------------------------------
    gauge::Ignition real;
    std::vector<std::pair<std::string, std::string>> seen;   // (event, hh:mm:ss)
    FILE* fh = fopen("../../../tests/fixtures/ignition-edges.csv", "r");
    check("fixture opens", fh != nullptr, true);
    if (fh) {
        char line[512];
        bool header = true;
        while (fgets(line, sizeof line, fh)) {
            if (header) { header = false; continue; }
            char* iso = strtok(line, ",");
            char* ts  = strtok(nullptr, ",");
            char* key = strtok(nullptr, ",");
            char* val = strtok(nullptr, ",\n\r");
            if (!iso || !ts || !key || !val) continue;
            char* end = nullptr;
            double v = strtod(val, &end);
            if (end == val) continue;              // non-numeric, as Python skips
            auto e = real.update(strtod(ts, nullptr), key, v);
            if (e != IgnitionEvent::None) {
                seen.push_back({name_of(e), std::string(iso).substr(11, 8)});
            }
        }
        fclose(fh);
    }
    V offs, ons;
    for (auto& s : seen) (s.first == "off" ? offs : ons).push_back(s.second);
    check("the real drive contains exactly two ignition-offs",
          static_cast<int>(offs.size()), 2);
    check("the real drive contains exactly two restarts",
          static_cast<int>(ons.size()), 2);
    // The off-edge is noticed only when a voltage read arrives, so it is late
    // but bounded. The on-edge fires on the first PID reply, so it is prompt.
    auto lag_ok = [](const V& v, size_t i, const char* truth, double within) {
        if (i >= v.size()) return false;
        double lag = secs(v[i]) - secs(truth);
        return lag >= 0 && lag <= within;
    };
    check("...the first off follows the real 06:59:19 stop",
          lag_ok(offs, 0, "06:59:19", 90), true);
    check("...the second follows the real 07:27:24 stop",
          lag_ok(offs, 1, "07:27:24", 90), true);
    check("...the first restart is caught at once",
          lag_ok(ons, 0, "07:06:32", 1), true);
    check("...and so is the second",
          lag_ok(ons, 1, "07:32:43", 1), true);
    return gauge_test::check_report();
}
