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

int main() {
    test_layout();
    test_mount_empty();
    return gauge_test::check_report();
}
