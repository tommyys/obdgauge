// Ported from tests/test_vehicle.py — VIN decode, dial profiles, mode-09.
#include "check.h"
#include "vehicle.h"
#include "parse.h"
#include "poll.h"
using gauge_test::check;
using S = std::string;
using I = std::optional<int>;

static S bytes_to_string(const std::optional<gauge::Bytes>& b) {
    if (!b) return "<none>";
    return S(b->begin(), b->end());
}

int main() {
    // --- mode 09 / VIN reassembly ----------------------------------------
    const S MULTI = "49 02 01 00 00 00 4A\r"
                    "49 02 02 4D 30 4E 44\r"
                    "49 02 03 41 31 52 30\r"
                    "49 02 04 31 32 33 34\r"
                    "49 02 05 35 36 37\r";
    check("multiframe VIN reassembles",
          gauge::clean_vin(bytes_to_string(gauge::parse_mode09(MULTI, 0x02))),
          S("JM0NDA1R01234567"));
    const S SINGLE = "014 \r49 02 01 4A 4D 30 4E 44 41 31 52 30 31 32 33 34 35 36 37 38";
    check("single-blob VIN reassembles",
          gauge::clean_vin(bytes_to_string(gauge::parse_mode09(SINGLE, 0x02))),
          S("JM0NDA1R012345678"));
    check("mode09 returns None when header absent",
          gauge::parse_mode09("41 0C 1A F8", 0x02).has_value(), false);

    // --- VIN field decode -------------------------------------------------
    check("clean_vin strips junk", gauge::clean_vin(" jm0nd-a1r0 1234567 "),
          S("JM0NDA1R01234567"));
    check("valid_vin needs 17 chars", gauge::valid_vin("JM0NDA1R012345678"), true);
    check("valid_vin rejects short", gauge::valid_vin("JM0ND"), false);

    check("make from WMI JM0 -> Mazda", gauge::make_of("JM0NDA1R012345678"), S("Mazda"));
    check("make from WMI JHM -> Honda", gauge::make_of("JHMFC1E30JH000001"), S("Honda"));
    check("make from WMI JTJ -> Lexus", gauge::make_of("JTJDARDZ102000001"), S("Lexus"));
    check("make unknown WMI -> empty", gauge::make_of("ZZZNDA1R012345678"), S(""));

    // position 10 is the model year; the code cycles every 30 years
    check("year code R -> 2024", gauge::model_year("JM0NDA1R0R2345678"), I{2024});
    check("year code L -> 2020", gauge::model_year("JM0NDA1R0L2345678"), I{2020});
    check("year code T -> 2026", gauge::model_year("JM0NDA1R0T2345678"), I{2026});
    check("year code W -> 1998 not 2028", gauge::model_year("JM0NDA1R0W2345678"), I{1998});
    check("year code Y -> 2000 not 2030", gauge::model_year("JM0NDA1R0Y2345678"), I{2000});
    check("year digit 5 -> 2005 not 2035", gauge::model_year("JM0NDA1R052345678"), I{2005});
    check("year invalid code -> None", gauge::model_year("JM0NDA1R0I2345678"), I{});

    // --- identify ---------------------------------------------------------
    auto mx5 = gauge::identify("JM0NDA1R0R2345678");
    check("identify make", mx5.make, S("Mazda"));
    check("identify year", mx5.year, I{2024});
    check("identify label", mx5.label, S("MAZDA"));
    check("identify known", mx5.known, true);
    check("identify uses make profile", mx5.rpm_red, 6200.0);

    auto named = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    check("explicit model in label", named.label, S("MAZDA MX-5"));
    check("model picks specific profile", named.rpm_red, 7000.0);
    check("model picks specific redline", named.rpm_max, 8000.0);

    auto unknown = gauge::identify("ZZZNDA1R0R2345678");
    check("unknown WMI shows the code", unknown.label, S("WMI ZZZ"));
    check("unknown WMI not known", unknown.known, false);
    check("unknown WMI gets default profile",
          unknown.rpm_max, gauge::kDefaultProfile.rpm_max);

    auto none = gauge::identify();
    check("no VIN at all -> OBD-II", none.label, S("OBD-II"));
    check("no VIN -> vin empty", none.vin, S(""));

    auto override_ = gauge::identify("JM0NDA1R0R2345678", "Honda", "", I{2019});
    check("explicit make wins", override_.make, S("Honda"));
    check("explicit year wins", override_.year, I{2019});

    // --- supported PIDs -> channel keys -----------------------------------
    auto keys = gauge::keys_for({0x0C, 0x0D, 0x05, 0x5C, 0x63});
    check("keys_for maps rpm", keys.count("rpm") > 0, true);
    check("keys_for maps oil", keys.count("oil") > 0, true);
    check("keys_for skips undecodable pid", gauge::keys_for({0x99}).empty(), true);
    return gauge_test::check_report();
}
