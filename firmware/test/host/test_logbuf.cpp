#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fake_flash.h"
#include "logbuf.h"
#include "poll.h"

using gauge_test::check;
using gauge_test::FakeFlash;
using gauge::kListCapacity;

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
    check("empty: no drive starts", (int)log.drive_starts(), 0);
    check("empty: no sectors used", (int)log.sectors_used(), 0);
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
    check("remount sees the drive", (int)again.drive_starts(), 1);
    check("remount counts the one sector in use", (int)again.sectors_used(), 1);
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
        uint16_t table_version;
    } h{};
    memcpy(h.magic, "MX5L", 4);
    h.seq = 1;
    h.drive = 1;
    h.flags = 1;   // opens a drive
    h.table_version = gauge::kChanTableVersion;
    static_assert(sizeof(h) == gauge::kSectorHeaderSize, "header layout mismatch");
    check("write header directly", f.write(0, &h, sizeof h), true);

    gauge::LogBuf log(f);
    check("mount sees the valid, empty sector", log.mount(), true);
    check("drive_starts sees the open drive", (int)log.drive_starts(), 1);
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
    // Bisection alone answered 301 here -- it never probed slots 100-149,
    // so it reported a count that spanned the hole (a genuine defect, fixed
    // by replacing the bisection with the exact chunked scan in
    // records_in()). The correct answer is the real first-erased index: 100.
    check("stops exactly at the hole, not past it", (int)after.record_count(), 100);
}

// A stride-8 grid-sampling fix (tried and rejected during review) would
// have missed a hole narrower than its stride if the hole happened to land
// entirely between two sampled slots. This is exactly that case: a 3-record
// hole, which a sample-every-8th-slot scan could step clean over while
// leaving valid records on both sides -- meaning a still-offered, otherwise
// intact drive would splice in a few garbage records (chan 0xFFFF among
// them, colliding with kChanDriveStart) rather than being dropped for being
// too short. The chunked scan reads every record, so a hole of any width is
// found exactly -- this proves that width does not matter to it.
static void test_records_in_across_a_narrow_erase_hole() {
    FakeFlash f(4);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(1756300000u);
    const int n = 300;
    for (int i = 0; i < n; ++i) check("append", log.append(rec((uint32_t)i, 12, (float)i)), true);
    check("flush", log.flush(), true);
    uint8_t* p = f.raw(1);
    const size_t rec_off = gauge::kSectorHeaderSize;
    // Indices 205-207: three records, well inside the real data, punched
    // back to 0xFF -- narrower than any plausible sampling stride.
    memset(p + rec_off + 205 * sizeof(gauge::Record), 0xFF, 3 * sizeof(gauge::Record));

    gauge::LogBuf after(f);
    check("remount survives a narrow hole", after.mount(), true);
    check("finds the narrow hole exactly", (int)after.record_count(), 205);
}

static void test_erase_all() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 200);
    check("erase_all", log.erase_all(), true);
    check("nothing left", (int)log.drive_starts(), 0);
    check("no sectors in use after erase_all", (int)log.sectors_used(), 0);
    check("still usable", log.begin_drive(0), true);
}

static void test_channel_ids() {
    // Ids come from poll.cpp's PID table, which is sorted by PID: 0x05 is
    // coolant, 0x0C is rpm, so coolant's id comes before rpm's.
    const uint16_t coolant = gauge::log_chan_id("coolant");
    const uint16_t rpm     = gauge::log_chan_id("rpm");
    check("coolant has an id", coolant != gauge::kChanUnknown, true);
    check("rpm has an id", rpm != gauge::kChanUnknown, true);
    check("table order is PID order", coolant < rpm, true);
    check("round trip rpm", std::string(gauge::log_chan_name(rpm)), std::string("rpm"));

    // volts is not a PID -- ATRV is an adapter command -- so it lives in the
    // extras block with the IMU rather than in the table.
    const uint16_t volts = gauge::log_chan_id("volts");
    check("volts has an id", volts != gauge::kChanUnknown, true);
    check("round trip volts", std::string(gauge::log_chan_name(volts)), std::string("volts"));

    for (const char* k : {"imu_ax", "imu_ay", "imu_az", "imu_gz"}) {
        const uint16_t id = gauge::log_chan_id(k);
        check("imu channel has an id", id != gauge::kChanUnknown, true);
        check("imu round trip", std::string(gauge::log_chan_name(id)), std::string(k));
    }

    check("nonsense has no id", gauge::log_chan_id("nope") == gauge::kChanUnknown, true);
    check("marker ids are not channels", gauge::log_chan_name(gauge::kChanDriveStart) == nullptr, true);
    // Nothing real may collide with the markers.
    check("ids stay clear of the markers",
          gauge::log_chan_id("imu_gz") < gauge::kChanDriveEnd, true);
}

