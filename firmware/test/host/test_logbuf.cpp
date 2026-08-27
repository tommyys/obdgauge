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

int main() {
    test_layout();
    test_mount_empty();
    test_append_and_reopen();
    test_append_without_flush_is_lost_but_harmless();
    test_records_in_matches_linear_scan();
    return gauge_test::check_report();
}
