// CRC-32 as every other tool on the far end of the wire means it.
//
// `GET` streams a drive off the board as base64 and ends with a checksum, and
// tools/pull_drives.py checks that checksum with Python's binascii.crc32.
// That the two agree used to be asserted nowhere: the firmware called
// esp_rom_crc32_le(), the Python test generated both sides of its own
// fixture, and if the conventions had disagreed EVERY pull of EVERY drive
// would have failed identically -- with the runbook telling the reader to go
// and check their USB cable.
//
// They do in fact agree. From ESP-IDF v5.5.2
// (components/esp_rom/patches/esp_rom_crc.c:220):
//
//     uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
//     { crc = ~crc; for (...) crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
//       return ~crc; }
//
// -- the standard reflected table, inverted in and inverted out, which is
// bit-for-bit what zlib's crc32() does with the same seed. So
// esp_rom_crc32_le(0, p, n) == binascii.crc32(p): seed 0, no `~` wrapper on
// either side. (The `~` wrappers the esp_rom_crc.h header comment describes
// are for building OTHER CRC-32 parameterisations out of it, not this one.)
//
// This function is that same algorithm, spelled out here rather than called
// from ROM for one reason: firmware/test/host links only gauge_core, so a
// ROM call is a thing no host test can reach. Living here, the exact code the
// board runs is pinned by test_crc32.cpp against the standard check value --
// CRC-32("123456789") == 0xCBF43926 -- and tests/test_pull_drives.py pins the
// Python side against the same literal. Neither test is generated from the
// other; they meet at a constant published outside this project.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gauge {

// Seed 0 for the first call; feed the previous return value back in to
// continue over a split buffer -- crc32(crc32(0, a, na), b, nb) equals the
// CRC of a and b concatenated, which is what streaming a drive sector by
// sector relies on.
uint32_t crc32(uint32_t crc, const void* data, size_t len);

}  // namespace gauge
