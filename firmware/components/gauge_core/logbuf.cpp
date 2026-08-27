#include "logbuf.h"
#include <cstring>

namespace gauge {
namespace {

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

// Committed records run contiguously from the start of the sector -- but a
// power cut mid-erase can break that invariant while leaving the sector's
// magic intact: a chunk in the MIDDLE of the sector can turn back to 0xFF
// while real records survive on both sides of it, not just the sector's
// tail. A pure bisection only ever visits O(log n) of the 340 slots and can
// walk straight past a hole like that without ever reading it -- an earlier
// version of this function did exactly that (see
// test_records_in_across_a_mid_sector_erase_hole in test_logbuf.cpp: it
// answered 301 for a sector whose slots 100-149 were erased, and a follow-up
// stride-8 sampling patch still missed holes narrower than the stride,
// which is a real failure mode inside an otherwise-long, still-offered
// drive, not just noise below kMinDriveRecords).
//
// So this reads the whole sector body up front, exactly, and finds the
// boundary in RAM: 10 reads of 408 bytes (34 records each; 10 x 34 = 340,
// the whole sector) rather than one flash transaction per record. That is
// fewer flash transactions than the old bisection's ~9 single-record reads
// plus 340/8 (~42) grid reads, and it is exact -- there is no hole of any
// width it can miss, because every record in the sector is actually read.
size_t LogBuf::records_in(size_t sector) {
    constexpr size_t kChunkRecords = 34;                          // 340 / 10
    constexpr size_t kChunks       = kRecordsPerSector / kChunkRecords;
    static_assert(kChunks * kChunkRecords == kRecordsPerSector,
                  "kChunkRecords must divide kRecordsPerSector exactly");

    Record chunk[kChunkRecords];
    for (size_t c = 0; c < kChunks; ++c) {
        const size_t base = c * kChunkRecords;
        const size_t off = sector * kSectorSize + kSectorHeaderSize + base * sizeof(Record);
        if (!flash_.read(off, chunk, sizeof chunk)) return base;
        for (size_t i = 0; i < kChunkRecords; ++i) {
            if (erased(&chunk[i], sizeof chunk[i])) return base + i;
        }
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
    ++drive_count_;
    Record m{};
    m.t_ms = 0;
    m.chan = kChanDriveStart;
    // epoch_s is a plain 32-bit count, not a quantity that means anything as
    // a float -- casting it (static_cast<float>(epoch_s)) only has 24 bits of
    // mantissa, so any real Unix timestamp (~1.75e9, ~31 bits) rounds to the
    // nearest 128 and never reads back exact. The value field is borrowed
    // storage here: copy the bits across instead of converting them.
    memcpy(&m.value, &epoch_s, sizeof(m.value));
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

// Is this sector part of drive `id`?
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

bool LogBuf::erase_all() {
    if (!mounted_) return false;
    for (size_t i = 0; i < flash_.sector_count(); ++i)
        if (!flash_.erase_sector(i)) return false;
    return mount();
}

bool LogBuf::has_drive(uint32_t id) {
    DriveInfo info{};
    return summarise(id, &info) && info.records >= kMinDriveRecords;
}

bool LogBuf::summarise(uint32_t id, DriveInfo* out) {
    struct Ctx { DriveInfo* d; bool first; } ctx{out, true};
    *out = DriveInfo{};
    out->id = id;
    auto sink = [](const Record* r, size_t n, void* c) -> bool {
        Ctx* x = static_cast<Ctx*>(c);
        for (size_t i = 0; i < n; ++i) {
            if (r[i].chan == kChanDriveStart) {
                // See begin_drive(): value holds epoch_s's raw bits, not a
                // float conversion of it.
                if (x->first) {
                    memcpy(&x->d->epoch_s, &r[i].value, sizeof(x->d->epoch_s));
                    x->first = false;
                }
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

}  // namespace gauge
