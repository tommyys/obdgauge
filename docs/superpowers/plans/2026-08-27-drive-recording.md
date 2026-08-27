# Drive Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The gauge records every reading of every drive to flash and gives them back over USB as `logs/*.csv`, so a drive nobody watched can be watched afterwards.

**Architecture:** A ring of 4 KB flash sectors in the 9.94 MB nothing uses, written by a dedicated low-priority task so no flash operation ever lands on the display's path. The ring's index and framing logic is pure C++ in `gauge_core` behind an `IFlash` seam, so every ugly case (wrap, power cut, half-erased sector) is tested on the Mac in a second rather than in a car in an hour. Records use the same 12-byte layout the replay library already uses, so pulled drives need no new parser anywhere.

**Tech Stack:** ESP-IDF 5.x, C++17, FreeRTOS, `esp_partition` raw flash access, NVS for the borrowed clock, Python 3 + `pyserial` on the host.

**Spec:** `docs/superpowers/specs/2026-08-27-drive-recording-design.md`

## Global Constraints

Every task's requirements implicitly include these. Values are verbatim from the spec.

- **Partition:** `logs, data, spiffs, 0x1610000, 0x9F0000` — 9.94 MB, 2,544 sectors of 4,096 bytes. Nothing existing moves.
- **Sector header:** 16 bytes — `magic[4]="MX5L"`, `u32 seq`, `u32 drive`, `u16 flags` (bit 0 = opens a drive), `u16 pad`.
- **Record:** 12 bytes, little endian — `u32 t_ms, u16 chan, u16 pad, f32 value`. Identical to `tools/build_drive_asset.py`'s `REC = struct.Struct('<IHHf')`. 340 records per sector.
- **Wipe-ahead:** the writer erases sector `(head + 1) % 2544` before writing to it. That sector holds the oldest data, so dropping the oldest is never bookkeeping.
- **Nothing may allocate or block on the UI loop.** The UI loop's only contribution is one non-blocking `xQueueSend`, dropping on a full queue — the pattern `live_link.cpp` already uses. See `SPEC.md` §6.
- **IMU: 5 Hz**, channels `imu_ax`, `imu_ay`, `imu_az`, `imu_gz`. Not 10 Hz — that halves retained history to 4.5 hours.
- **Drive ends after 20 s with no car reading.** Next reading opens a new drive with a new id.
- **A drive with fewer than 100 records is not offered for retrieval** — that is a key touched, not a drive.
- **Channel table version is 1**, carried in `STATS` and checked by the host tool. A mismatch is an error, never a guess.
- **Live data only.** Replay mode records nothing.
- C++17, `-Wall -Wextra -Werror` on the host tests (`firmware/test/host/Makefile`), which is where all `gauge_core` work is proven.

---

### Task 1: The flash seam and an empty ring

The `IFlash` interface, the layout constants, a fake flash that behaves like the real part, and a `LogBuf` that can mount an erased partition and report that it holds nothing. No writing yet.

**Files:**
- Create: `firmware/components/gauge_core/logbuf.h`
- Create: `firmware/components/gauge_core/logbuf.cpp`
- Create: `firmware/test/host/fake_flash.h`
- Test: `firmware/test/host/test_logbuf.cpp`
- Modify: `firmware/components/gauge_core/CMakeLists.txt` (add `logbuf.cpp` to `SRCS`)

**Interfaces:**
- Consumes: nothing.
- Produces: `gauge::IFlash` (virtual `sector_count()`, `read()`, `write()`, `erase_sector()`), `gauge::LogBuf`, `gauge::Record`, `gauge::kSectorSize`, `gauge::kSectorHeaderSize`, `gauge::kRecordsPerSector`, `gauge::LogBuf::mount()`, `gauge::LogBuf::drive_count()`. Every later task builds on these exact names.

- [ ] **Step 1: Write the failing test**

Create `firmware/test/host/fake_flash.h`. It must behave like NOR flash or the tests prove nothing: writes may only clear bits, never set them, and only an erase returns a sector to `0xFF`.

```cpp
// A stand-in for the real part, with the two behaviours that matter:
// a write can only clear bits, and only an erase sets them back to 1.
// Without those, a test can "pass" doing something the flash would refuse.
#pragma once
#include <cstring>
#include <vector>
#include "logbuf.h"

namespace gauge_test {

class FakeFlash : public gauge::IFlash {
public:
    explicit FakeFlash(size_t sectors)
        : sectors_(sectors), data_(sectors * gauge::kSectorSize, 0xFF) {}

    size_t sector_count() const override { return sectors_; }

    bool read(size_t off, void* dst, size_t len) override {
        if (off + len > data_.size()) return false;
        memcpy(dst, data_.data() + off, len);
        ++reads_;
        return true;
    }

    bool write(size_t off, const void* src, size_t len) override {
        if (off + len > data_.size()) return false;
        if (off % 4 || len % 4) return false;      // the real part demands this
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (size_t i = 0; i < len; ++i) {
            // A write ANDs. Setting a cleared bit needs an erase.
            if (s[i] & ~data_[off + i]) return false;
            data_[off + i] &= s[i];
        }
        ++writes_;
        return true;
    }

    bool erase_sector(size_t index) override {
        if (index >= sectors_) return false;
        memset(data_.data() + index * gauge::kSectorSize, 0xFF, gauge::kSectorSize);
        ++erases_;
        return true;
    }

    // Test-only reach-ins.
    uint8_t* raw(size_t sector) { return data_.data() + sector * gauge::kSectorSize; }
    size_t erases() const { return erases_; }
    size_t writes() const { return writes_; }
    // Simulates losing power part-way through an erase: some bytes went to
    // 0xFF, the rest did not.
    void half_erase(size_t index, size_t bytes) {
        memset(raw(index), 0xFF, bytes);
    }

private:
    size_t sectors_;
    std::vector<uint8_t> data_;
    size_t reads_ = 0, writes_ = 0, erases_ = 0;
};

}  // namespace gauge_test
```

Create `firmware/test/host/test_logbuf.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `logbuf.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `firmware/components/gauge_core/logbuf.h`:

```cpp
// A ring of flash sectors holding one record stream, oldest dropped when full.
//
// This is the part of drive recording with the bugs worth catching on a Mac:
// wrap-around, a power cut mid-write, a sector whose erase did not finish. So
// it owns no flash -- it talks to IFlash, and the board's implementation of
// that is twenty lines in main/drive_log.cpp. Everything here is host-tested.
//
// Records are the same 12 bytes tools/build_drive_asset.py writes, so a drive
// pulled off the board needs no new parser: not in the desk app, not in the
// asset compiler, not in these tests.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gauge {

constexpr size_t   kSectorSize       = 4096;
constexpr size_t   kSectorHeaderSize = 16;
constexpr uint16_t kChanTableVersion = 1;

// u32 t_ms, u16 chan, u16 pad, f32 value -- 12 bytes, every field on its own
// 4-byte boundary because Xtensa has no unaligned 32-bit load.
struct Record {
    uint32_t t_ms;
    uint16_t chan;
    uint16_t pad;
    float    value;
};
static_assert(sizeof(Record) == 12, "the record layout is the file format");

constexpr size_t kRecordsPerSector = (kSectorSize - kSectorHeaderSize) / sizeof(Record);

// Reserved channel ids. Real channels are assigned in poll.cpp table order
// (Task 6) and never reach this range.
constexpr uint16_t kChanDriveStart = 0xFFFF;   // value = epoch seconds, 0 if unknown
constexpr uint16_t kChanDriveEnd   = 0xFFFE;

// The flash, as little of it as this needs. The board implements this over
// esp_partition; the tests implement it over a byte array.
class IFlash {
public:
    virtual ~IFlash() = default;
    virtual size_t sector_count() const = 0;
    virtual bool read(size_t offset, void* dst, size_t len) = 0;
    // offset and len must be 4-byte multiples; may only clear bits.
    virtual bool write(size_t offset, const void* src, size_t len) = 0;
    virtual bool erase_sector(size_t index) = 0;
};

class LogBuf {
public:
    explicit LogBuf(IFlash& flash) : flash_(flash) {}

    // Finds the write head by reading every sector header. Call once.
    // False only if the flash itself will not answer.
    bool mount();

    size_t drive_count() const { return drive_count_; }
    size_t record_count() const { return record_count_; }

private:
    IFlash& flash_;
    size_t  drive_count_  = 0;
    size_t  record_count_ = 0;
};

}  // namespace gauge
```

Create `firmware/components/gauge_core/logbuf.cpp`:

```cpp
#include "logbuf.h"
#include <cstring>

namespace gauge {
namespace {

struct SectorHeader {
    char     magic[4];
    uint32_t seq;
    uint32_t drive;
    uint16_t flags;
    uint16_t pad;
};
static_assert(sizeof(SectorHeader) == kSectorHeaderSize, "header is the file format");

bool valid(const SectorHeader& h) { return memcmp(h.magic, "MX5L", 4) == 0; }

}  // namespace

bool LogBuf::mount() {
    drive_count_ = 0;
    record_count_ = 0;
    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return false;
        if (!valid(h)) continue;
    }
    return true;
}

}  // namespace gauge
```

