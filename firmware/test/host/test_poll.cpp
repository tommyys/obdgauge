// Assertions ported from tests/test_pids.py (bitmask, poll cycle, decode).
#include "check.h"
#include "poll.h"
#include <algorithm>
using gauge_test::check;
using gauge::Bytes;

static bool has(const std::set<uint8_t>& s, uint8_t v) { return s.count(v) > 0; }
static bool has(const std::vector<uint8_t>& v, uint8_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

int main() {
    // 0xBE1FA813 is the classic example mask for PIDs 01-20
    auto sup = gauge::parse_supported(Bytes{0xBE, 0x1F, 0xA8, 0x13}, 0x00);
    check("bitmask contains 0x0C (rpm)",   has(sup, 0x0C), true);
    check("bitmask contains 0x0D (speed)", has(sup, 0x0D), true);
    check("bitmask excludes 0x02",         has(sup, 0x02), false);
    check("MSB of A means base+1",
          has(gauge::parse_supported(Bytes{0x80, 0, 0, 0}, 0x00), 0x01), true);
    check("LSB of D means base+32",
          has(gauge::parse_supported(Bytes{0, 0, 0, 0x01}, 0x00), 0x20), true);
    check("base 0x20 offsets",
          has(gauge::parse_supported(Bytes{0x80, 0, 0, 0}, 0x20), 0x21), true);

    auto cyc = gauge::build_poll_cycle({0x0C, 0x0D, 0x05});
    check("poll cycle includes rpm",          has(cyc, 0x0C), true);
    check("poll cycle drops unsupported oil", has(cyc, 0x5C), false);
    check("poll cycle empty when nothing supported",
          gauge::build_poll_cycle({}).empty(), true);

    auto wide = gauge::build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, true);
    check("log_all includes fuel level 0x2F", has(wide, 0x2F), true);
    check("log_all includes ambient 0x46",    has(wide, 0x46), true);
    check("rpm interleaved between slow pids",
          std::count(wide.begin(), wide.end(), uint8_t{0x0C}) > 1, true);

    auto narrow = gauge::build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, false);
    check("display-only skips fuel level", has(narrow, 0x2F), false);
    check("display-only keeps coolant",    has(narrow, 0x05), true);

    // end-to-end decode, via the PID table
    auto rpm = gauge::decode(0x0C, Bytes{0x1A, 0xF8});
    check("decode rpm key",   rpm ? rpm->key : std::string("none"), std::string("rpm"));
    check("decode rpm value", rpm ? rpm->value : -1.0, 1726.0);
    auto cool = gauge::decode(0x05, Bytes{0x5A});
    check("decode coolant key",   cool ? cool->key : std::string("none"), std::string("coolant"));
    check("decode coolant value", cool ? cool->value : -1.0, 50.0);
    check("decode unknown pid -> none", gauge::decode(0xFE, Bytes{0x00}).has_value(), false);
    check("decode short payload -> none", gauge::decode(0x0C, Bytes{0x1A}).has_value(), false);
    return gauge_test::check_report();
}
