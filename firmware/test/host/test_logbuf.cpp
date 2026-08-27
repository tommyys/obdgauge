#include <cstring>
#include <vector>

#include "check.h"
#include "fake_flash.h"
#include "logbuf.h"

using gauge_test::check;
using gauge_test::FakeFlash;

static void test_layout() {
    check("sector size", (int)gauge::kSectorSize, 4096);
    check("header size", (int)gauge::kSectorHeaderSize, 16);
    check("record size", (int)sizeof(gauge::Record), 12);
    check("records per sector", (int)gauge::kRecordsPerSector, 340);
}

static void test_mount_empty() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    check("mount on erased flash", log.mount(), true);
    check("empty: no drives", (int)log.drive_count(), 0);
    check("empty: no records", (int)log.record_count(), 0);
    check("mount does not erase", (int)f.erases(), 0);
}

static gauge::Record rec(uint32_t t_ms, uint16_t chan, float v) {
    return gauge::Record{t_ms, chan, 0, v};
}

static void test_append_and_reopen() {
    FakeFlash f(8);
    {
        gauge::LogBuf log(f);
        check("mount", log.mount(), true);
        check("begin drive", log.begin_drive(1756300000u), true);
        check("first drive id is 1", (int)log.current_drive(), 1);
        for (int i = 0; i < 10; ++i)
            check("append", log.append(rec(i * 100u, 12, 800.0f + i)), true);
        check("flush", log.flush(), true);
        // one drive-start marker plus ten readings
        check("records after flush", (int)log.record_count(), 11);
    }
    // A fresh object over the same flash is what a reboot looks like.
    gauge::LogBuf again(f);
    check("remount", again.mount(), true);
    check("remount sees the drive", (int)again.drive_count(), 1);
    check("remount sees the records", (int)again.record_count(), 11);
    check("remount continues the drive numbering", (int)again.next_drive_id(), 2);
}

static void test_append_without_flush_is_lost_but_harmless() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(0);
    log.append(rec(0, 12, 900.0f));
    log.flush();
    log.append(rec(100, 12, 950.0f));      // buffered, never committed
    gauge::LogBuf after(f);
    after.mount();
    // The drive-start marker and the flushed reading survive; the buffered
    // one does not. Losing at most one batch is the deal flush() makes.
    check("survivors after a cut", (int)after.record_count(), 2);
}

// Independent linear-scan reference for records_in()'s bisection. Scans the
// raw flash bytes directly rather than going through LogBuf at all, so a
// match against LogBuf::record_count() (computed via bisection inside
// mount()) is a real cross-check, not the same code checking itself.
static size_t linear_count(gauge_test::FakeFlash& f, size_t sector) {
    size_t n = 0;
    for (size_t r = 0; r < gauge::kRecordsPerSector; ++r) {
        const uint8_t* p = f.raw(sector) + gauge::kSectorHeaderSize + r * sizeof(gauge::Record);
        bool is_erased = true;
        for (size_t i = 0; i < sizeof(gauge::Record); ++i) {
            if (p[i] != 0xFF) { is_erased = false; break; }
        }
        if (is_erased) break;
        ++n;
    }
    return n;
}

static void test_records_in_matches_linear_scan() {
    // target = 0 here is an *unwritten* sector: its header never validates,
    // so mount() skips it via valid(h) before records_in() is ever called.
    // It proves an unopened sector contributes 0 to record_count() -- it
    // does not exercise the bisection. The bisection's own zero case (a
    // valid header, no committed records) is covered separately by
    // test_records_in_zero_with_valid_header(), below.
    const size_t targets[] = {0, 1, 2, 170, 339, 340};
    for (size_t target : targets) {
        FakeFlash f(8);
        gauge::LogBuf log(f);
        check("mount", log.mount(), true);
        size_t drive_sector = 0;
        if (target > 0) {
            check("begin drive", log.begin_drive(1000u), true);
            drive_sector = 1;  // fresh flash: the first drive lands in sector 1
            for (size_t i = 1; i < target; ++i)
                check("append", log.append(rec((uint32_t)i, 12, 1.0f * (float)i)), true);
            check("flush", log.flush(), true);
        }
        gauge::LogBuf again(f);
        check("remount", again.mount(), true);
        const size_t expect = linear_count(f, drive_sector);
        check("bisection matches linear scan", (int)again.record_count(), (int)expect);
        if (target > 0) check("linear scan sees the target fill level", (int)expect, (int)target);
    }
}

