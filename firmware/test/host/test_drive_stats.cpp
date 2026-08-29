// What the Drives view shows for one recorded drive
// (docs/superpowers/specs/2026-08-29-drives-view-design.md).
//
// The fold is fed a record stream and nothing else -- no flash, no panel -- so
// everything the view will ever show can be checked here.
#include "check.h"
#include "drive_stats.h"
#include "logbuf.h"
#include "metrics.h"
#include "poll.h"
#include <vector>
using gauge_test::check;
using gauge_test::near;

static const uint16_t kRpm   = gauge::log_chan_id("rpm");
static const uint16_t kSpeed = gauge::log_chan_id("speed");

static gauge::Record rec(uint32_t t_ms, uint16_t chan, float v) {
    return gauge::Record{t_ms, chan, 0, v};
}

// Fold a whole vector in one call, the way read_drive() hands over a sector.
static gauge::DriveStats fold(const std::vector<gauge::Record>& rs) {
    gauge::DriveStatsFold f;
    f.add(rs.data(), rs.size());
    return f.result();
}

int main() {
    // --- peaks -----------------------------------------------------------
    {
        auto s = fold({rec(0, kRpm, 850), rec(100, kRpm, 3532), rec(200, kRpm, 1200),
                       rec(0, kSpeed, 0), rec(100, kSpeed, 90), rec(200, kSpeed, 42)});
        check("peak rpm is the maximum", s.peak_rpm, 3532.0);
        check("peak speed is the maximum", s.peak_kph, 90.0);
    }

    // --- duration --------------------------------------------------------
    {
        auto s = fold({rec(5000, kRpm, 800), rec(65000, kRpm, 900)});
        check("duration spans first to last record", (int)s.duration_ms, 60000);
    }
    {
        auto s = fold({rec(1234, kRpm, 800)});
        check("one record is a zero-length drive", (int)s.duration_ms, 0);
        check("one record does not divide by zero", s.distance_km, 0.0);
    }
    {
        gauge::DriveStatsFold f;
        check("nothing folded yet -> empty", (int)f.result().records, 0);
    }

    // --- distance, and the promise that it matches the Trip view ---------
    // 60 km/h held for 60 s is 1 km. Asserted against gauge::Trip on the same
    // input rather than against a constant: the two must never drift apart,
    // which a hard-coded number would not catch.
    {
        std::vector<gauge::Record> rs;
        gauge::Trip trip;
        for (int i = 0; i <= 60; ++i) {
            rs.push_back(rec((uint32_t)i * 1000, kSpeed, 60.0f));
            trip.update(i, 60.0, std::nullopt);
        }
        auto s = fold(rs);
        near("60 km/h for 60 s is 1 km", s.distance_km, 1.0, 1e-9);
        near("distance agrees with gauge::Trip", s.distance_km, trip.dist_km, 1e-12);
    }

    // A gap longer than metrics.cpp's 5 s cut-off is not integrated -- an
    // adapter that stopped answering must not read as distance covered.
    {
        std::vector<gauge::Record> rs = {rec(0, kSpeed, 100), rec(600000, kSpeed, 100)};
        gauge::Trip trip;
        trip.update(0, 100.0, std::nullopt);
        trip.update(600, 100.0, std::nullopt);
        auto s = fold(rs);
        check("a 10 min gap adds no distance", s.distance_km, 0.0);
        check("...same as gauge::Trip", trip.dist_km, 0.0);
    }

    // --- a drive with no speed channel ------------------------------------
    {
        auto s = fold({rec(0, kRpm, 900), rec(1000, kRpm, 2000)});
        check("no speed channel -> no distance", s.distance_km, 0.0);
        check("no speed channel -> no peak speed", s.peak_kph, 0.0);
        check("...but rpm still peaks", s.peak_rpm, 2000.0);
    }

    // --- markers are not readings ----------------------------------------
    // kChanDriveStart's value holds punned epoch bits, not a number. Folding
    // it as a reading would put a ~1.7e9 outlier in whatever it landed on.
    {
        std::vector<gauge::Record> rs = {
            rec(0, gauge::kChanDriveStart, 0), rec(0, kRpm, 900),
            rec(1000, kRpm, 2000), rec(1000, gauge::kChanDriveEnd, 0)};
        auto s = fold(rs);
        check("markers do not count as records", (int)s.records, 2);
        check("markers do not move the peaks", s.peak_rpm, 2000.0);
        check("markers do not extend the duration", (int)s.duration_ms, 1000);
    }

    // --- fed in batches, as read_drive() actually delivers ----------------
    {
        std::vector<gauge::Record> rs;
        for (int i = 0; i < 100; ++i) rs.push_back(rec((uint32_t)i * 1000, kSpeed, 50.0f));
        gauge::DriveStatsFold one, many;
        one.add(rs.data(), rs.size());
        for (size_t i = 0; i < rs.size(); i += 7)
            many.add(rs.data() + i, std::min<size_t>(7, rs.size() - i));
        near("batching does not change the answer",
             one.result().distance_km, many.result().distance_km, 1e-12);
    }

    return gauge_test::check_report();
}
