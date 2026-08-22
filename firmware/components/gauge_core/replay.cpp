#include "replay.h"
#include <cstring>

namespace gauge {
namespace {

constexpr size_t kHeader = 32;
constexpr size_t kChanEntry = 16;
constexpr size_t kDriveEntry = 32;
constexpr size_t kRecord = 12;

template <typename T>
T rd(const uint8_t* p) {
    T v{};
    std::memcpy(&v, p, sizeof v);   // Xtensa has no unaligned 32-bit load
    return v;
}

}  // namespace

bool Replay::open(const uint8_t* data, size_t len) {
    data_ = nullptr;
    if (!data || len < kHeader) return false;
    if (std::memcmp(data, "MX5D", 4) != 0) return false;
    uint16_t version = rd<uint16_t>(data + 4);
    if (version != 1) return false;
    channels_ = rd<uint16_t>(data + 6);
    drives_   = rd<uint16_t>(data + 8);
    records_  = rd<uint32_t>(data + 12);

    size_t need = kHeader + static_cast<size_t>(channels_) * kChanEntry +
                  static_cast<size_t>(drives_) * kDriveEntry +
                  static_cast<size_t>(records_) * kRecord;
    if (len < need) return false;

    data_ = data;
    len_ = len;
    sel_ = -1;
    return select(0);
}

std::string Replay::channel_name(uint16_t id) const {
    if (!data_ || id >= channels_) return "";
    const char* p = reinterpret_cast<const char*>(data_ + kHeader + id * kChanEntry);
    size_t n = 0;
    while (n < kChanEntry && p[n]) ++n;
    return std::string(p, n);
}

std::string Replay::drive_name(int index) const {
    if (!data_ || index < 0 || index >= drives_) return "";
    const uint8_t* p = data_ + kHeader + static_cast<size_t>(channels_) * kChanEntry +
                       static_cast<size_t>(index) * kDriveEntry;
    const char* s = reinterpret_cast<const char*>(p);
    size_t n = 0;
    while (n < 20 && s[n]) ++n;
    return std::string(s, n);
}

bool Replay::select(int index) {
    if (!data_ || index < 0 || index >= drives_) return false;
    const uint8_t* p = data_ + kHeader + static_cast<size_t>(channels_) * kChanEntry +
                       static_cast<size_t>(index) * kDriveEntry;
    sel_ = index;
    sel_first_  = rd<uint32_t>(p + 20);
    sel_count_  = rd<uint32_t>(p + 24);
    sel_dur_ms_ = rd<uint32_t>(p + 28);
    cursor_ = 0;
    return true;
}

double Replay::duration_s() const { return sel_dur_ms_ / 1000.0; }

bool Replay::next(double t_s, ReplaySample* out) {
    if (!data_ || sel_ < 0 || !out || cursor_ >= sel_count_) return false;
    const uint8_t* base = data_ + kHeader +
                          static_cast<size_t>(channels_) * kChanEntry +
                          static_cast<size_t>(drives_) * kDriveEntry;
    const uint8_t* rec = base + static_cast<size_t>(sel_first_ + cursor_) * kRecord;
    uint32_t t_ms = rd<uint32_t>(rec);
    if (t_ms > t_s * 1000.0) return false;      // not due yet
    out->t_ms  = t_ms;
    out->chan  = rd<uint16_t>(rec + 4);
    out->value = rd<float>(rec + 8);
    ++cursor_;
    return true;
}

}  // namespace gauge