// A power cut between open_sector() writing the 16-byte header and flush()
// writing the first record leaves exactly this on the real board: a valid,
// still-current header with zero committed records after it. That is the
// bisection's genuine zero case -- unlike an unopened sector, valid(h)
// passes and mount() does call records_in() here. The header is written
// directly (not via begin_drive(), which always commits its start marker
// in the same call) because this is a partial-write state, not one any
// public LogBuf sequence produces on its own.
static void test_records_in_zero_with_valid_header() {
    FakeFlash f(8);
    struct {
        char     magic[4];
        uint32_t seq;
        uint32_t drive;
        uint16_t flags;
        uint16_t pad;
    } h{};
    memcpy(h.magic, "MX5L", 4);
    h.seq = 1;
    h.drive = 1;
    h.flags = 1;   // opens a drive
    h.pad = 0;
    static_assert(sizeof(h) == gauge::kSectorHeaderSize, "header layout mismatch");
    check("write header directly", f.write(0, &h, sizeof h), true);

    gauge::LogBuf log(f);
    check("mount sees the valid, empty sector", log.mount(), true);
    check("drive_count sees the open drive", (int)log.drive_count(), 1);
    check("record_count is zero", (int)log.record_count(), 0);

    const size_t expect = linear_count(f, 0);
    check("bisection matches linear scan on header-only sector",
          (int)log.record_count(), (int)expect);
    check("linear scan also sees zero", (int)expect, 0);
}

struct Collect {
    std::vector<gauge::Record> all;
};
static bool collect(const gauge::Record* r, size_t n, void* ctx) {
    auto* c = static_cast<Collect*>(ctx);
    c->all.insert(c->all.end(), r, r + n);
    return true;
}

static void test_sector_roll() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(0);
    // One marker is already in, so this crosses the boundary by 9 records.
    const int n = (int)gauge::kRecordsPerSector + 8;
    for (int i = 0; i < n; ++i) check("append", log.append(rec(i * 10u, 12, (float)i)), true);
    check("flush", log.flush(), true);

    Collect c;
    check("read back", log.read_drive(log.current_drive(), collect, &c), true);
    check("stream is continuous", (int)c.all.size(), n + 1);
    check("marker first", (int)c.all[0].chan, (int)gauge::kChanDriveStart);
    check("last record survived the roll", (int)c.all.back().value, n - 1);
    // Two sectors written means two erased: one per open.
    check("erases so far", (int)f.erases(), 2);
}

// read_drive() must keep no state between calls: start, buf, and the loop
// index are all locals, so two calls for the same drive must walk the
// identical path and yield the identical stream. Task 4's summarise() calls
// read_drive() repeatedly for the same drive and would silently go quiet if
// this ever regressed. The drive spans two sectors so both calls' walks
// actually iterate more than once.
static void test_read_drive_is_stateless() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(0);
    const int n = (int)gauge::kRecordsPerSector + 8;
    for (int i = 0; i < n; ++i) check("append", log.append(rec(i * 10u, 12, (float)i)), true);
    check("flush", log.flush(), true);

    Collect first, second;
    check("first read", log.read_drive(log.current_drive(), collect, &first), true);
    check("second read", log.read_drive(log.current_drive(), collect, &second), true);

    check("same length", (int)first.all.size(), (int)second.all.size());
    const size_t mid = first.all.size() / 2;
    const gauge::Record* pairs[][2] = {
        {&first.all.front(), &second.all.front()},
        {&first.all[mid],    &second.all[mid]},
        {&first.all.back(),  &second.all.back()},
    };
    for (auto& p : pairs) {
        check("t_ms matches", (int)p[0]->t_ms, (int)p[1]->t_ms);
        check("chan matches", (int)p[0]->chan, (int)p[1]->chan);
        check("value matches", (double)p[0]->value, (double)p[1]->value);
    }
}

