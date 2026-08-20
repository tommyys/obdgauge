// Assertions ported from tests/test_pids.py (response parsing + ATRV).
#include "check.h"
#include "parse.h"
using gauge_test::check;
using gauge::Bytes;
using B = std::optional<Bytes>;
using D = std::optional<double>;

int main() {
    check("parse 41 0C 1A F8", gauge::parse_mode01("41 0C 1A F8", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse no spaces",   gauge::parse_mode01("410C1AF8", 0x0C),    B{Bytes{0x1A, 0xF8}});
    check("parse with prompt", gauge::parse_mode01("41 0C 1A F8 \r>", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse with header noise",
          gauge::parse_mode01("7E8 03 41 0C 1A F8", 0x0C), B{Bytes{0x1A, 0xF8}});
    check("parse wrong pid -> None", gauge::parse_mode01("41 0D 20", 0x0C), B{});
    check("parse NO DATA -> None",   gauge::parse_mode01("NO DATA", 0x0C), B{});

    check("ATRV 13.8V",        gauge::parse_voltage("13.8V"),    D{13.8});
    check("ATRV 14.4V\\r>",    gauge::parse_voltage("14.4V\r>"), D{14.4});
    check("ATRV junk -> None", gauge::parse_voltage("ELM327"),   D{});
    return gauge_test::check_report();
}
