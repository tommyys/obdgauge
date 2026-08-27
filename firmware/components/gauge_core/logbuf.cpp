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