// A drive long enough to own every sector of the ring. The old start-finding
// looked for the sector whose ring predecessor belongs to a DIFFERENT drive;
// a drive that fills the ring has no such gap anywhere, so read_drive found
// no start, returned false, and list() silently dropped a drive holding the
// entire partition -- "OK 0 drives" while sitting on 10 MB. Every existing
// wrap test keeps two drives alive, which is exactly why this survived.
static void test_drive_filling_the_whole_ring_is_readable() {
    FakeFlash f(4);
    gauge::LogBuf log(f);
    log.mount();
    log.begin_drive(1756300000u);        // writes the start marker
    // 4 sectors x 340 records, minus the marker already written.
    const int n = (int)(4 * gauge::kRecordsPerSector) - 1;
    for (int i = 0; i < n; ++i) log.append(rec((uint32_t)i * 10u, 12, (float)i));
    check("flush a ring-filling drive", log.flush(), true);
    check("every sector is in use", (int)log.sectors_used(), 4);

    Collect got;
    check("read_drive finds a drive with no gap", log.read_drive(1, collect, &got), true);
    check("it hands back every record", (int)got.all.size(),
          (int)(4 * gauge::kRecordsPerSector));
    // Guarded: read_drive returning false (which is exactly the bug this
    // test exists for) leaves this empty, and a crashing test reports worse
    // than a failing one.
    if (got.all.size() == 4 * gauge::kRecordsPerSector) {
        check("first record is the start marker", (int)got.all.front().chan,
              (int)gauge::kChanDriveStart);
        check("second record is reading 0", (double)got.all[1].value, 0.0);
        check("last record is the last reading", (double)got.all.back().value,
              (double)(n - 1));
    }
    // In order, not just present: t_ms rises monotonically across the wrap.
    bool ordered = true;
    for (size_t i = 2; i < got.all.size(); ++i)
        if (got.all[i].t_ms < got.all[i - 1].t_ms) ordered = false;
    check("records come back in order across the whole ring", ordered, true);

    gauge::DriveInfo out[4]{};
    check("list still offers it", (int)log.list(out, 4), 1);
    check("with all of its records", (int)out[0].records,
          (int)(4 * gauge::kRecordsPerSector));
}

// Design s9: "a channel table version mismatch is refused, not guessed."
// A record stores a channel *id*, which is a position in poll.cpp's compiled
// table -- so a firmware whose table gained a middle entry renames every
// channel of every drive already on flash.
static void test_channel_table_version_mismatch_is_refused() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 200);

    // The sector this drive lives in, stamped by a firmware whose table is
    // one version newer. Poked straight into the array: a real flash write
    // could not set the bit, and a real one would not need to.
    size_t stamped = 0;
    for (size_t i = 0; i < 8; ++i) {
        auto* h = reinterpret_cast<gauge::SectorHeader*>(f.raw(i));
        if (memcmp(h->magic, "MX5L", 4) == 0 && h->drive == 1) {
            h->table_version = gauge::kChanTableVersion + 1;
            ++stamped;
        }
    }
    check("the drive's sectors were re-stamped", (int)stamped > 0, true);

    gauge::LogBuf after(f);
    check("remount", after.mount(), true);
    gauge::DriveInfo out[4]{};
    check("list still describes it", (int)after.list(out, 4), 1);
    check("and reports the version it was written under", (int)out[0].table_version,
          (int)gauge::kChanTableVersion + 1);
    check("its records are still countable", (int)out[0].records, 202);

    // The refusal: a read that does not ask about the version is denied
    // rather than handed records this firmware's table would mislabel.
    Collect got;
    check("read_drive refuses a foreign table version",
          after.read_drive(1, collect, &got), false);
    check("and hands back nothing", (int)got.all.size(), 0);

    // A drive written by THIS table is unaffected.
    after.begin_drive(1756300500u);
    for (int i = 0; i < 200; ++i) after.append(rec((uint32_t)i, 12, (float)i));
    after.end_drive();
    Collect mine;
    check("a current-version drive still reads", after.read_drive(2, collect, &mine), true);
}

