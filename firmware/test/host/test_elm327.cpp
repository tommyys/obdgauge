// The ELM327 conversation, exercised against a fake transport. The same
// Elm327 code runs on the board over NimBLE — only the transport differs.
#include "check.h"
#include "elm327.h"
#include "fake_transport.h"
using gauge_test::check;
using B = std::optional<gauge::Bytes>;
using D = std::optional<double>;
using S = std::string;

int main() {
    FakeTransport t;
    t.replies = {"ELM327 v1.5\r>", "OK\r>", "OK\r>", "OK\r>", "OK\r>", "OK\r>"};
    gauge::Elm327 elm(t);
    check("init succeeds on a healthy adapter", elm.init(), true);
    check("init sends the full bring-up sequence",
          static_cast<int>(t.written.size()), 6);
    check("init starts with a reset", t.written[0], S("ATZ"));
    check("init turns echo off", t.written[1], S("ATE0"));
    check("init lets the adapter pick the protocol", t.written[5], S("ATSP0"));
    check("init pauses between commands", t.delays, 6);

    t.written.clear();
    t.replies = {"41 0C 1A F8\r>"};
    check("request returns the decoded payload", elm.request(0x0C), B{gauge::Bytes{0x1A, 0xF8}});
    check("request asks for the right pid", t.written[0], S("010C"));

    t.replies = {"NO DATA\r>"};
    check("NO DATA yields nothing", elm.request(0x0C), B{});

    t.replies = {};
    check("a silent adapter yields nothing", elm.request(0x0C), B{});

    // An adapter still echoing must not have its echo mistaken for a reply.
    t.replies = {"010C\r41 0C 1A F8\r>"};
    check("an echoed command is stripped", elm.request(0x0C), B{gauge::Bytes{0x1A, 0xF8}});

    t.replies = {"13.8V\r>"};
    check("ATRV is read as voltage", elm.read_voltage(), D{13.8});

    // discover() walks the four bitmask blocks, stopping when a block does
    // not advertise the next one.
    FakeTransport d;
    gauge::Elm327 elm2(d);
    // block 0 advertises 0x20 (bit 32 set), block 0x20 does not advertise 0x40
    d.replies = {"41 00 BE 1F A8 13\r>", "41 20 80 00 00 00\r>"};
    auto sup = elm2.discover();
    check("discover finds rpm",   sup.count(0x0C) > 0, true);
    check("discover finds speed", sup.count(0x0D) > 0, true);
    check("discover crosses into the second block", sup.count(0x21) > 0, true);
    check("discover stops when a block does not advertise the next",
          static_cast<int>(d.written.size()), 2);

    // VIN over mode 09
    FakeTransport v;
    gauge::Elm327 elm3(v);
    v.replies = {"49 02 01 00 00 00 4A\r49 02 02 4D 30 4E 44\r"
                 "49 02 03 41 31 52 30\r49 02 04 31 32 33 34\r"
                 "49 02 05 35 36 37\r>"};
    check("VIN read and cleaned", elm3.read_vin(), S("JM0NDA1R01234567"));
    check("VIN asked for with mode 09 pid 02", v.written[0], S("0902"));

    FakeTransport n;
    gauge::Elm327 elm4(n);
    n.replies = {"NO DATA\r>"};
    check("a car that will not give a VIN yields empty", elm4.read_vin(), S(""));
    return gauge_test::check_report();
}