Add `logbuf.cpp` to `firmware/components/gauge_core/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "version.cpp" "pid.cpp" "parse.cpp" "poll.cpp" "state.cpp" "metrics.cpp" "glow.cpp" "vehicle.cpp" "ignition.cpp" "elm327.cpp" "replay.cpp" "avail.cpp" "logbuf.cpp"
                       INCLUDE_DIRS ".")
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `=== test_logbuf ===` all ok, and `ALL SUITES PASSED` — the existing suites must still pass. The host Makefile globs `test_*.cpp` and `../../components/gauge_core/*.cpp`, so no build file changes are needed there.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/logbuf.h firmware/components/gauge_core/logbuf.cpp \
        firmware/components/gauge_core/CMakeLists.txt \
        firmware/test/host/fake_flash.h firmware/test/host/test_logbuf.cpp
git commit -m "core: the shape of the drive ring, and a flash that says no

A fake flash that only clears bits and only returns them on an erase, because
a test against a plain byte array would pass doing things the real part
refuses. Layout constants and an empty mount; nothing writes yet."
```

---

### Task 2: Append and survive a reopen

Records go in, `flush()` commits them to the live sector, and a fresh `mount()` finds both the head and the records that were already there.

**Files:**
- Modify: `firmware/components/gauge_core/logbuf.h`
- Modify: `firmware/components/gauge_core/logbuf.cpp`
- Test: `firmware/test/host/test_logbuf.cpp`

**Interfaces:**
- Consumes: `gauge::IFlash`, `gauge::LogBuf::mount()`, `gauge::Record` from Task 1.
- Produces: `LogBuf::begin_drive(uint32_t epoch_s) -> bool`, `LogBuf::append(const Record&) -> bool`, `LogBuf::flush() -> bool`, `LogBuf::end_drive() -> bool`, `LogBuf::current_drive() -> uint32_t`. Task 7 calls exactly these from the writer task.

- [ ] **Step 1: Write the failing test**

Append to `firmware/test/host/test_logbuf.cpp` (and add the calls to `main`):

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `no member named 'begin_drive' in 'gauge::LogBuf'`.

- [ ] **Step 3: Write the minimal implementation**

Add to `logbuf.h`, inside `class LogBuf`'s public section:

```cpp
    // Opens a drive. epoch_s is wall-clock seconds if the Mac has set the
    // clock, 0 if it has not -- a drive with an honest "unknown" beats one
    // with a timestamp that looks authoritative and is wrong.
    bool begin_drive(uint32_t epoch_s);

    // Buffers one record. Committed by flush(), or when the batch fills.
    bool append(const Record& r);

    // Commits the buffer. Call at least every 2 s: a power cut can only cost
    // what has not been flushed.
    bool flush();

    // Writes the end marker and commits. The 20 s silence rule that decides
    // when to call this lives in main/drive_log.cpp, not here.
    bool end_drive();

    uint32_t current_drive() const { return drive_; }
    uint32_t next_drive_id() const { return next_drive_; }
```

and to its private section:

```cpp
    static constexpr size_t kBatch = 32;      // 384 B of RAM, ~1 s of records

    bool open_sector(uint32_t drive, bool opens_drive);
    bool advance_sector(uint32_t drive, bool opens_drive);

    size_t   head_        = 0;      // sector index being written
    uint32_t seq_         = 0;      // seq of head_
    size_t   used_        = 0;      // records already committed in head_
    uint32_t drive_       = 0;      // 0 = no drive open
    uint32_t next_drive_  = 1;
    Record   batch_[kBatch]{};
    size_t   batch_n_     = 0;
    bool     mounted_     = false;
```

Implement in `logbuf.cpp`:

```cpp
namespace {

// Wrap-safe. seq is u32 and the ring outlives 2^32 sectors written, so a
// plain `a > b` eventually picks the oldest sector as the newest.
bool newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

bool erased(const void* p, size_t len) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < len; ++i) if (b[i] != 0xFF) return false;
    return true;
}

}  // namespace

bool LogBuf::mount() {
    drive_count_ = 0;
    record_count_ = 0;
    bool any = false;
    uint32_t best_seq = 0;
    size_t   best = 0;
    uint32_t max_drive = 0;

    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return false;
        if (!valid(h)) continue;
        if (!any || newer(h.seq, best_seq)) { any = true; best_seq = h.seq; best = i; }
        if (h.flags & 1) ++drive_count_;
        if (h.drive > max_drive) max_drive = h.drive;
        record_count_ += records_in(i);
    }

    if (any) {
        head_ = best;
        seq_  = best_seq;
        used_ = records_in(best);
        next_drive_ = max_drive + 1;
    } else {
        head_ = 0;
        seq_  = 0;
        used_ = kRecordsPerSector;   // forces a fresh sector on first append
        next_drive_ = 1;
    }
    drive_ = 0;
    batch_n_ = 0;
    mounted_ = true;
    return true;
}

// Records run until the first slot that is still erased. A sector that
// trails off into 0xFF is the normal case, not damage.
size_t LogBuf::records_in(size_t sector) {
    for (size_t r = 0; r < kRecordsPerSector; ++r) {
        Record x{};
        if (!flash_.read(sector * kSectorSize + kSectorHeaderSize + r * sizeof x,
                         &x, sizeof x)) return r;
        if (erased(&x, sizeof x)) return r;
    }
    return kRecordsPerSector;
}

bool LogBuf::open_sector(uint32_t drive, bool opens_drive) {
    SectorHeader h{};
    memcpy(h.magic, "MX5L", 4);
    h.seq = ++seq_;
    h.drive = drive;
    h.flags = opens_drive ? 1 : 0;
    h.pad = 0;
    if (!flash_.write(head_ * kSectorSize, &h, sizeof h)) return false;
    used_ = 0;
    return true;
}

bool LogBuf::advance_sector(uint32_t drive, bool opens_drive) {
    head_ = (head_ + 1) % flash_.sector_count();
    // Wipe-ahead: this sector holds the oldest data in the ring, so erasing
    // it here IS "drop the oldest" -- no free-space accounting anywhere.
    if (!flash_.erase_sector(head_)) return false;
    return open_sector(drive, opens_drive);
}

bool LogBuf::begin_drive(uint32_t epoch_s) {
    if (!mounted_) return false;
    if (drive_) end_drive();
    drive_ = next_drive_++;
    if (!advance_sector(drive_, /*opens_drive=*/true)) return false;
    Record m{};
    m.t_ms = 0;
    m.chan = kChanDriveStart;
    m.value = static_cast<float>(epoch_s);   // 0 when the clock is unknown
    return append(m) && flush();
}

bool LogBuf::append(const Record& r) {
    if (!mounted_ || !drive_) return false;
    batch_[batch_n_++] = r;
    return batch_n_ < kBatch ? true : flush();
}

bool LogBuf::flush() {
    if (!batch_n_) return true;
    size_t done = 0;
    while (done < batch_n_) {
        if (used_ >= kRecordsPerSector && !advance_sector(drive_, false)) return false;
        const size_t room = kRecordsPerSector - used_;
        const size_t n = (batch_n_ - done) < room ? (batch_n_ - done) : room;
        const size_t off = head_ * kSectorSize + kSectorHeaderSize + used_ * sizeof(Record);
        if (!flash_.write(off, batch_ + done, n * sizeof(Record))) return false;
        used_ += n;
        record_count_ += n;
        done += n;
    }
    batch_n_ = 0;
    return true;
}

bool LogBuf::end_drive() {
    if (!drive_) return true;
    Record m{};
    m.chan = kChanDriveEnd;
    const bool ok = append(m) && flush();
    drive_ = 0;
    return ok;
}
```

Also declare `size_t records_in(size_t sector);` in `LogBuf`'s private section, and increment `drive_count_` in `begin_drive()` after `advance_sector()` succeeds.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `test_logbuf` all ok, `ALL SUITES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/logbuf.h firmware/components/gauge_core/logbuf.cpp \
        firmware/test/host/test_logbuf.cpp
git commit -m "core: records in, and still there after a reboot

A reopen has to find the head by reading sector headers, because nothing
tracks it: a power cut gives no warning and gets no chance to write a
footer. Records run until the first still-erased slot, which is why a
half-full sector reads back correctly rather than as damage."
```

---

### Task 3: Rolling to the next sector

Filling a sector must roll to the next one, erasing it first, and the records must read back as one continuous stream across the boundary.

**Files:**
- Modify: `firmware/components/gauge_core/logbuf.h`
- Modify: `firmware/components/gauge_core/logbuf.cpp`
- Test: `firmware/test/host/test_logbuf.cpp`

**Interfaces:**
- Consumes: everything from Task 2.
- Produces: `LogBuf::read_drive(uint32_t id, RecordSink sink, void* ctx) -> bool` where `using RecordSink = bool (*)(const Record* records, size_t count, void* ctx);` — Task 8 streams a drive out over serial through this, one sector at a time, because the whole drive does not fit in RAM.

- [ ] **Step 1: Write the failing test**

Append to `test_logbuf.cpp`:

```cpp
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
```

Add `#include <vector>` to the test file.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `no member named 'read_drive'`.

- [ ] **Step 3: Write the minimal implementation**

Add to `logbuf.h` public section:

```cpp
    // Streams a drive's records in order, a sector at a time. A whole drive
    // can be 2 MB; the board hands it to the serial port in pieces rather
    // than holding it. Return false from the sink to stop early.
    using RecordSink = bool (*)(const Record* records, size_t count, void* ctx);
    bool read_drive(uint32_t id, RecordSink sink, void* ctx);
```

