// Replay a recorded drive from the binary library built by
// tools/build_drive_asset.py.
//
// Pure: it is handed a byte buffer and a logical time, and knows nothing about
// where the bytes came from. On the board they are read out of a flash
// partition; on the Mac the same code reads a file. That is what lets the host
// tests drive the real firmware path rather than a parallel implementation.
//
// Time is logical, not wall-clock -- the same distinction the metrics make. A
// drive replayed at 8x must still feed the metrics real-world seconds, or a
// sped-up replay reads as violent acceleration (SPEC.md section 4).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace gauge {

struct ReplaySample {
    uint32_t t_ms;
    uint16_t chan;
    float    value;
};

class Replay {
  public:
    // Validates the header. False if the buffer is not a drive library.
    bool open(const uint8_t* data, size_t len);
    bool is_open() const { return data_ != nullptr; }

    int drive_count() const { return drives_; }
    int channel_count() const { return channels_; }
    int total_records() const { return static_cast<int>(records_); }

    // Pick a drive and rewind to its start. False if out of range.
    bool select(int index);
    int  selected() const { return sel_; }

    std::string drive_name(int index) const;
    std::string channel_name(uint16_t id) const;

    // Duration of the selected drive, in seconds.
    double duration_s() const;

    void rewind() { cursor_ = 0; }
    bool finished() const { return cursor_ >= sel_count_; }

    // Consume the next sample if it is due at logical time `t_s` (seconds
    // since the start of this drive). Call in a loop until it returns false.
    bool next(double t_s, ReplaySample* out);

  private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    int channels_ = 0;
    int drives_ = 0;
    uint32_t records_ = 0;

    int sel_ = -1;
    uint32_t sel_first_ = 0;
    uint32_t sel_count_ = 0;
    uint32_t sel_dur_ms_ = 0;
    uint32_t cursor_ = 0;
};

}  // namespace gauge
