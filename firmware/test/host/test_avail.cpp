// Which views a car can actually drive. Ported from the simulator's
// applyChannels() (mx5gauge/web/index.html): a view lists the channels it
// needs, and it is unavailable only when the car reports NONE of them.
//
// The rule is OR, not AND, and that is deliberate: ENGINE needs
// "coolant,volts,ctrl_volt,intake" and is worth showing on a car that reports
// only one of the four. Requiring all of them would blank views that work.
#include "check.h"
#include "avail.h"
#include <set>
#include <string>

using namespace gauge;
using gauge_test::check;

namespace {

std::set<std::string> chans(std::initializer_list<const char*> keys) {
    std::set<std::string> s;
    for (const char* k : keys) s.insert(k);
    return s;
}

}  // namespace

int main() {
    // --- the unknown-car case, which is the one that bites --------------------
    // Before the car is identified there is no channel list. Every view must
    // stay available: the alternative is "n/a" flashing across all eight views
    // during connect, which reads as a broken gauge rather than a connecting
    // one. nullptr means unknown; it does NOT mean "reports nothing".
    check("unknown car, tacho", view_available("rpm", nullptr), true);
    check("unknown car, thermals",
          view_available("coolant,intake,cat_b1s1,catalyst", nullptr), true);

    // A car identified as reporting nothing is a different thing from a car
    // not yet identified, and gets the honest answer.
    const std::set<std::string> none = chans({});
    check("identified, reports nothing", view_available("rpm", &none), false);

    // --- the ordinary case ---------------------------------------------------
    const std::set<std::string> mx5 =
        chans({"rpm", "speed", "throttle", "coolant", "intake", "ctrl_volt",
               "act_torque", "ref_torque", "cat_b1s1"});

    check("tacho on the MX-5",    view_available("rpm", &mx5), true);
    check("trip on the MX-5",     view_available("speed", &mx5), true);
    check("power on the MX-5",    view_available("act_torque,ref_torque", &mx5), true);
    // The MX-5 does not report fuel rate, so ECONOMY is the one view that is
    // genuinely unavailable on this car -- SPEC.md section 2.
    check("economy on the MX-5",  view_available("fuel_rate", &mx5), false);

    // --- OR, not AND ---------------------------------------------------------
    // One channel out of four is enough. This is the assertion that fails if
    // anyone "tidies" the rule into requiring every listed channel.
    const std::set<std::string> only_volts = chans({"volts"});
    check("engine on volts alone",
          view_available("coolant,volts,ctrl_volt,intake", &only_volts), true);
    const std::set<std::string> only_maf = chans({"maf"});
    check("engine on an unrelated channel",
          view_available("coolant,volts,ctrl_volt,intake", &only_maf), false);

    // --- degenerate needs ----------------------------------------------------
    // DRIVES lists no channels because it is about files, not readings, so it
    // is available on any car -- including one that reports nothing.
    check("no needs, empty car", view_available("", &none), true);
    check("null needs, empty car", view_available(nullptr, &none), true);

    // Whitespace and trailing separators must not invent a channel named "".
    // An empty key would never match, so a stray comma would silently make an
    // otherwise-fine view look unavailable.
    check("trailing comma", view_available("rpm,", &mx5), true);
    check("spaces around keys", view_available(" rpm , speed ", &mx5), true);
    check("only separators", view_available(" , ", &none), true);

    return gauge_test::check_report();
}