Implement in `logbuf.cpp`:

```cpp
// Helper: is this sector part of drive `id`?
bool LogBuf::sector_of(size_t index, uint32_t id, SectorHeader* out) {
    SectorHeader h{};
    if (!flash_.read(index * kSectorSize, &h, sizeof h)) return false;
    if (!valid(h) || h.drive != id) return false;
    if (out) *out = h;
    return true;
}

bool LogBuf::read_drive(uint32_t id, RecordSink sink, void* ctx) {
    if (!mounted_ || !id) return false;
    const size_t count = flash_.sector_count();

    // A drive's sectors are CONSECUTIVE ring indices, because the head only
    // ever advances by one -- so this needs one pass to find where the drive
    // starts and then a walk, not a search per sector. The earlier draft of
    // this searched the whole ring for each sector in turn, which on a full
    // 2544-sector partition is millions of flash reads and turns LIST into
    // a multi-minute command.
    //
    // The first surviving sector is the one whose ring predecessor is NOT
    // part of this drive. That is also the correct answer when the drive's
    // real opening sector has already been overwritten.
    size_t start = count;
    for (size_t i = 0; i < count; ++i) {
        if (!sector_of(i, id, nullptr)) continue;
        if (!sector_of((i + count - 1) % count, id, nullptr)) { start = i; break; }
    }
    if (start == count) return false;          // no such drive

    Record buf[kRecordsPerSector];
    for (size_t k = 0; k < count; ++k) {
        const size_t i = (start + k) % count;
        if (!sector_of(i, id, nullptr)) break;   // walked off the end of the drive
        const size_t n = records_in(i);
        if (n && !flash_.read(i * kSectorSize + kSectorHeaderSize, buf, n * sizeof(Record)))
            return false;
        if (n && !sink(buf, n, ctx)) return true;
        if (n < kRecordsPerSector) break;        // a partial sector is the last one
    }
    return true;
}
```

Declare `bool sector_of(size_t index, uint32_t id, SectorHeader* out);` in `LogBuf`'s private section, and move `struct SectorHeader` from the anonymous namespace in `logbuf.cpp` into `logbuf.h` (still inside `namespace gauge`) so the declaration can name it.

`read_drive` holds no state between calls — Task 4's `summarise` calls it repeatedly for the same drive and must get the same answer every time.

`Record buf[kRecordsPerSector]` is 4,080 bytes of stack. The writer task in Task 7 is created with 6144 bytes, so raise it to 8192 there; `read_drive` is only ever called from the serial task, which Task 8 gives 8192.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `test_logbuf` all ok, `ALL SUITES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/logbuf.h firmware/components/gauge_core/logbuf.cpp \
        firmware/test/host/test_logbuf.cpp
git commit -m "core: roll to the next sector, and read the stream back whole

Reading walks seq order rather than sector order, because a drive that
crossed the wrap point is contiguous in neither. The sink takes a sector at
a time: a long drive is megabytes and the board is not holding that."
```

---

### Task 4: Wrapping, and losing the oldest drive

The ring must wrap and the drive that falls off must be the oldest one — and `list()` must report honestly what is still there.

**Files:**
- Modify: `firmware/components/gauge_core/logbuf.h`
- Modify: `firmware/components/gauge_core/logbuf.cpp`
- Test: `firmware/test/host/test_logbuf.cpp`

**Interfaces:**
- Consumes: everything from Task 3.
- Produces: `struct gauge::DriveInfo { uint32_t id; uint32_t epoch_s; uint32_t records; uint32_t duration_ms; bool complete; };` and `LogBuf::list(DriveInfo* out, size_t max) -> size_t`, newest first. Tasks 8 and 9 render this as the `LIST` reply.

- [ ] **Step 1: Write the failing test**

Append to `test_logbuf.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `no member named 'list'`.

- [ ] **Step 3: Write the minimal implementation**

Add to `logbuf.h`:

```cpp
// What a drive looks like from outside. `complete` is false for a drive with
// no end marker -- the power went out mid-drive, or it is the one recording
// right now. Its records are still good.
struct DriveInfo {
    uint32_t id;
    uint32_t epoch_s;       // 0 when the clock was unknown
    uint32_t records;
    uint32_t duration_ms;
    bool     complete;
};

// A drive shorter than this is a key touched, not a drive, and list() does
// not offer it.
constexpr uint32_t kMinDriveRecords = 100;
```

and in the public section:

```cpp
    // Drives held, newest first, skipping ones below kMinDriveRecords.
    // Returns how many were written to `out`.
    size_t list(DriveInfo* out, size_t max);
    bool has_drive(uint32_t id);
```

Implement in `logbuf.cpp`:

```cpp
size_t LogBuf::list(DriveInfo* out, size_t max) {
    if (!mounted_ || !max) return 0;
    // Gather per-drive facts in one pass over the headers, then a second pass
    // for the drives that qualify. The board has 8 MB of PSRAM but this runs
    // in a task with an 8 KB stack, so nothing here is per-record.
    struct Acc { uint32_t id, first_seq, last_seq, sectors; bool opens; };
    // 2544 sectors could in principle be 2544 drives; in practice a drive is
    // never one sector. Cap the table and report the newest that fit.
    constexpr size_t kMaxDrives = 64;
    Acc acc[kMaxDrives]{};
    size_t n_acc = 0;

    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return 0;
        if (!valid(h) || !h.drive) continue;
        Acc* a = nullptr;
        for (size_t k = 0; k < n_acc; ++k) if (acc[k].id == h.drive) { a = &acc[k]; break; }
        if (!a) {
            if (n_acc == kMaxDrives) {
                // Drop the oldest entry in the table, not this one.
                size_t oldest = 0;
                for (size_t k = 1; k < n_acc; ++k)
                    if (newer(acc[oldest].last_seq, acc[k].last_seq)) oldest = k;
                acc[oldest] = Acc{};
                a = &acc[oldest];
            } else {
                a = &acc[n_acc++];
            }
            a->id = h.drive;
            a->first_seq = a->last_seq = h.seq;
        }
        if (newer(a->first_seq, h.seq)) a->first_seq = h.seq;
        if (newer(h.seq, a->last_seq)) a->last_seq = h.seq;
        a->sectors++;
        if (h.flags & 1) a->opens = true;
    }

    // Newest first, by last_seq.
    size_t written = 0;
    for (;;) {
        size_t pick = n_acc;
        for (size_t k = 0; k < n_acc; ++k) {
            if (!acc[k].id) continue;
            if (pick == n_acc || newer(acc[k].last_seq, acc[pick].last_seq)) pick = k;
        }
        if (pick == n_acc || written == max) break;
        DriveInfo info{};
        if (summarise(acc[pick].id, &info) && info.records >= kMinDriveRecords)
            out[written++] = info;
        acc[pick].id = 0;
    }
    return written;
}

bool LogBuf::has_drive(uint32_t id) {
    DriveInfo info{};
    return summarise(id, &info) && info.records >= kMinDriveRecords;
}
```

Add the private helper, which is the only place that counts records and reads markers:

```cpp
bool LogBuf::summarise(uint32_t id, DriveInfo* out) {
    struct Ctx { DriveInfo* d; bool first; } ctx{out, true};
    *out = DriveInfo{};
    out->id = id;
    auto sink = [](const Record* r, size_t n, void* c) -> bool {
        Ctx* x = static_cast<Ctx*>(c);
        for (size_t i = 0; i < n; ++i) {
            if (r[i].chan == kChanDriveStart) {
                if (x->first) { x->d->epoch_s = static_cast<uint32_t>(r[i].value); x->first = false; }
            } else if (r[i].chan == kChanDriveEnd) {
                x->d->complete = true;
            } else if (r[i].t_ms > x->d->duration_ms) {
                x->d->duration_ms = r[i].t_ms;
            }
            x->d->records++;
        }
        return true;
    };
    return read_drive(id, sink, &ctx);
}
```

Declare `bool summarise(uint32_t id, DriveInfo* out);` in `LogBuf`'s private section.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `test_logbuf` all ok, `ALL SUITES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/logbuf.h firmware/components/gauge_core/logbuf.cpp \
        firmware/test/host/test_logbuf.cpp
git commit -m "core: wrap the ring, and lose the drive that should be lost

The oldest drive going first needs no code -- it falls out of erasing the
sector ahead. What does need code is list(): a drive whose sectors have
been partly overwritten must not be offered as if it were whole, and a
five-record touch of the key is not a drive."
```

---

### Task 5: The cases the car will actually produce

Power cut mid-batch and mid-erase, a `seq` that wraps at u32, a drive that spans the wrap point. These are why the ring lives in `gauge_core` and not in `main`.

**Files:**
- Modify: `firmware/components/gauge_core/logbuf.cpp` (fixes the tests find)
- Test: `firmware/test/host/test_logbuf.cpp`

**Interfaces:**
- Consumes: everything from Task 4.
- Produces: no new API. This task hardens what exists.

- [ ] **Step 1: Write the failing test**

Append to `test_logbuf.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `no member named 'force_seq_for_test'`. After adding that, expect `test_seq_wraparound` to FAIL on ordering if `newer()` were a plain comparison, and `test_half_erased_sector_is_not_data` to FAIL if `mount()` treated a magic-less sector as unusable.