// "OK 32 drives" used to be indistinguishable from "32 drives and more I
// cannot show you". Both ways of running out have to say so.
static void test_list_says_when_it_is_truncated() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    for (int d = 0; d < 4; ++d) drive_of(log, 1756300000u + (uint32_t)d, 150);

    gauge::DriveInfo out[8]{};
    bool truncated = true;
    check("all four fit", (int)log.list(out, 8, &truncated), 4);
    check("nothing was hidden", truncated, false);

    truncated = false;
    check("asked for two, got two", (int)log.list(out, 2, &truncated), 2);
    check("and it says the answer is partial", truncated, true);
    check("the two it gave are the newest", (int)out[0].id, 4);

    // The other way out of room: more distinct drives than the accumulator
    // table holds. kListCapacity is the number, so make one more than that.
    FakeFlash big(kListCapacity + 8);
    gauge::LogBuf many(big);
    many.mount();
    for (size_t d = 0; d < kListCapacity + 1; ++d)
        drive_of(many, 1756300000u + (uint32_t)d, 150);
    static gauge::DriveInfo room[kListCapacity];
    truncated = false;
    check("the table fills up", (int)many.list(room, kListCapacity, &truncated),
          (int)kListCapacity);
    check("and that is reported too", truncated, true);
}

// GET used to resolve a drive by looking for it in list()'s reply, so every
// drive older than the newest kListCapacity was unreachable -- and since the
// pull tool skips drives already in logs/, "pull these and run again" could
// never reach them. Nothing outside list()'s window may be unresolvable.
static void test_a_drive_past_the_list_window_is_still_resolvable() {
    FakeFlash f(kListCapacity + 8);
    gauge::LogBuf log(f);
    log.mount();
    for (size_t d = 0; d < kListCapacity + 4; ++d)
        drive_of(log, 1756300000u + (uint32_t)d, 150);

    static gauge::DriveInfo room[kListCapacity];
    bool truncated = false;
    const size_t n = log.list(room, kListCapacity, &truncated);
    check("the window is full", (int)n, (int)kListCapacity);
    check("and says so", truncated, true);

    // Drive 1 is old enough that the newest-64 window cannot reach it.
    bool listed = false;
    for (size_t i = 0; i < n; ++i) if (room[i].id == 1) listed = true;
    check("drive 1 is outside the window", listed, false);

    gauge::DriveInfo info{};
    check("but it resolves by id", log.has_drive(1, &info), true);
    check("with the count GET needs", (int)info.records, 152);   // + 2 markers
    check("and the table it was written under", (int)info.table_version,
          (int)gauge::kChanTableVersion);
    check("a drive that was never written does not", log.has_drive(9999), false);
}

// The other half: LIST itself must be able to walk past its own window, or
// the tool has no way to learn the older ids in the first place.
static void test_list_pages_backwards() {
    FakeFlash f(16);
    gauge::LogBuf log(f);
    log.mount();
    for (int d = 0; d < 6; ++d) drive_of(log, 1756300000u + (uint32_t)d, 150);

    gauge::DriveInfo page[8]{};
    bool truncated = false;
    check("first page of two", (int)log.list(page, 2, &truncated), 2);
    check("newest first", (int)page[0].id, 6);
    check("and the second is drive 5", (int)page[1].id, 5);
    check("there is more", truncated, true);

    check("second page", (int)log.list(page, 2, &truncated, 5), 2);
    check("carries on where it left off", (int)page[0].id, 4);
    check("still more", truncated, true);

    check("third page", (int)log.list(page, 2, &truncated, 3), 2);
    check("the oldest two", (int)page[0].id, 2);
    check("down to drive 1", (int)page[1].id, 1);
    check("and now the ring is exhausted", truncated, false);

    check("past the end is empty, not an error", (int)log.list(page, 2, &truncated, 1), 0);
    check("with nothing hidden", truncated, false);
    check("an id no sector carries pages to nothing",
          (int)log.list(page, 2, &truncated, 77), 0);
}

// Only the writer can stamp a version, so this is the guard that a future
// table change actually has to bump the constant to be detectable.
static void test_written_sectors_carry_the_table_version() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 150);
    const auto* h = reinterpret_cast<const gauge::SectorHeader*>(f.raw(1));
    check("the sector is the drive's", (int)h->drive, 1);
    check("and carries the compiled table version", (int)h->table_version,
          (int)gauge::kChanTableVersion);
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
    test_records_in_across_a_narrow_erase_hole();
    test_erase_all();
    test_drive_filling_the_whole_ring_is_readable();
    test_channel_table_version_mismatch_is_refused();
    test_list_says_when_it_is_truncated();
    test_a_drive_past_the_list_window_is_still_resolvable();
    test_list_pages_backwards();
    test_written_sectors_carry_the_table_version();
    test_channel_ids();
    return gauge_test::check_report();
}
