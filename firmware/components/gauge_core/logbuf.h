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

// 16 bytes, on flash. Moved here (out of logbuf.cpp's anonymous namespace)
// so LogBuf::sector_of() can name it in its declaration.
struct SectorHeader {
    char     magic[4];
    uint32_t seq;
    uint32_t drive;
    uint16_t flags;
    uint16_t pad;
};
static_assert(sizeof(SectorHeader) == kSectorHeaderSize, "header is the file format");

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

    // Streams a drive's records in order, a sector at a time. A whole drive
    // can be 2 MB; the board hands it to the serial port in pieces rather
    // than holding it. Return false from the sink to stop early.
    using RecordSink = bool (*)(const Record* records, size_t count, void* ctx);
    bool read_drive(uint32_t id, RecordSink sink, void* ctx);

    // Drives held, newest first, skipping ones below kMinDriveRecords.
    // Returns how many were written to `out`.
    size_t list(DriveInfo* out, size_t max);
    bool has_drive(uint32_t id);

private:
    static constexpr size_t kBatch = 32;      // 384 B of RAM, ~1 s of records

    // Number of records already committed in `sector`. Binary-searches for
    // the boundary between committed slots and still-erased ones -- records
    // are contiguous from the start of a sector by construction, so ~log2
    // reads (about 9 for 340 slots) replace what would otherwise be up to
    // 340 flash reads per sector, and mount() does this for every sector.
    size_t records_in(size_t sector);

    bool open_sector(uint32_t drive, bool opens_drive);
    bool advance_sector(uint32_t drive, bool opens_drive);

    // Is this sector part of drive `id`? Fills *out with its header if so.
    bool sector_of(size_t index, uint32_t id, SectorHeader* out);

    // Scans one drive's records via read_drive() and reduces them to a
    // DriveInfo. The only place that counts records and reads markers.
    bool summarise(uint32_t id, DriveInfo* out);

    IFlash& flash_;
    size_t  drive_count_  = 0;
    size_t  record_count_ = 0;

    size_t   head_        = 0;      // sector index being written
    uint32_t seq_         = 0;      // seq of head_
    size_t   used_        = 0;      // records already committed in head_
    uint32_t drive_       = 0;      // 0 = no drive open
    uint32_t next_drive_  = 1;
    Record   batch_[kBatch]{};
    size_t   batch_n_     = 0;
    bool     mounted_     = false;
};

}  // namespace gauge
