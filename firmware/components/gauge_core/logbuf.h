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
