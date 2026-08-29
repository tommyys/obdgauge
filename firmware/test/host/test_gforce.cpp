// Ported from tests/test_gforce.py.
//
// The claim under test is the one the whole g view rests on: the gauge can
// work out which way it is pointing from the drive alone, with nobody
// measuring the bracket. Two halves -- find down from gravity, then find
// forward from the fact that road speed only changes when the car accelerates
// or brakes.
//
// All of it runs on synthetic drives, where the true answer is known. The
// Python suite additionally replays the real 2026-08-29 mounted drive; that
// is not repeated here because the C++ side has no CSV reader, and
// tools/verify_port.sh already puts every real log through both ports and
// diffs the result channel by channel.
#include "check.h"
#include "gforce.h"
#include <cmath>
#include <vector>
using gauge_test::check;
using gauge_test::near;
using O = std::optional<double>;

namespace {

gauge::Vec3 rot(gauge::Vec3 v, double yaw, double pitch) {
    double cy = std::cos(yaw), sy = std::sin(yaw);
    double x = v.x * cy - v.y * sy, y = v.x * sy + v.y * cy, z = v.z;
    double cp = std::cos(pitch), sp = std::sin(pitch);
    double y2 = y * cp - z * sp, z2 = y * sp + z * cp;
    return {x, y2, z2};
}

// The gauge is bolted on at a silly angle on purpose: a solver that only
// works for a tidy mounting is worthless, because nobody mounts anything
// tidily. These are the car's own axes as the accelerometer would report them.
const double kYaw = 0.7, kPitch = -0.4;
const gauge::Vec3 kDown  = rot({0.0, 0.0, 1.0}, kYaw, kPitch);
const gauge::Vec3 kFwd   = rot({1.0, 0.0, 0.0}, kYaw, kPitch);
const gauge::Vec3 kRight = rot({0.0, 1.0, 0.0}, kYaw, kPitch);

struct Leg {
    double secs, accel_g, lat_g;
};

// Drive the solver. Speed is integrated from the acceleration, so the OBD
// channel and the accelerometer agree the way they do in a car -- which is
// the only reason the solver can work at all.
gauge::GForce synth(const std::vector<Leg>& legs, double dt = 0.1) {
    gauge::GForce g;
    double t = 0.0, speed = 0.0;
    for (const Leg& leg : legs) {
        int n = static_cast<int>(leg.secs / dt);
        for (int i = 0; i < n; ++i) {
            t += dt;
            speed = std::max(0.0, speed + leg.accel_g * 9.81 * dt * 3.6);
            gauge::Vec3 s{kDown.x + kFwd.x * leg.accel_g + kRight.x * leg.lat_g,
                          kDown.y + kFwd.y * leg.accel_g + kRight.y * leg.lat_g,
                          kDown.z + kFwd.z * leg.accel_g + kRight.z * leg.lat_g};
            g.update(t, s.x, s.y, s.z);
            g.speed(t, std::round(speed));      // OBD speed is whole km/h
        }
    }
    return g;
}

double angle_between(const gauge::Vec3& a, const gauge::Vec3& b) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z;
    d = std::max(-1.0, std::min(1.0, d));
    return std::acos(d) * 180.0 / 3.14159265358979;
}

// Twenty seconds of cruising to settle gravity, then six accelerate/brake
// pairs -- enough evidence for forward.
std::vector<Leg> learning_legs() {
    std::vector<Leg> legs{{20, 0.0, 0.0}};
    for (int i = 0; i < 6; ++i) {
        legs.push_back({6, 0.25, 0.0});
        legs.push_back({6, -0.25, 0.0});
    }
    return legs;
}

}  // namespace

int main() {
    // Cruising alone teaches down but never forward: nothing in it
    // distinguishes the two flat directions.
    auto cruise = synth({{60, 0.0, 0.0}});
    check("cruising alone never finds forward", cruise.ready(), false);
    near("...but it does find down", O{angle_between(cruise.down, kDown)}, 0.0, 1.0);
    near("...and the mount check reads 1.000 g", cruise.mount_g, 1.0, 0.01);

    auto drive = synth(learning_legs());
    check("accelerating and braking finds forward", drive.ready(), true);
    near("forward is found to within a few degrees",
         O{angle_between(drive.fwd, kFwd)}, 0.0, 6.0);
    near("right falls out of it", O{angle_between(drive.right, kRight)}, 0.0, 6.0);

    // Sign conventions: what makes the dot move the way a driver expects.
    auto legs = learning_legs();
    auto with = [&](Leg tail) {
        auto l = legs;
        l.push_back(tail);
        return synth(l);
    };
    check("braking reads as positive longitudinal g",
          with({3, -0.4, 0.0}).lon > 0.3, true);
    check("accelerating reads as negative", with({3, 0.4, 0.0}).lon < -0.3, true);
    check("a right-hand turn reads as positive lateral g",
          with({3, 0.0, 0.5}).lat > 0.4, true);

    // Cornering must not leak into braking. A car held in a long
    // constant-radius corner is not decelerating, and a solver that says it
    // is would score every roundabout as a harsh stop.
    near("a steady corner leaks no longitudinal g",
         O{with({6, 0.0, 0.5}).lon}, 0.0, 0.08);

    // A knock is not a corner. The live reading still shows it -- refusing to
    // would lie about what the part reported -- but it must not pin the
    // drive's high-water mark for the rest of the drive.
    auto knock = with({0.3, 0.0, 4.0});
    check("a knock does not become the drive's peak",
          knock.peak_lat < gauge::kPeakSaneG, true);

    // --- persistence ------------------------------------------------------
    auto saved = drive.export_axes();
    check("a solved gauge has axes to save", saved.has_value(), true);
    gauge::GForce fresh;
    fresh.restore_axes(*saved);
    check("a restored gauge is ready immediately", fresh.ready(), true);
    near("and points the same way", O{angle_between(fresh.fwd, drive.fwd)}, 0.0, 0.01);
    gauge::GForce junk;
    junk.restore_axes({{1.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, 1.0});
    check("a corrupt saved axis is refused, not trusted", junk.ready(), false);
    gauge::GForce unsolved;
    check("nothing to save before it solves", unsolved.export_axes().has_value(), false);

    return gauge_test::check_report();
}
