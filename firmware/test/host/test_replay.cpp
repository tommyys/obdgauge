// The replay engine, against a synthetic library and then the real one.
#include "check.h"
#include "replay.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using gauge_test::check;
using S = std::string;

static void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
static void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8));
}

// One drive, two channels, three samples at 0 / 1000 / 2000 ms.
static std::vector<uint8_t> synth() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'M','X','5','D'});
    put16(b, 1); put16(b, 2); put16(b, 1); put16(b, 0);
    put32(b, 3);
    while (b.size() < 32) b.push_back(0);
    auto name16 = [&](const char* s) {
        size_t at = b.size();
        b.resize(at + 16, 0);
        memcpy(&b[at], s, strlen(s));
    };
    name16("rpm"); name16("coolant");
    size_t at = b.size(); b.resize(at + 20, 0); memcpy(&b[at], "synthetic", 9);
    put32(b, 0); put32(b, 3); put32(b, 2000);
    auto rec = [&](uint32_t t, uint16_t c, float v) {
        put32(b, t); put16(b, c); put16(b, 0);
        uint32_t bits; memcpy(&bits, &v, 4); put32(b, bits);
    };
    rec(0, 0, 900.0f); rec(1000, 1, 72.0f); rec(2000, 0, 3000.0f);
    return b;
}

int main() {
    auto blob = synth();
    gauge::Replay r;
    check("opens a valid library", r.open(blob.data(), blob.size()), true);
    check("drive count", r.drive_count(), 1);
    check("channel count", r.channel_count(), 2);
    check("total records", r.total_records(), 3);
    check("drive name", r.drive_name(0), S("synthetic"));
    check("channel names", r.channel_name(0) + "," + r.channel_name(1), S("rpm,coolant"));
    check("duration", r.duration_s(), 2.0);

    gauge::Replay bad;
    check("rejects a non-library buffer",
          bad.open(reinterpret_cast<const uint8_t*>("NOPE1234"), 8), false);
    check("rejects a truncated library", bad.open(blob.data(), 40), false);

    // Nothing is due before its timestamp; everything is due by the end.
    gauge::ReplaySample s{};
    check("first sample is due at t=0", r.next(0.0, &s), true);
    check("...and it is the rpm reading", (int)s.chan == 0 && s.value == 900.0f, true);
    check("second is not due yet at t=0.5", r.next(0.5, &s), false);
    check("second is due at t=1.0", r.next(1.0, &s), true);
    check("...and it is the coolant reading", (int)s.chan == 1 && s.value == 72.0f, true);
    check("not finished yet", r.finished(), false);
    check("third is due at t=9 (late is still due)", r.next(9.0, &s), true);
    check("finished after the last sample", r.finished(), true);
    check("nothing more is yielded", r.next(99.0, &s), false);

    r.rewind();
    check("rewind replays from the start", r.next(0.0, &s) && s.value == 900.0f, true);

    // The real library, if it has been built.
    FILE* fh = fopen("../../../build-assets/drives.bin", "rb");
    if (!fh) {
        printf("%-46s %s\n", "real library (build it to cover this)", "skip");
    } else {
        fseek(fh, 0, SEEK_END);
        long n = ftell(fh);
        fseek(fh, 0, SEEK_SET);
        std::vector<uint8_t> real(static_cast<size_t>(n));
        size_t got = fread(real.data(), 1, real.size(), fh);
        fclose(fh);
        gauge::Replay rr;
        check("real library opens", rr.open(real.data(), got), true);
        check("real library has three drives", rr.drive_count(), 3);
        check("real library has 35 channels", rr.channel_count(), 35);
        check("real library has 42412 records", rr.total_records(), 42412);
        // Walk the whole of drive 0 and confirm timestamps never go backwards.
        rr.select(0);
        gauge::ReplaySample p{};
        uint32_t last = 0;
        long walked = 0;
        bool ordered = true;
        while (rr.next(1e9, &p)) {
            if (p.t_ms < last) ordered = false;
            last = p.t_ms;
            ++walked;
        }
        check("drive 0 walks 16896 samples", static_cast<int>(walked), 16896);
        check("timestamps are monotonic", ordered, true);
    }
    return gauge_test::check_report();
}