// Fills a drive with `n` readings and closes it.
static void drive_of(gauge::LogBuf& log, uint32_t epoch, int n) {
    log.begin_drive(epoch);
    for (int i = 0; i < n; ++i) log.append(rec((uint32_t)i * 10u, 12, (float)i));
    log.end_drive();
}

static void test_wrap_drops_the_oldest() {
    FakeFlash f(4);                      // a deliberately tiny ring
    gauge::LogBuf log(f);
    log.mount();
    // Each drive takes one sector, and begin_drive always advances -- so four
    // drives fill the four sectors exactly and the FIFTH is the one that
    // pushes the first out. (Getting this off by one hid the wrap entirely:
    // four drives in four sectors lose nothing and the test still "passed".)
    for (int d = 0; d < 5; ++d) drive_of(log, 1756300000u + (uint32_t)d, 100);

    gauge::DriveInfo got[8]{};
    const size_t n = log.list(got, 8);
    check("holds four drives, not five", (int)n, 4);
    check("newest first", (int)got[0].id, 5);
    check("oldest surviving is drive 2", (int)got[3].id, 2);
    check("drive 1 is gone", log.has_drive(1), false);
    check("epoch came back", (int)got[3].epoch_s, (int)1756300001u);
}

static void test_short_drives_are_not_offered() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 5);        // a key touched and released
    drive_of(log, 1756300100u, 200);      // an actual drive
    gauge::DriveInfo got[8]{};
    const size_t n = log.list(got, 8);
    check("the 5-record drive is hidden", (int)n, 1);
    check("the real drive is listed", (int)got[0].records, 202);  // + 2 markers
}

static void test_duration_and_completeness() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(1756300000u);
    for (int i = 0; i < 150; ++i) log.append(rec((uint32_t)i * 100u, 12, (float)i));
    log.end_drive();
    gauge::DriveInfo got[4]{};
    log.list(got, 4);
    check("duration is the last t_ms", (int)got[0].duration_ms, 14900);
    check("closed drive is complete", got[0].complete, true);
}

static void test_half_erased_sector_is_not_data() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    // Three drives, so sectors 1, 2 and 3 all hold real records -- half-
    // erasing a sector that was already blank proves nothing.
    for (int d = 0; d < 3; ++d) drive_of(log, 1756300000u + (uint32_t)d, 200);
    // Power lost part-way through erasing sector 3: the first 512 bytes went
    // to 0xFF, including the magic. The rest still looks like records.
    f.half_erase(3, 512);
    gauge::LogBuf after(f);
    check("remount survives it", after.mount(), true);
    gauge::DriveInfo got[4]{};
    const size_t n = after.list(got, 4);
    check("the two intact drives are listed", (int)n, 2);
    check("the drive in the wrecked sector is gone", after.has_drive(3), false);
    // The half-erased sector has no magic, so it is a free sector, and the
    // next append must be willing to take it.
    check("still writable", after.begin_drive(0), true);
}

static void test_seq_wraparound() {
    FakeFlash f(4);
    gauge::LogBuf log(f);
    log.mount();
    // Forge two sectors whose seq straddles the u32 rollover: 0xFFFFFFFE is
    // OLDER than 2, and a plain `a > b` gets that backwards.
    log.force_seq_for_test(0xFFFFFFFEu);
    drive_of(log, 1756300000u, 150);      // seq FFFFFFFF
    drive_of(log, 1756300100u, 150);      // seq 0, 1 ...
    gauge::DriveInfo got[4]{};
    const size_t n = log.list(got, 4);
    check("both drives listed", (int)n, 2);
    check("the later drive is first", (int)got[0].id, 2);
}

