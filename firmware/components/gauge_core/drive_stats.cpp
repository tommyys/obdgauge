#include "drive_stats.h"

#include "poll.h"

namespace gauge {
namespace {

// Resolved once: log_chan_id() walks the PID table by name, and this is on
// the path that folds tens of thousands of records.
uint16_t rpm_chan()   { static const uint16_t c = log_chan_id("rpm");   return c; }
uint16_t speed_chan() { static const uint16_t c = log_chan_id("speed"); return c; }

}  // namespace

void DriveStatsFold::add(const Record* records, size_t count) {
    const uint16_t rpm_id   = rpm_chan();
    const uint16_t speed_id = speed_chan();

    for (size_t i = 0; i < count; ++i) {
        const Record& r = records[i];
        // A marker is not a reading. kChanDriveStart's value field carries the
        // drive's epoch as punned uint32 bits (logbuf.h), so folding it as a
        // number would drop a ~1.7e9 outlier into whatever it was folded with.
        if (r.chan == kChanDriveStart || r.chan == kChanDriveEnd) continue;

        if (!seen_) { seen_ = true; first_t_ = r.t_ms; }
        last_t_ = r.t_ms;
        ++out_.records;

        if (r.chan == rpm_id) {
            if (r.value > out_.peak_rpm) out_.peak_rpm = r.value;
        } else if (r.chan == speed_id) {
            if (r.value > out_.peak_kph) out_.peak_kph = r.value;
            // Only speed records advance the trip: Trip::update integrates
            // between consecutive calls, so feeding it once per unrelated
            // channel would integrate the same speed many times over.
            trip_.update(r.t_ms / 1000.0, r.value, std::nullopt);
        }
    }

    out_.distance_km = trip_.dist_km;
    out_.duration_ms = seen_ ? last_t_ - first_t_ : 0;
}

}  // namespace gauge