- [ ] **Step 3: Write the minimal implementation**

Add to `logbuf.h`'s public section:

```cpp
    // Test-only: starts the sequence numbers near the u32 rollover so the
    // wrap can be exercised without writing four billion sectors.
    void force_seq_for_test(uint32_t seq) { seq_ = seq; }
```

Then fix whatever the tests catch. The three known ones:

1. `newer()` must stay the wrap-safe `static_cast<int32_t>(a - b) > 0` from Task 2 — if any comparison was written as `a > b`, replace it.
2. `mount()` must count a magic-less sector as free, which the `if (!valid(h)) continue;` already does — confirm `advance_sector` does not skip it.
3. `flush()` must not lose the tail of a batch that spans a sector boundary — the `while (done < batch_n_)` loop in Task 2 handles it; the wrap test proves it.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `test_logbuf` all ok, `ALL SUITES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/logbuf.cpp firmware/components/gauge_core/logbuf.h \
        firmware/test/host/test_logbuf.cpp
git commit -m "core: the cases the car will actually produce

Power cut mid-batch and mid-erase, a seq that wraps at u32, a drive that
spans the ring's end. Every one of these happens in a car eventually and
none of them can be provoked on a bench, which is the whole argument for
this code living in gauge_core behind a fake flash."
```

---

### Task 6: Channel ids

A record stores a channel id, not a name. Ids come from `poll.cpp`'s table so the firmware and the host tool cannot disagree, and a version number makes a disagreement an error rather than mislabelled data.

**Files:**
- Modify: `firmware/components/gauge_core/poll.h`
- Modify: `firmware/components/gauge_core/poll.cpp`
- Test: `firmware/test/host/test_logbuf.cpp`

**Interfaces:**
- Consumes: `gauge::kChanTableVersion`, `gauge::kChanDriveStart`, `gauge::kChanDriveEnd` from Task 1.
- Produces: `gauge::log_chan_id(const char* key) -> uint16_t` (returns `gauge::kChanUnknown` for a key it does not know) and `gauge::log_chan_name(uint16_t id) -> const char*` (returns `nullptr` for an unknown id). Task 7 calls the first; Task 9 mirrors both in Python.

- [ ] **Step 1: Write the failing test**

Append to `test_logbuf.cpp`:

```cpp
static void test_channel_ids() {
    // Ids come from poll.cpp's PID table, which is sorted by PID, so rpm
    // (0x0C) sits below coolant... no: 0x05 is coolant, 0x0C is rpm.
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
```

Add `#include <string>` and `#include "poll.h"` to the test file.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd firmware/test/host && make build/test_logbuf
```

Expected: FAIL to compile — `no member named 'log_chan_id' in namespace 'gauge'`.

- [ ] **Step 3: Write the minimal implementation**

Add to `firmware/components/gauge_core/poll.h`:

```cpp
// Channel ids for the recorder. A 12-byte record has room for an id, not a
// name, so the id IS the file format: it is the index into the PID table
// below, and kChanTableVersion in logbuf.h must change if that table's order
// ever does. Non-PID channels (volts, the IMU) get the extras block, well
// clear of the PID range so adding a PID never moves them.
constexpr uint16_t kChanUnknown = 0xFFFD;
constexpr uint16_t kChanExtras  = 0x0200;

uint16_t    log_chan_id(const char* key);
const char* log_chan_name(uint16_t id);
```

Add to `firmware/components/gauge_core/poll.cpp`, inside the anonymous namespace:

```cpp
// Channels with no PID behind them. Order is the file format; append only.
const char* const kExtras[] = {"volts", "imu_ax", "imu_ay", "imu_az", "imu_gz"};
constexpr size_t kExtraCount = sizeof kExtras / sizeof kExtras[0];
```

and after `keys_for`:

```cpp
uint16_t log_chan_id(const char* key) {
    if (!key) return kChanUnknown;
    uint16_t i = 0;
    for (const auto& kv : table()) {              // std::map: sorted by PID
        if (std::strcmp(kv.second.key, key) == 0) return i;
        ++i;
    }
    for (size_t e = 0; e < kExtraCount; ++e)
        if (std::strcmp(kExtras[e], key) == 0)
            return static_cast<uint16_t>(kChanExtras + e);
    return kChanUnknown;
}

const char* log_chan_name(uint16_t id) {
    if (id >= kChanExtras) {
        const size_t e = id - kChanExtras;
        return e < kExtraCount ? kExtras[e] : nullptr;
    }
    uint16_t i = 0;
    for (const auto& kv : table()) {
        if (i++ == id) return kv.second.key;
    }
    return nullptr;
}
```

Add `#include <cstring>` to `poll.cpp`. Note `PidInfo::key` is a `const char*`, so `strcmp` is the comparison, not `==`.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd firmware/test/host && make test
```

Expected: `test_logbuf` all ok, `ALL SUITES PASSED`. `test_poll.cpp` and `test_poll_order.cpp` must still pass — the PID table is not being reordered, only read.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/gauge_core/poll.h firmware/components/gauge_core/poll.cpp \
        firmware/test/host/test_logbuf.cpp
git commit -m "core: a channel id the host tool cannot get wrong

Twelve bytes have room for an id, not a name, so the id is the file format.
Deriving it from poll.cpp's table means the firmware and the puller cannot
drift apart silently; a version number in the header means that if they do,
it is an error rather than a column of coolant labelled as rpm."
```

---

### Task 7: The recorder on the board

The real flash, the writer task, the drive boundaries, the IMU, and the one line in the UI loop that feeds it. First task with hardware in the loop.

**Files:**
- Create: `firmware/main/drive_log.h`
- Create: `firmware/main/drive_log.cpp`
- Modify: `firmware/partitions.csv`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/main/main.cpp` (the `live::next` drain loop, ~line 276; the IMU print at ~line 395)
- Modify: `firmware/main/live_link.cpp` (`build_poll_cycle(..., /*log_all=*/true)`)

**Interfaces:**
- Consumes: `gauge::LogBuf`, `gauge::IFlash`, `gauge::Record`, `gauge::log_chan_id` from Tasks 1-6. `live::Sample` from `live_link.h`. `imu_read`/`imu_sample_t` from `imu.h`.
- Produces: `void drive_log_init(void)`, `void drive_log_sample(const char* key, float value, double t_s)`, `void drive_log_set_epoch(uint32_t epoch_s)`, `bool drive_log_stats(drive_log_stats_t* out)`, and `gauge::LogBuf* drive_log_buf(void)` for Task 8's serial commands. All `extern "C"`.

- [ ] **Step 1: Add the partition and prove the board sees it**

Modify `firmware/partitions.csv` — append one line, changing nothing above it:

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x400000
assets,   data, spiffs,  0x410000, 0x1000000
drives,   data, spiffs,  0x1410000, 0x200000
logs,     data, spiffs,  0x1610000, 0x9F0000
```

Verify the arithmetic before flashing anything: `0x1610000 + 0x9F0000 = 0x2000000` = exactly 32 MB, and `0x9F0000 / 0x1000 = 2544` sectors.

```bash
python3 -c "print(hex(0x1610000+0x9F0000), 0x9F0000//0x1000)"
```

Expected: `0x2000000 2544`.

- [ ] **Step 2: Write the recorder**

Create `firmware/main/drive_log.h`:

```cpp
// Records every reading of every drive to the `logs` partition.
//
// The gauge's real life is the car's own USB socket with no laptop attached,
// and until now that life left no trace: readings arrived, got drawn, and were
// gone. This keeps them, oldest drive dropped when the 10 MB fills, and
// tools/pull_drives.py hands them back as logs/*.csv.
//
// Nothing here may run on the UI loop. Erasing a sector takes tens of
// milliseconds, which is visibly dropped frames, and the draw path is already
// the fragile one (SPEC.md s6). So the UI loop's whole contribution is one
// non-blocking queue send, and a task on core 0 does the flash.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t drives;        // drives held and offerable
    uint32_t records;       // records committed since mount
    uint32_t sectors;       // sectors in the partition
    uint32_t dropped;       // samples the queue could not take
    uint32_t epoch_s;       // borrowed wall clock, 0 if never set
    uint16_t table_version;
} drive_log_stats_t;

// Mounts the ring and starts the writer task. Safe to call with no partition:
// it says so and records nothing.
void drive_log_init(void);

// Hands one live reading to the writer. Called from the UI loop -- never
// blocks, never allocates, drops when the queue is full.
void drive_log_sample(const char* key, float value, double t_s);

// The Mac's clock, from `TIME <epoch>`. Applies to drives opened afterwards.
void drive_log_set_epoch(uint32_t epoch_s);

bool drive_log_stats(drive_log_stats_t* out);

#ifdef __cplusplus
}

// For serial_cmd.cpp's LIST/GET. Null until drive_log_init has mounted.
// Only the serial task may touch it, and only while holding drive_log_lock().
namespace gauge { class LogBuf; }
gauge::LogBuf* drive_log_buf(void);
bool drive_log_lock(int ms);
void drive_log_unlock(void);
#endif
```

Create `firmware/main/drive_log.cpp`:

