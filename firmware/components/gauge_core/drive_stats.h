// What one recorded drive came to: the four numbers the Drives view shows.
// Design: docs/superpowers/specs/2026-08-29-drives-view-design.md.
//
// Pure, like the rest of gauge_core: it is fed records and nothing else. On
// the board those records come off flash through LogBuf::read_drive(); in the
// host tests they are written by hand. There is no third implementation.
//
// Nothing here is stored in the ring. A drive's summary is folded when the
// view asks for it, which is what lets this work on drives that were already
// recorded -- including the first real one, 2026-08-29 -- with no change to
// the 12-byte record format that tools/pull_drives.py and the desk replay
// both read.
#pragma once
#include <cstddef>
#include <cstdint>

#include "logbuf.h"
#include "metrics.h"

namespace gauge {

struct DriveStats {
    double   distance_km = 0.0;
    double   peak_rpm    = 0.0;
    double   peak_kph    = 0.0;
    uint32_t duration_ms = 0;    // from the records' own clock, not the wall clock
    uint32_t records     = 0;    // readings folded; drive markers are not readings
};

// Folds a drive's record stream, in whatever batches it arrives in.
class DriveStatsFold {
  public:
    void add(const Record* records, size_t count);
    DriveStats result() const { return out_; }

  private:
    DriveStats out_;
    // Distance is not computed here. It is delegated to the same gauge::Trip
    // the Trip view runs on, so the two can never quote different distances
    // for the same drive -- including its 5-second gap rule, which is what
    // stops an adapter that stopped answering reading as kilometres covered.
    Trip     trip_;
    bool     seen_    = false;
    uint32_t first_t_ = 0;
    uint32_t last_t_  = 0;
};

}  // namespace gauge
