#include "crc32.h"

namespace gauge {

uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    // Inverted in, inverted out, reflected polynomial 0xEDB88320 -- see the
    // header for why this is byte-identical to esp_rom_crc32_le() and to
    // Python's binascii.crc32().
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

}  // namespace gauge