```cpp
#include "drive_log.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "flight_log.h"
#include "imu.h"
#include "logbuf.h"
#include "poll.h"

namespace {

const char* TAG = "drivelog";

// A drive ends when the car goes quiet for this long. At key-off the vLinker
// sleeps and the poll loop notices within ~15 s -- measured in the car
// 2026-08-27 -- so 20 s closes the drive without splitting one at a red light.
constexpr int64_t kSilenceUs = 20LL * 1000 * 1000;
// Flush at least this often: a power cut can only cost what is unflushed.
constexpr int64_t kFlushUs = 2LL * 1000 * 1000;
// 5 Hz, not 10: the IMU is four channels against the car's twelve readings a
// second, and 10 Hz would halve the history the partition holds (design s3).
constexpr int64_t kImuPeriodUs = 200LL * 1000;
// Persist the borrowed clock this often, so a boot with no Mac has a floor.
constexpr int64_t kClockSaveUs = 300LL * 1000 * 1000;

struct QSample {
    uint16_t chan;
    float    value;
    double   t_s;
};

// The real part, behind gauge_core's seam. This is the only file that knows
// the ring is in flash at all.
class PartitionFlash : public gauge::IFlash {
public:
    bool open() {
        part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         ESP_PARTITION_SUBTYPE_ANY, "logs");
        return part_ != nullptr;
    }
    size_t sector_count() const override { return part_ ? part_->size / gauge::kSectorSize : 0; }
    bool read(size_t off, void* dst, size_t len) override {
        return part_ && esp_partition_read(part_, off, dst, len) == ESP_OK;
    }
    bool write(size_t off, const void* src, size_t len) override {
        return part_ && esp_partition_write(part_, off, src, len) == ESP_OK;
    }
    bool erase_sector(size_t index) override {
        return part_ && esp_partition_erase_range(part_, index * gauge::kSectorSize,
                                                  gauge::kSectorSize) == ESP_OK;
    }
private:
    const esp_partition_t* part_ = nullptr;
};

PartitionFlash   g_flash;
gauge::LogBuf*   g_log = nullptr;
QueueHandle_t    g_q = nullptr;
SemaphoreHandle_t g_lock = nullptr;
uint32_t         g_dropped = 0;
uint32_t         g_epoch_at_boot = 0;      // wall clock of uptime 0, 0 if unknown
bool             g_have_imu = false;

uint32_t wall_now() {
    if (!g_epoch_at_boot) return 0;
    return g_epoch_at_boot + (uint32_t)(esp_timer_get_time() / 1000000);
}

void save_clock() {
    const uint32_t now = wall_now();
    if (!now) return;
    nvs_handle_t h;
    if (nvs_open("drivelog", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "clock", now);
    nvs_commit(h);
    nvs_close(h);
}

void task(void*) {
    int64_t last_sample = 0;              // 0 = no drive open
    int64_t last_flush = esp_timer_get_time();
    int64_t last_imu = 0;
    int64_t last_clock_save = esp_timer_get_time();
    double  drive_t0 = 0.0;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        // Close a drive the car has stopped feeding.
        if (last_sample && now - last_sample > kSilenceUs) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_log->end_drive();
            xSemaphoreGive(g_lock);
            flight_log("drive %u closed, %u records",
                       (unsigned)g_log->current_drive(), (unsigned)g_log->record_count());
            last_sample = 0;
        }

        QSample s{};
        while (xQueueReceive(g_q, &s, 0) == pdTRUE) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            if (!last_sample) {
                drive_t0 = s.t_s;
                g_log->begin_drive(wall_now());
                flight_log("drive %u opened, clock %s",
                           (unsigned)g_log->current_drive(),
                           wall_now() ? "known" : "UNKNOWN");
            }
            gauge::Record r{};
            const double dt = s.t_s - drive_t0;
            r.t_ms = dt > 0 ? (uint32_t)(dt * 1000.0) : 0;
            r.chan = s.chan;
            r.value = s.value;
            g_log->append(r);
            xSemaphoreGive(g_lock);
            last_sample = now;
        }

        // The IMU moves here from the UI loop, where it was read once a
        // second only to be printed.
        if (last_sample && g_have_imu && now - last_imu > kImuPeriodUs) {
            last_imu = now;
            imu_sample_t im{};
            if (imu_read(&im)) {
                const double t_s = now / 1e6;
                drive_log_sample("imu_ax", im.ax, t_s);
                drive_log_sample("imu_ay", im.ay, t_s);
                drive_log_sample("imu_az", im.az, t_s);
                drive_log_sample("imu_gz", im.gz, t_s);
            }
        }

        if (now - last_flush > kFlushUs) {
            last_flush = now;
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_log->flush();
            xSemaphoreGive(g_lock);
        }
        if (now - last_clock_save > kClockSaveUs) {
            last_clock_save = now;
            save_clock();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

extern "C" void drive_log_init(void) {
    if (!g_flash.open()) {
        ESP_LOGW(TAG, "no 'logs' partition -- drives will not be recorded");
        flight_log("no logs partition, NOT recording");
        return;
    }
    static gauge::LogBuf log(g_flash);
    if (!log.mount()) {
        ESP_LOGE(TAG, "logs partition will not mount");
        flight_log("logs partition will not mount, NOT recording");
        return;
    }
    g_log = &log;
    g_lock = xSemaphoreCreateMutex();
    g_q = xQueueCreate(128, sizeof(QSample));
    g_have_imu = imu_address() != 0;

    // The floor for a boot with no Mac: the drive happened after this.
    nvs_handle_t h;
    uint32_t floor_s = 0;
    if (nvs_open("drivelog", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "clock", &floor_s);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%u sectors, %u drives held, %u records, clock floor %u",
             (unsigned)g_flash.sector_count(), (unsigned)log.drive_count(),
             (unsigned)log.record_count(), (unsigned)floor_s);
    flight_log("drive log up: %u drives held", (unsigned)log.drive_count());

    // Priority 3 -- below the UI and below live_link's 4. Core 0, beside the
    // radio, so LVGL's render on core 1 never waits behind a flash erase.
    xTaskCreatePinnedToCore(task, "drivelog", 8192, nullptr, 3, nullptr, 0);
}

extern "C" void drive_log_sample(const char* key, float value, double t_s) {
    if (!g_q) return;
    const uint16_t chan = gauge::log_chan_id(key);
    if (chan == gauge::kChanUnknown) return;
    QSample s{chan, value, t_s};
    // Dropped rather than blocked on, exactly as live_link drops readings the
    // UI cannot take: the draw loop must never wait on flash.
    if (xQueueSend(g_q, &s, 0) != pdTRUE) ++g_dropped;
}

extern "C" void drive_log_set_epoch(uint32_t epoch_s) {
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    g_epoch_at_boot = epoch_s > up ? epoch_s - up : 0;
    save_clock();
}

extern "C" bool drive_log_stats(drive_log_stats_t* out) {
    if (!out || !g_log) return false;
    *out = drive_log_stats_t{};
    out->drives = (uint32_t)g_log->drive_count();
    out->records = (uint32_t)g_log->record_count();
    out->sectors = (uint32_t)g_flash.sector_count();
    out->dropped = g_dropped;
    out->epoch_s = wall_now();
    out->table_version = gauge::kChanTableVersion;
    return true;
}

gauge::LogBuf* drive_log_buf(void) { return g_log; }
bool drive_log_lock(int ms) {
    return g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void drive_log_unlock(void) { if (g_lock) xSemaphoreGive(g_lock); }
```

- [ ] **Step 3: Wire it in**

`firmware/main/CMakeLists.txt` — add the source and the `nvs_flash` requirement is already there:

```cmake
idf_component_register(SRCS "main.cpp" "imu.c" "drive_source.c" "live_link.cpp" "flight_log.cpp" "drive_log.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core gauge_ui gauge_platform espressif__esp_lvgl_adapter driver esp_driver_i2c esp_timer esp_partition nvs_flash)
```

`firmware/main/main.cpp` — add `#include "drive_log.h"` beside the other includes, call `drive_log_init();` immediately after `imu_init()` at ~line 191 (it needs `imu_address()` to know whether the IMU is there), and add the one line to the live drain loop at ~line 276:

```cpp
        if (live_mode) {
            live::Sample smp{};
            while (live::next(&smp)) {
                st.set(smp.key, smp.value);
                drive_log_sample(smp.key, smp.value, smp.t_s);
```

`firmware/main/live_link.cpp` at ~line 96 — record everything the car answers:

```cpp
                // Every PID the car supports, not just the display set: the
                // recorder wants all of it, and build_poll_cycle keeps
                // kPollFast in front of each one so the needle does not
                // notice (design s3).
                std::vector<uint8_t> cycle =
                    gauge::build_poll_cycle(supported, /*log_all=*/true);
```

- [ ] **Step 4: Build, flash, and test on the board**

The partition table changed, so this needs a full flash of the app and the table — but **not** the 13 MB assets partition, which does not move.