static void test_drive_spanning_the_wrap() {
    FakeFlash f(4);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 100);      // uses a sector, will be overwritten
    // A drive long enough to run off the end of the ring and back round.
    log.begin_drive(1756300100u);
    const int n = (int)gauge::kRecordsPerSector * 2 + 50;
    for (int i = 0; i < n; ++i) log.append(rec((uint32_t)i, 12, (float)i));
    log.end_drive();

    Collect c;
    check("read the wrapped drive", log.read_drive(2, collect, &c), true);
    // Markers plus every reading, in order, across the wrap.
    check("nothing lost across the wrap", (int)c.all.size(), n + 2);
    for (size_t i = 1; i + 1 < c.all.size(); ++i) {
        if (c.all[i].value != (float)(i - 1)) {
            check("records are in order", false, true);
            break;
        }
    }
}

static void test_records_survive_a_cut_mid_drive() {
    FakeFlash f(8);
    {
        gauge::LogBuf log(f);
        log.mount();
        log.begin_drive(1756300000u);
        for (int i = 0; i < 200; ++i) log.append(rec((uint32_t)i * 50u, 12, (float)i));
        log.flush();
        // No end_drive(): the power went out.
    }
    gauge::LogBuf after(f);
    after.mount();
    gauge::DriveInfo got[4]{};
    check("the unfinished drive is still offered", (int)after.list(got, 4), 1);
    check("marked incomplete", got[0].complete, false);
    check("its records are all there", (int)got[0].records, 201);
}

// Task 2's review deferred this here: records_in() bisects on the invariant
// "records are contiguous from the start of the sector, everything after is
// erased". A power cut during an erase can break that invariant while
// leaving the sector's magic intact -- not just as a truncated prefix (the
// half-erase case above), but as a hole: bytes in the MIDDLE of the sector
// return to 0xFF while real records both before and after the hole survive.
// This builds that fixture directly (raw(), not half_erase(), because
// half_erase() only clears from the start of the sector) and records what
// the bisection actually returns, so the answer is established rather than
// assumed.
static void test_records_in_across_a_mid_sector_erase_hole() {
    FakeFlash f(4);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(1756300000u);
    const int n = 300;
    for (int i = 0; i < n; ++i) check("append", log.append(rec((uint32_t)i, 12, (float)i)), true);
    check("flush", log.flush(), true);
    // Sector 1 now holds indices 0..300 (the marker plus 300 readings) as
    // real data, 301..339 still erased from mount.
    uint8_t* p = f.raw(1);
    const size_t rec_off = gauge::kSectorHeaderSize;
    // Punch a hole: indices 100..149 go back to 0xFF, as if an interrupted
    // erase touched the middle of the sector rather than a clean prefix.
    // Indices 150..300 are untouched and still look like real records.
    memset(p + rec_off + 100 * sizeof(gauge::Record), 0xFF, 50 * sizeof(gauge::Record));

    gauge::LogBuf after(f);
    check("remount survives a mid-sector hole", after.mount(), true);
    // Bisection alone answers 301 here -- it never probes slots 100-149, so
    // it reports a count that spans the hole (a genuine defect, fixed in
    // records_in() by the grid-scan-plus-fallback below the bisection). The
    // safe answer is the real first-erased index: 100.
    check("stops exactly at the hole, not past it", (int)after.record_count(), 100);
}

int main() {
    test_layout();
    test_mount_empty();
    test_append_and_reopen();
    test_append_without_flush_is_lost_but_harmless();
    test_records_in_matches_linear_scan();
    test_records_in_zero_with_valid_header();
    test_sector_roll();
    test_read_drive_is_stateless();
    test_wrap_drops_the_oldest();
    test_short_drives_are_not_offered();
    test_duration_and_completeness();
    test_half_erased_sector_is_not_data();
    test_seq_wraparound();
    test_drive_spanning_the_wrap();
    test_records_survive_a_cut_mid_drive();
    test_records_in_across_a_mid_sector_erase_hole();
    return gauge_test::check_report();
}
