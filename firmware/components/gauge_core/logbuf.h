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
//
// kChanDriveStart's record carries the drive's epoch seconds (0 if the clock
// was unknown when it opened) -- but as the raw bits of a uint32_t stuffed
// into Record::value, not as that uint32_t converted to a float. A 12-byte
// record has no spare field to hold it properly, and widening Record would
// break the format tools/build_drive_asset.py and the desk replay app both
// already read. float only has 24 bits of mantissa, so converting a real
// epoch (~1.75e9, ~31 bits) instead of copying it rounds to the nearest 128
// and is silently wrong -- there is no exception thrown, just a timestamp
// that's off by up to two minutes. A reader must memcpy the 4 bytes of
// Record::value into a uint32_t, the same way begin_drive() in logbuf.cpp
// writes them; treating this field as a float to read is a bug, not a
// simplification.
constexpr uint16_t kChanDriveStart = 0xFFFF;
// kChanDriveEnd just closes the drive -- its record's value is left zeroed
// and carries no meaning.
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
    // The channel-table version stamped into this drive's sector headers.
    // A drive written by a different firmware's table cannot be labelled by
    // this one's names -- see SectorHeader::table_version.
    uint16_t table_version;
};

// A drive shorter than this is a key touched, not a drive, and list() does
// not offer it.
constexpr uint32_t kMinDriveRecords = 100;

// How many drives list() can hold in its accumulator, and therefore the most
// it can ever report in one call. Callers must size their DriveInfo array to
// this -- passing a smaller `max` silently hides the drives that do not fit,
// which is exactly the bug the `truncated` out-parameter exists to surface.
constexpr size_t kListCapacity = 64;

// 16 bytes, on flash. Moved here (out of logbuf.cpp's anonymous namespace)
// so LogBuf::sector_of() can name it in its declaration.
struct SectorHeader {
    char     magic[4];
    uint32_t seq;
    uint32_t drive;
    uint16_t flags;
    // Design s2: "the header carries a table version so a mismatch is an
    // error rather than silently mislabelled data." A record stores a channel
    // *id*, and ids are positions in poll.cpp's compiled-in table -- so a
    // firmware whose table gained an entry in the middle renames every
    // channel of every drive already on flash. This is the field that makes
    // that detectable: it is written from kChanTableVersion and compared
    // against it on the way out. It occupies the 2 bytes the design called
    // `pad`, so the 16-byte header is unchanged.
    uint16_t table_version;
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

    // Sector headers flagged as opening a drive: the ones found at mount,
    // plus one per begin_drive() since. It is an UPPER BOUND on what can be
    // pulled, not an answer -- it is never decremented when the ring drops a
    // drive, and it counts drives too short for list() to offer. list() is
    // the only authority on what is held and offerable.
    size_t drive_starts() const { return drive_starts_; }
    size_t record_count() const { return record_count_; }
    // Sectors carrying a valid header. Rises to sector_count() and stops --
    // once the ring has been round once, every sector is in use.
    size_t sectors_used() const { return sectors_used_; }

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
    // `version_out`, when given, receives the channel-table version stamped
    // in this drive's sector headers and the read goes ahead whatever it
    // says -- that is for list(), which must still be able to describe a
    // drive it cannot label. When it is null, a drive stamped with any
    // version other than kChanTableVersion is REFUSED (returns false)
    // rather than streamed out to be mislabelled by the reader's table.
    using RecordSink = bool (*)(const Record* records, size_t count, void* ctx);
    bool read_drive(uint32_t id, RecordSink sink, void* ctx,
                    uint16_t* version_out = nullptr);

    // Drives held, newest first, skipping ones below kMinDriveRecords.
    // Returns how many were written to `out`. `truncated`, when given, is set
    // true if there were drives this call could not report -- either `max`
    // was reached or the internal table overflowed. "N drives" and "N drives
    // and more I cannot show you" must never look the same to a caller.
    size_t list(DriveInfo* out, size_t max, bool* truncated = nullptr);
    bool has_drive(uint32_t id);

    // Wipes every sector and starts over. The only way back from a ring the
    // channel table has outgrown.
    bool erase_all();

    // Test-only: starts the sequence numbers near the u32 rollover so the
    // wrap can be exercised without writing four billion sectors.
    void force_seq_for_test(uint32_t seq) { seq_ = seq; }

private:
    static constexpr size_t kBatch = 32;      // 384 B of RAM, ~1 s of records

    // Number of records already committed in `sector`. Reads the sector body
    // in 10 chunks of 34 records (408 bytes each -- 10 x 34 = 340, the whole
    // sector) and finds the first erased slot in RAM. This used to be a
    // binary search trusting "records are contiguous from the start of a
    // sector, everything after is erased", but a power cut mid-erase can
    // break that invariant with a hole in the middle of a sector rather
    // than just its tail, and a bisection can walk straight past a hole
    // without ever reading it. The chunked scan is exact -- every record in
    // the sector is actually read -- while still costing 10 flash
    // transactions per sector rather than up to 340, which matters because
    // mount() does this for every sector -- 2,544 of them on the real
    // partition.
    size_t records_in(size_t sector);

    bool open_sector(uint32_t drive, bool opens_drive);
    bool advance_sector(uint32_t drive, bool opens_drive);

    // Is this sector part of drive `id`? Fills *out with its header if so.
    bool sector_of(size_t index, uint32_t id, SectorHeader* out);

    // Scans one drive's records via read_drive() and reduces them to a
    // DriveInfo. The only place that counts records and reads markers.
    bool summarise(uint32_t id, DriveInfo* out);

    IFlash& flash_;
    size_t  drive_starts_ = 0;
    size_t  record_count_ = 0;
    size_t  sectors_used_ = 0;

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