```bash
source tools/idf_env.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Expected on the console:
- `drivelog: 2544 sectors, 0 drives held, 0 records, clock floor 0`
- `flight: ... drive log up: 0 drives held`
- No `NO_MEM`, no draw failures, and the fps line still reporting 21-24 — a drop here means flash work reached the display and the task priority or core pinning is wrong.

Then in the car, with the Mac's Bluetooth OFF (a held BLE peripheral does not advertise, so the board cannot see the adapter while the laptop has it):
- `live: NN PIDs` where NN is around 50, and `drivelog: drive 1 opened, clock UNKNOWN`
- Let it run two minutes, key off, wait 25 s: `drive 1 closed, N records`
- Key on again: `drive 2 opened`

Verify the frame rate held while recording. Note `verify_port.sh` is unaffected — nothing in the replay path changed — but run it anyway since `poll.cpp` was touched in Task 6:

```bash
tools/verify_port.sh
```

- [ ] **Step 5: Commit**

```bash
git add firmware/partitions.csv firmware/main/drive_log.h firmware/main/drive_log.cpp \
        firmware/main/CMakeLists.txt firmware/main/main.cpp firmware/main/live_link.cpp
git commit -m "firmware: the gauge starts keeping the drives

Ten megabytes at the tail of flash that nothing was using, a task on core 0
that owns every write to it, and one non-blocking queue send in the UI loop.
The erase that reclaims the oldest drive takes tens of milliseconds, which
is why none of it happens where the panel is drawing.

The poll cycle now asks for every PID the car answers rather than the
fourteen the gauge draws. That is free on screen: build_poll_cycle already
puts rpm, speed and throttle in front of every other request, so the needle
keeps its refresh and the other forty channels come round every few seconds.

The IMU moves off the UI loop, where it was read once a second only to be
printed, and into the recorder at 5 Hz."
```

---

### Task 8: Getting it off over USB

A line reader on the console that answers `TIME`, `STATS`, `LIST`, `GET` and `ERASE`.

**Files:**
- Create: `firmware/main/serial_cmd.h`
- Create: `firmware/main/serial_cmd.cpp`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/main/main.cpp`

**Interfaces:**
- Consumes: `drive_log_buf()`, `drive_log_lock()`, `drive_log_unlock()`, `drive_log_set_epoch()`, `drive_log_stats()` from Task 7; `gauge::LogBuf::list`, `::read_drive`, `gauge::DriveInfo`, `gauge::kChanTableVersion` from Tasks 1-4.
- Produces: `void serial_cmd_init(void)`, and the wire protocol Task 9 speaks.

- [ ] **Step 1: Write the protocol down, then the reader**

The protocol, exactly as Task 9 will parse it. Every reply ends with a line starting `OK` or `ERR`:

```
> TIME 1756300000
OK clock set

> STATS
STATS sectors=2544 drives=3 records=41233 dropped=0 epoch=1756300512 table=1
OK

> LIST
DRIVE id=4 epoch=1756300000 records=18422 ms=1640000 complete=1
DRIVE id=3 epoch=0 records=9110 ms=910000 complete=1
OK 2 drives

> GET 4
BEGIN 4 18422
<base64 of 12-byte records, 4080 bytes per line max>
...
END crc32=a1b2c3d4
OK

> ERASE CONFIRM
OK erased
```

Create `firmware/main/serial_cmd.h`:

```cpp
// A line reader on the USB console, so tools/pull_drives.py can take the
// recorded drives off the board.
//
// This shares the port with `idf.py monitor` -- they cannot both be open. The
// puller says so when the port is busy rather than hanging.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the reader task. Harmless if the recorder never mounted: the
// commands answer ERR instead.
void serial_cmd_init(void);

#ifdef __cplusplus
}
#endif
```

Create `firmware/main/serial_cmd.cpp`:

```cpp
#include "serial_cmd.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drive_log.h"
#include "logbuf.h"

namespace {

// 3 records per base64 line group: 36 bytes -> 48 chars, no padding, so a
// line never carries a partial record and the host can decode line by line.
constexpr size_t kRecordsPerLine = 3;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_line(const uint8_t* in, size_t len) {
    char out[80];
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = kB64[(v >> 6) & 63];
        out[o++] = kB64[v & 63];
    }
    out[o] = 0;
    printf("%s\n", out);
}

struct GetCtx {
    uint32_t crc;
    uint32_t sent;
};

bool emit(const gauge::Record* r, size_t n, void* ctx) {
    GetCtx* g = static_cast<GetCtx*>(ctx);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(r);
    const size_t total = n * sizeof(gauge::Record);
    g->crc = esp_rom_crc32_le(g->crc, bytes, total);
    for (size_t off = 0; off < total; off += kRecordsPerLine * sizeof(gauge::Record)) {
        const size_t chunk = total - off < kRecordsPerLine * sizeof(gauge::Record)
                                 ? total - off
                                 : kRecordsPerLine * sizeof(gauge::Record);
        b64_line(bytes + off, chunk);
    }
    g->sent += n;
    return true;
}

void cmd_stats() {
    drive_log_stats_t s{};
    if (!drive_log_stats(&s)) { printf("ERR no recorder\n"); return; }
    printf("STATS sectors=%u drives=%u records=%u dropped=%u epoch=%u table=%u\n",
           (unsigned)s.sectors, (unsigned)s.drives, (unsigned)s.records,
           (unsigned)s.dropped, (unsigned)s.epoch_s, (unsigned)s.table_version);
    printf("OK\n");
}

void cmd_list() {
    gauge::LogBuf* log = drive_log_buf();
    if (!log) { printf("ERR no recorder\n"); return; }
    if (!drive_log_lock(5000)) { printf("ERR busy\n"); return; }
    gauge::DriveInfo info[32]{};
    const size_t n = log->list(info, 32);
    drive_log_unlock();
    for (size_t i = 0; i < n; ++i)
        printf("DRIVE id=%u epoch=%u records=%u ms=%u complete=%d\n",
               (unsigned)info[i].id, (unsigned)info[i].epoch_s,
               (unsigned)info[i].records, (unsigned)info[i].duration_ms,
               info[i].complete ? 1 : 0);
    printf("OK %u drives\n", (unsigned)n);
}

void cmd_get(uint32_t id) {
    gauge::LogBuf* log = drive_log_buf();
    if (!log) { printf("ERR no recorder\n"); return; }
    if (!drive_log_lock(5000)) { printf("ERR busy\n"); return; }
    gauge::DriveInfo info[32]{};
    const size_t n = log->list(info, 32);
    uint32_t records = 0;
    for (size_t i = 0; i < n; ++i) if (info[i].id == id) records = info[i].records;
    if (!records) { drive_log_unlock(); printf("ERR no drive %u\n", (unsigned)id); return; }
    printf("BEGIN %u %u\n", (unsigned)id, (unsigned)records);
    GetCtx g{0, 0};
    const bool ok = log->read_drive(id, emit, &g);
    drive_log_unlock();
    printf("END crc32=%08x\n", (unsigned)g.crc);
    printf(ok && g.sent == records ? "OK\n" : "ERR short read\n");
}

void task(void*) {
    char line[64];
    for (;;) {
        // The console is USB-Serial-JTAG; stdin is line buffered by the VFS.
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "TIME ", 5)) {
            drive_log_set_epoch((uint32_t)strtoul(line + 5, nullptr, 10));
            printf("OK clock set\n");
        } else if (!strcmp(line, "STATS")) {
            cmd_stats();
        } else if (!strcmp(line, "LIST")) {
            cmd_list();
        } else if (!strncmp(line, "GET ", 4)) {
            cmd_get((uint32_t)strtoul(line + 4, nullptr, 10));
        } else if (!strcmp(line, "ERASE CONFIRM")) {
            printf("ERR not implemented\n");     // Step 2
        } else if (line[0]) {
            printf("ERR unknown command\n");
        }
        fflush(stdout);
    }
}

}  // namespace

extern "C" void serial_cmd_init(void) {
    // Priority 2: below the recorder, well below the UI. Nothing waits on it.
    // 8192 bytes because read_drive puts a 4 KB sector on the stack.
    xTaskCreatePinnedToCore(task, "serialcmd", 8192, nullptr, 2, nullptr, 0);
}
```

- [ ] **Step 2: Implement ERASE**

Add to `logbuf.h`'s public section and implement in `logbuf.cpp`:

```cpp
    // Wipes every sector and starts over. The only way back from a ring the
    // channel table has outgrown.
    bool erase_all();
```

```cpp
bool LogBuf::erase_all() {
    if (!mounted_) return false;
    for (size_t i = 0; i < flash_.sector_count(); ++i)
        if (!flash_.erase_sector(i)) return false;
    return mount();
}
```

Add a host test for it in `test_logbuf.cpp`:

```cpp
static void test_erase_all() {
    FakeFlash f(8);
    gauge::LogBuf log(f);
    log.mount();
    drive_of(log, 1756300000u, 200);
    check("erase_all", log.erase_all(), true);
    check("nothing left", (int)log.drive_count(), 0);
    check("still usable", log.begin_drive(0), true);
}
```

and replace the `ERR not implemented` branch in `serial_cmd.cpp`:

```cpp
        } else if (!strcmp(line, "ERASE CONFIRM")) {
            gauge::LogBuf* log = drive_log_buf();
            if (!log) { printf("ERR no recorder\n"); }
            else if (!drive_log_lock(30000)) { printf("ERR busy\n"); }
            else { const bool ok = log->erase_all(); drive_log_unlock();
                   printf(ok ? "OK erased\n" : "ERR erase failed\n"); }
        } else if (!strcmp(line, "ERASE")) {
            printf("ERR say 'ERASE CONFIRM'\n");
```

