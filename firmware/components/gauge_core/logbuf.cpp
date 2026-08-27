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
    drive_starts_ = 0;
    record_count_ = 0;
    sectors_used_ = 0;
    bool any = false;
    uint32_t best_seq = 0;
    size_t   best = 0;
    uint32_t max_drive = 0;

    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return false;
        if (!valid(h)) continue;
        ++sectors_used_;
        if (!any || newer(h.seq, best_seq)) { any = true; best_seq = h.seq; best = i; }
        if (h.flags & 1) ++drive_starts_;
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
    h.table_version = kChanTableVersion;
    if (!flash_.write(head_ * kSectorSize, &h, sizeof h)) return false;
    used_ = 0;
    return true;
}

bool LogBuf::advance_sector(uint32_t drive, bool opens_drive) {
    head_ = (head_ + 1) % flash_.sector_count();
    // One 16-byte read before the erase, purely so sectors_used() can be a
    // fact rather than an estimate: a sector that already carried a header is
    // being recycled, not newly occupied.
    SectorHeader old{};
    const bool was_used = flash_.read(head_ * kSectorSize, &old, sizeof old) && valid(old);
    // Wipe-ahead: this sector holds the oldest data in the ring, so erasing
    // it here IS "drop the oldest" -- no free-space accounting anywhere.
    if (!flash_.erase_sector(head_)) return false;
    if (!open_sector(drive, opens_drive)) return false;
    if (!was_used && sectors_used_ < flash_.sector_count()) ++sectors_used_;
    return true;
}

bool LogBuf::begin_drive(uint32_t epoch_s) {
    if (!mounted_) return false;
    if (drive_) end_drive();
    drive_ = next_drive_++;
    if (!advance_sector(drive_, /*opens_drive=*/true)) return false;
    ++drive_starts_;
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

bool LogBuf::read_drive(uint32_t id, RecordSink sink, void* ctx,
                        uint16_t* version_out) {
    if (!mounted_ || !id) return false;
    const size_t count = flash_.sector_count();

    // A drive's sectors are CONSECUTIVE ring indices, because the head only
    // ever advances by one -- so this needs one pass to find where the drive
    // starts and then a walk, not a search per sector. The earlier draft of
    // this searched the whole ring for each sector in turn, which on a full
    // 2544-sector partition is millions of flash reads and turns LIST into
    // a multi-minute command.
    //
    // The first surviving sector is the one with the OLDEST seq, which is
    // exactly the drive's first surviving sector because seq only ever
    // increases as the head advances. This used to look for the sector whose
    // ring predecessor belongs to a different drive, which is the same answer
    // whenever such a gap exists -- but a drive long enough to own EVERY
    // sector of the ring has no gap anywhere, so that search found nothing,
    // read_drive returned false, and list() silently dropped a drive holding
    // the entire 10 MB partition. Oldest-seq needs no gap, and it is still
    // correct when the drive's real opening sector has been overwritten (the
    // overwriting sector carries a much newer seq).
    size_t start = count;
    SectorHeader h{}, start_h{};
    for (size_t i = 0; i < count; ++i) {
        if (!sector_of(i, id, &h)) continue;
        if (start == count || newer(start_h.seq, h.seq)) { start = i; start_h = h; }
    }
    if (start == count) return false;          // no such drive

    // The channel-table version this drive was written under. Records store
    // channel *ids*, so labelling them with a different table's names is
    // silent corruption of meaning -- refuse rather than guess (design s2).
    if (version_out) *version_out = start_h.table_version;
    else if (start_h.table_version != kChanTableVersion) return false;

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

size_t LogBuf::list(DriveInfo* out, size_t max, bool* truncated, uint32_t before_id) {
    if (truncated) *truncated = false;
    if (!mounted_ || !max) return 0;
    // Paging: everything strictly older than where `before_id` starts. A
    // drive's sectors are contiguous in seq, so one seq cut-off separates
    // "older than that drive" from that drive and everything after it.
    uint32_t before_seq = 0;
    if (before_id && !drive_first_seq(before_id, &before_seq)) return 0;
    // Gather per-drive facts in one pass over the headers, then a second pass
    // for the drives that qualify. The board has 8 MB of PSRAM but this runs
    // in a task with an 8 KB stack, so nothing here is per-record.
    struct Acc { uint32_t id, first_seq, last_seq, sectors; bool opens; };
    // 2544 sectors could in principle be 2544 drives; in practice a drive is
    // never one sector. Cap the table and report the newest that fit.
    constexpr size_t kMaxDrives = kListCapacity;
    Acc acc[kMaxDrives]{};
    size_t n_acc = 0;

    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return 0;
        if (!valid(h) || !h.drive) continue;
        if (before_id && !newer(before_seq, h.seq)) continue;   // this page starts below it
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
                if (truncated) *truncated = true;   // a drive fell off the table
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
        if (pick == n_acc) break;
        DriveInfo info{};
        const bool offerable =
            summarise(acc[pick].id, &info) && info.records >= kMinDriveRecords;
        acc[pick].id = 0;
        if (!offerable) continue;              // too short to offer: not hidden, absent
        if (written == max) {
            // A drive that WOULD have been offered and there is no room for
            // it. Saying "N drives" here would read as a complete answer.
            if (truncated) *truncated = true;
            break;
        }
        out[written++] = info;
    }
    return written;
}

bool LogBuf::erase_all() {
    if (!mounted_) return false;
    for (size_t i = 0; i < flash_.sector_count(); ++i)
        if (!flash_.erase_sector(i)) return false;
    return mount();
}

bool LogBuf::has_drive(uint32_t id, DriveInfo* out) {
    DriveInfo info{};
    if (!summarise(id, &info) || info.records < kMinDriveRecords) return false;
    if (out) *out = info;
    return true;
}

bool LogBuf::drive_first_seq(uint32_t id, uint32_t* out) {
    if (!mounted_ || !id) return false;
    bool any = false;
    uint32_t first = 0;
    for (size_t i = 0; i < flash_.sector_count(); ++i) {
        SectorHeader h{};
        if (!flash_.read(i * kSectorSize, &h, sizeof h)) return false;
        if (!valid(h) || h.drive != id) continue;
        if (!any || newer(first, h.seq)) { first = h.seq; any = true; }
    }
    if (any && out) *out = first;
    return any;
}

bool LogBuf::summarise(uint32_t id, DriveInfo* out) {
    struct Ctx { DriveInfo* d; bool first; } ctx{out, true};
    *out = DriveInfo{};
    out->id = id;
    out->table_version = kChanTableVersion;
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
    // summarise() must describe a drive it cannot label, so it asks for the
    // version rather than being refused because of it -- LIST reports the
    // mismatch and GET is what refuses.
    return read_drive(id, sink, &ctx, &out->table_version);
}

}  // namespace gauge
