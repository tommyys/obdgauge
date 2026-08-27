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

// Records run contiguously from the start of the sector, so the boundary
// between committed slots and still-erased ones can be found by bisection
// instead of a linear scan: ~log2(kRecordsPerSector) reads (about 9) rather
// than up to 340, which matters because mount() calls this for every
// sector -- 2,544 of them on the real partition.
size_t LogBuf::records_in(size_t sector) {
    size_t lo = 0, hi = kRecordsPerSector;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        Record x{};
        if (!flash_.read(sector * kSectorSize + kSectorHeaderSize + mid * sizeof x,
                          &x, sizeof x)) return lo;
        if (erased(&x, sizeof x)) hi = mid; else lo = mid + 1;
    }
    return lo;
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
    ++drive_count_;
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

}  // namespace gauge