- [ ] **Step 3: Wire it in and check the host tests still pass**

`firmware/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp" "imu.c" "drive_source.c" "live_link.cpp" "flight_log.cpp" "drive_log.cpp" "serial_cmd.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core gauge_ui gauge_platform espressif__esp_lvgl_adapter driver esp_driver_i2c esp_timer esp_partition nvs_flash esp_rom)
```

`firmware/main/main.cpp`: `#include "serial_cmd.h"` and `serial_cmd_init();` right after `drive_log_init();`.

```bash
cd firmware/test/host && make test
```

Expected: `ALL SUITES PASSED`.

- [ ] **Step 4: Test on the board by hand**

```bash
source tools/idf_env.sh
cd firmware && idf.py build && idf.py -p /dev/cu.usbmodem* flash
# monitor doubles as a terminal you can type into
idf.py -p /dev/cu.usbmodem* monitor
```

Type each command and check the reply shape:
- `STATS` → a `STATS ...` line with `table=1`, then `OK`
- `TIME 1756300000` → `OK clock set`, then `STATS` shows a plausible `epoch=`
- `LIST` → the drives recorded in Task 7's car test, newest first
- `GET <id>` → `BEGIN`, base64 lines, `END crc32=`, `OK`
- `GET 999` → `ERR no drive 999`
- `ERASE` → `ERR say 'ERASE CONFIRM'`

The board must keep drawing throughout — a `GET` of a long drive is thousands of lines and must not stall the panel.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/serial_cmd.h firmware/main/serial_cmd.cpp \
        firmware/main/CMakeLists.txt firmware/main/main.cpp \
        firmware/components/gauge_core/logbuf.h firmware/components/gauge_core/logbuf.cpp \
        firmware/test/host/test_logbuf.cpp
git commit -m "firmware: ask the gauge what it saw

A line reader on the console: TIME to lend it the Mac's clock, LIST for what
it is holding, GET to stream a drive out as base64 with a crc32 at the end.

Three records per line, deliberately: 36 bytes encodes to 48 characters with
no padding, so a line never carries half a record and the puller can decode
as it reads instead of buffering megabytes."
```

---

### Task 9: The puller, and the drive that closes the loop

One command on the Mac that sets the clock, pulls new drives, and writes them into `logs/` in the shape everything else already reads.

**Files:**
- Create: `tools/pull_drives.py`
- Modify: `requirements.txt` (add `pyserial`)
- Modify: `SPEC.md` (a §15 describing recording and retrieval)
- Modify: `README.md` (the pull command beside the existing launchers)

**Interfaces:**
- Consumes: the protocol from Task 8; the channel ids from Task 6, mirrored in Python.
- Produces: `logs/drive-YYYYmmdd-HHMMSS.csv` in the existing `iso,t,key,value` shape, which `tools/build_drive_asset.py` and the desk replay already read.

- [ ] **Step 1: Write the puller**

Create `tools/pull_drives.py`:

```python
"""Pull recorded drives off the gauge and write them into logs/.

The gauge records every reading of every drive to flash (SPEC.md s15). This
takes them off over the USB console and writes the same four-column CSV the
desk replay and tools/build_drive_asset.py already read -- so a drive nobody
watched can be replayed, and can be compiled straight back into the replay
library it will be played from.

The console is shared with `idf.py monitor`; they cannot both be open. If the
port is busy this says so rather than hanging.

Usage: .venv/bin/python tools/pull_drives.py [--port /dev/cu.usbmodemXXXX]
                                             [--list] [--force]
"""
import argparse
import base64
import binascii
import datetime as dt
import glob
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing. Run: .venv/bin/pip install pyserial")

REC = struct.Struct('<IHHf')          # identical to build_drive_asset.py
TABLE_VERSION = 1

# The PID table from firmware/components/gauge_core/poll.cpp, in PID order,
# which is what a channel id indexes. If poll.cpp's table gains an entry in
# the middle, kChanTableVersion in logbuf.h must change and so must this --
# which is what the version check below is for.
PID_KEYS = [
    'fuel_status', 'load', 'coolant', 'stft1', 'ltft1', 'stft2', 'ltft2',
    'fuel_press', 'map', 'rpm', 'speed', 'timing', 'intake', 'maf',
    'throttle', 'o2_b1s1', 'o2_b1s2', 'run_time', 'dist_mil', 'rail_press',
    'rail_gauge', 'egr_cmd', 'egr_err', 'evap_purge', 'fuel_level', 'warmups',
    'dist_clear', 'evap_press', 'baro', 'cat_b1s1', 'cat_b2s1', 'cat_b1s2',
    'cat_b2s2', 'ctrl_volt', 'abs_load', 'equiv_ratio', 'rel_thr', 'ambient',
    'thr_b', 'thr_c', 'pedal_d', 'pedal_e', 'pedal_f', 'thr_actuator',
    'time_mil', 'time_clear', 'ethanol', 'rail_abs', 'pedal', 'hybrid_soc',
    'oil', 'inject_timing', 'fuel_rate', 'torque_demand', 'act_torque',
    'ref_torque',
]
EXTRAS_BASE = 0x0200
EXTRA_KEYS = ['volts', 'imu_ax', 'imu_ay', 'imu_az', 'imu_gz']
CHAN_DRIVE_START = 0xFFFF
CHAN_DRIVE_END = 0xFFFE


def chan_name(chan):
    if chan >= EXTRAS_BASE:
        i = chan - EXTRAS_BASE
        return EXTRA_KEYS[i] if i < len(EXTRA_KEYS) else None
    return PID_KEYS[chan] if chan < len(PID_KEYS) else None


def find_port():
    for pattern in ('/dev/cu.usbmodem*', '/dev/tty.usbmodem*'):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    sys.exit('no board found. Plug it in, or pass --port.')


class Gauge:
    def __init__(self, port):
        try:
            self.ser = serial.Serial(port, 115200, timeout=5)
        except serial.SerialException as e:
            sys.exit('cannot open %s: %s\n'
                     '(is `idf.py monitor` still open? they share the port)'
                     % (port, e))
        # The board prints its own log lines; drain whatever is in flight.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, cmd):
        self.ser.write((cmd + '\n').encode())
        self.ser.flush()

    def lines(self):
        """Yields reply lines until OK/ERR, skipping the board's own logging."""
        while True:
            raw = self.ser.readline()
            if not raw:
                sys.exit('the board stopped answering')
            line = raw.decode('utf-8', 'replace').strip()
            if not line:
                continue
            # Board log lines are prefixed (I/W/E or "flight:"/"live:"); the
            # protocol's are not. Anything unrecognised is logging, not data.
            if line.startswith(('OK', 'ERR')):
                yield line
                return
            yield line

    def command(self, cmd):
        self.send(cmd)
        out = list(self.lines())
        if out[-1].startswith('ERR'):
            sys.exit('%s -> %s' % (cmd, out[-1]))
        return out[:-1], out[-1]


def stats(g):
    body, _ = g.command('STATS')
    for line in body:
        if line.startswith('STATS '):
            return dict(kv.split('=', 1) for kv in line.split()[1:])
    sys.exit('no STATS in the reply')


def drives(g):
    body, _ = g.command('LIST')
    out = []
    for line in body:
        if not line.startswith('DRIVE '):
            continue
        d = dict(kv.split('=', 1) for kv in line.split()[1:])
        out.append({'id': int(d['id']), 'epoch': int(d['epoch']),
                    'records': int(d['records']), 'ms': int(d['ms']),
                    'complete': d['complete'] == '1'})
    return out


