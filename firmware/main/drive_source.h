// Hands gauge_core::Replay the bytes of the drive library.
//
// This is the only part of replay that is platform-specific: the library is
// memory-mapped straight out of a flash partition, so a 0.49 MB library costs
// no PSRAM and no copy. Everything above it -- which sample is due when -- is
// pure code in gauge_core/replay, shared with the host tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maps the `drives` partition. Returns NULL if it is missing or empty.
const uint8_t* drive_library_map(size_t* out_len);

#ifdef __cplusplus
}
#endif