def fetch(g, drive_id):
    g.send('GET %d' % drive_id)
    blob = bytearray()
    expect = None
    crc = None
    for line in g.lines():
        if line.startswith('BEGIN '):
            expect = int(line.split()[2])
        elif line.startswith('END '):
            crc = int(line.split('=', 1)[1], 16)
        elif line.startswith(('OK', 'ERR')):
            if line.startswith('ERR'):
                sys.exit('GET %d -> %s' % (drive_id, line))
        elif expect is not None and crc is None:
            try:
                blob += base64.b64decode(line, validate=True)
            except binascii.Error:
                continue          # a board log line landed mid-stream
    got = binascii.crc32(bytes(blob)) & 0xFFFFFFFF
    if crc is not None and got != crc:
        sys.exit('drive %d: crc32 mismatch (got %08x, board said %08x)'
                 % (drive_id, got, crc))
    if expect is not None and len(blob) // REC.size != expect:
        sys.exit('drive %d: got %d records, board said %d'
                 % (drive_id, len(blob) // REC.size, expect))
    return [REC.unpack_from(blob, o) for o in range(0, len(blob), REC.size)]


def write_csv(root, drive, records):
    epoch = drive['epoch']
    if epoch:
        start = dt.datetime.fromtimestamp(epoch)
        name = 'drive-%s.csv' % start.strftime('%Y%m%d-%H%M%S')
    else:
        # No clock when this was recorded. An honest name beats a wrong one.
        start = None
        name = 'drive-unknown-%d.csv' % drive['id']
    path = os.path.join(root, 'logs', name)
    skipped = 0
    with open(path, 'w', newline='') as fh:
        fh.write('iso,t,key,value\n')
        for t_ms, chan, _pad, value in records:
            if chan in (CHAN_DRIVE_START, CHAN_DRIVE_END):
                continue
            key = chan_name(chan)
            if key is None:
                skipped += 1
                continue
            t = t_ms / 1000.0
            if start:
                iso = (start + dt.timedelta(seconds=t)).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]
            else:
                iso = ''
            fh.write('%s,%.3f,%s,%g\n' % (iso, t, key, value))
    return path, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--list', action='store_true', help='show what is held, pull nothing')
    ap.add_argument('--force', action='store_true', help='re-pull drives already in logs/')
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    g = Gauge(args.port or find_port())

    s = stats(g)
    if int(s.get('table', -1)) != TABLE_VERSION:
        sys.exit('board channel table is v%s, this tool speaks v%d -- update '
                 'PID_KEYS from poll.cpp before trusting anything it says'
                 % (s.get('table'), TABLE_VERSION))
    print('board: %s sectors, %s drives held, %s records, %s samples dropped'
          % (s['sectors'], s['drives'], s['records'], s['dropped']))

    now = int(time.time())
    g.command('TIME %d' % now)
    print('clock set to %s' % dt.datetime.fromtimestamp(now).isoformat(' ', 'seconds'))

    held = drives(g)
    if not held:
        print('no drives held.')
        return
    for d in held:
        when = (dt.datetime.fromtimestamp(d['epoch']).isoformat(' ', 'seconds')
                if d['epoch'] else 'clock unknown')
        print('  drive %-4d %-20s %7d records  %5.1f min%s'
              % (d['id'], when, d['records'], d['ms'] / 60000.0,
                 '' if d['complete'] else '  (unfinished)'))
    if args.list:
        return

    existing = set(os.path.basename(p) for p in glob.glob(os.path.join(root, 'logs', '*.csv')))
    for d in held:
        guess = ('drive-%s.csv' % dt.datetime.fromtimestamp(d['epoch']).strftime('%Y%m%d-%H%M%S')
                 if d['epoch'] else 'drive-unknown-%d.csv' % d['id'])
        if guess in existing and not args.force:
            print('drive %d already in logs/ as %s -- skipping' % (d['id'], guess))
            continue
        print('pulling drive %d (%d records)...' % (d['id'], d['records']))
        records = fetch(g, d['id'])
        path, skipped = write_csv(root, d, records)
        print('  wrote %s (%d rows%s)'
              % (os.path.relpath(path, root), len(records),
                 ', %d unknown channels skipped' % skipped if skipped else ''))


if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Check the Python channel table against the C++ one**

The two tables must agree or every column is mislabelled. Prove it rather than eyeballing it:

```bash
cd firmware/test/host && make build/test_logbuf && cat > /tmp/dump_chans.cpp <<'EOF'
#include <cstdio>
#include "poll.h"
int main() {
    for (uint16_t i = 0; i < 64; ++i) {
        const char* n = gauge::log_chan_name(i);
        if (n) printf("%u %s\n", i, n);
    }
    for (uint16_t i = 0; i < 8; ++i) {
        const char* n = gauge::log_chan_name(gauge::kChanExtras + i);
        if (n) printf("%u %s\n", gauge::kChanExtras + i, n);
    }
}
EOF
c++ -std=c++17 -I. -I../../components/gauge_core /tmp/dump_chans.cpp \
    ../../components/gauge_core/*.cpp -o /tmp/dump_chans && /tmp/dump_chans > /tmp/cpp_chans.txt
cd ../../.. && .venv/bin/python - <<'EOF' > /tmp/py_chans.txt
import sys; sys.path.insert(0, 'tools')
from pull_drives import PID_KEYS, EXTRA_KEYS, EXTRAS_BASE
for i, k in enumerate(PID_KEYS): print(i, k)
for i, k in enumerate(EXTRA_KEYS): print(EXTRAS_BASE + i, k)
EOF
diff /tmp/cpp_chans.txt /tmp/py_chans.txt && echo "CHANNEL TABLES AGREE"
```

Expected: `CHANNEL TABLES AGREE`. If they do not, fix `PID_KEYS` to match the C++ output — the C++ is the source of truth because it is what wrote the records.

- [ ] **Step 3: Install pyserial and pull the bench drive**

Add `pyserial` to `requirements.txt`, then:

```bash
.venv/bin/pip install pyserial
.venv/bin/python tools/pull_drives.py --list
```

Expected: the board's stats and the drives recorded during Task 7's car test. Then pull them:

```bash
.venv/bin/python tools/pull_drives.py
```

Expected: `logs/drive-*.csv` files written, no crc mismatch, no unknown channels skipped.

- [ ] **Step 4: The acceptance test — replay a drive nobody watched**

This is the whole point of the feature, so it is the test that matters.

```bash
head -5 logs/drive-*.csv          # four columns, sane values
.venv/bin/python run.py --replay logs/drive-<the-new-one>.csv
```

Expected: the drive plays in the desk app with real rpm, speed, coolant — and now also `imu_ax/ay/az/gz` channels, which no previous log has.

Then close the loop the other way: compile it back into the replay library the board boots with.

```bash
.venv/bin/python tools/build_drive_asset.py build-assets/drives.bin
```

Expected: the new drive appears in the listing with its record count.

**And the question this unblocks:** with a real moving-car log in hand, check which of `imu_ax`/`imu_ay` moves under braking and which under cornering. That answers the axis question B3 has been blocked on (see the backlog note in `SPEC.md` §B3) — write the answer down in the spec rather than leaving it in a terminal.

- [ ] **Step 5: Document and commit**

Add a `§15 Drive recording` to `SPEC.md` covering: the `logs` partition and its size, what is recorded (all supported PIDs + IMU at 5 Hz), the ~9 hours retained with oldest-dropped-first, the 20 s drive boundary, the borrowed clock and what `drive-unknown-N` means, and `tools/pull_drives.py` with its shared-port caveat. Add the pull command to `README.md` beside the existing launchers.

```bash
git add tools/pull_drives.py requirements.txt SPEC.md README.md
git commit -m "tools: take the drives off the gauge

One command: it lends the board the Mac's clock, asks what it is holding,
and writes anything new into logs/ as the same four-column CSV the desk
replay already reads. Which means a drive nobody watched can be replayed,
and can be compiled straight back into the library it will be played from.

The channel table is duplicated here, in Python, because the record has room
for an id and not a name. That duplication is checked rather than trusted:
the board reports a table version and this refuses to guess if it differs."
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §1 Storage — partition, why not a filesystem | 7 (partition), 1 (the seam that makes a filesystem unnecessary) |
| §2 The ring — header, records, wipe-ahead, head on boot, seq wrap, channel ids | 1-6 |
| §3 What is recorded — all PIDs, IMU 5 Hz, live only | 7 |
| §4 Drive boundaries — 20 s silence, restart, 100-record floor | 4 (floor), 7 (timer) |
| §5 Time — `TIME`, epoch_at_boot, NVS floor, `drive-unknown-N` | 7 (clock), 8 (`TIME`), 9 (naming) |
| §6 Off the display's path — own task, queue, IMU moves | 7 |
| §7 Retrieval — the five commands, the puller, CSV shape | 8, 9 |
| §8 Files | all |
| §9 Testing — every listed case, then the car | 1-5 (host cases), 7 (car), 9 (acceptance) |

Two spec details deliberately implemented differently, both noted at the point of use:
- The spec says a short drive is "not kept"; the plan hides it in `list()` instead of erasing it, because a mid-ring erase would take a live sector with it. Same observable behaviour, no new failure mode.
- The spec's "engine restart caught by `run_time` resetting" needs no new code: a restart that the poll loop notices produces a silence, and one that it does not is a continuous drive, correctly recorded as one.

**Placeholders:** none. Every code step carries the code; the one `ERR not implemented` is written, then replaced, inside Task 8.

**Type consistency:** `LogBuf::current_drive/next_drive_id/drive_count/record_count/mount/begin_drive/append/flush/end_drive/read_drive/list/has_drive/erase_all/force_seq_for_test` and `summarise/records_in/open_sector/advance_sector` private. `RecordSink` is `bool(*)(const Record*, size_t, void*)` in Tasks 3, 4, 8. `log_chan_id`/`log_chan_name` in Tasks 6, 7, 9. `drive_log_*` C API in Tasks 7, 8. `Record` is 12 bytes everywhere and matches `REC` in both Python tools.

**Three bugs this review found in the plan's own test cases, fixed above — worth knowing about, because an executor who "fixes the code until the test passes" would implement the bug:**

1. `test_wrap_drops_the_oldest` used four drives in a four-sector ring. Since `begin_drive` always advances the head, four one-sector drives fill four sectors exactly and nothing is lost — the test would have passed while proving the opposite of its name. It takes five.
2. `test_half_erased_sector_is_not_data` half-erased sector 3 after writing only sector 1, so it was corrupting blank flash. It now writes three drives first.
3. `read_drive` searched the whole ring once per sector emitted. Correct, but on a full 2,544-sector partition that is millions of 16-byte flash reads, and `list()` calls it once per drive — `LIST` would have taken minutes. A drive's sectors are consecutive ring indices, so one pass to find the start plus a walk is enough.

And one thing an executor must not miss: **`read_drive` keeps no state between calls.** `summarise` calls it repeatedly for the same drive and must get the same answer each time.
