// Raw accelerometer readings -> lateral and longitudinal g.
// Ported from mx5gauge/gforce.py -- pure, no I/O, host-testable. The two are
// cross-checked by tools/verify_port.sh, so a threshold changed here must be
// changed there in the same commit.
//
// The board's QMI8658 reports acceleration on three axes fixed to the *gauge*,
// not to the car. Nothing in the firmware knows which way the gauge points: it
// is stuck to a dashboard by hand, at whatever angle looked right that day. So
// this unit works the mounting out from the data instead of being told.
//
// **Which way is down.** Averaged over long enough, the only acceleration a
// car sustains is gravity -- everything else is a corner or a stop, and those
// cancel. The slow average of the three axes is the down vector, and its
// length is a free check on the install: it must come out at 1.000 g.
//
// **Which way is forward.** Gravity gives vertical, which leaves a flat plane
// holding forward and sideways with no way to tell them apart. The car breaks
// the tie: road speed only changes when it accelerates or brakes, so the
// horizontal direction that tracks the speed change is forward.
//
// Until forward is found, `ready()` is false and the caller must show nothing.
// Same honesty rule as gauge::view_available. A guessed axis draws a
// convincing cornering figure that is really a bump.
#pragma once
#include <optional>

namespace gauge {

// Seconds of history in the gravity average. Long enough that a 30-second
// motorway curve cannot lean the vertical over, short enough that the gauge
// being knocked into a new angle is forgiven within a minute.
inline constexpr double kGravityTauS = 30.0;

// What demo mode uses instead. Held in the hand rather than bolted to a car,
// the 30 s figure above is wrong in the one way that matters: no chip can tell
// a tilt from a shove -- both press on it identically -- so tipping the gauge
// six degrees reads as 0.1 g and hangs the dot out there for the best part of
// a minute while the slow average catches up. On a dashboard that is right,
// because a car body does not tilt. In a hand it makes the meter look wild.
// At 1 s the dot springs out when the gauge is moved and settles back by
// itself, which is what somebody holding it expects to see. Demo only; the
// car is never given this. (Tommy, 2026-09-05: "the gauge on my hand".)
inline constexpr double kDemoGravityTauS = 1.0;

// How much evidence forward needs before it is believed, as the summed square
// of the observed longitudinal acceleration in g. Tuned on the 2026-08-29
// mounted drive by locking at a range of values and measuring how far the axis
// then moved for the rest of the drive: 0.05 locks at 5.3 min and drifts 17
// deg, 0.20 locks at 6.5 min and drifts 5.8 deg, 0.60 locks at 7.1 min and
// drifts 0.7 deg. The final direction is the same to two decimals at every
// threshold -- the axis is a property of the bracket, not of the tuning.
inline constexpr double kForwardConfidence = 0.20;

// Speed changes are usable as a forward reference only across these gaps. The
// lower bound is not about the channel refreshing -- it refreshes at 3 Hz. It
// is quantisation: OBD speed arrives in whole km/h, so over 0.33 s one count of
// rounding is 0.086 g of imaginary acceleration, which swamps what we are
// trying to measure. Over 1 s the same count is worth a third of that.
inline constexpr double kMinSpeedDt = 0.9;
inline constexpr double kMaxSpeedDt = 3.0;
inline constexpr double kMinLearnG  = 0.04;

// Horizontal g past which a sample is not believed as a peak. A road car on
// road tyres does not make 1.5 g; a gauge knocked with an elbow makes far more
// in one sample. The live dot still shows it -- suppressing the reading would
// lie about what the part reported -- but the drive's high-water marks are
// quoted on the summary card afterwards, and one knock must not pin them
// there for the rest of the drive.
inline constexpr double kPeakSaneG = 1.5;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

// What survives a key cycle: the mounting angle, and how much evidence backs
// it. A bracket does not move between drives, so the minutes of learning are
// paid once. Learning continues regardless, so a mount that really was moved
// corrects itself instead of trusting a stale answer for ever.
struct MountAxes {
    Vec3   fwd;
    Vec3   down;
    double weight = 0.0;
};

class GForce {
  public:
    // Feed one accelerometer sample, in g, on the drive's own clock.
    void update(double t, std::optional<double> ax, std::optional<double> ay,
                std::optional<double> az);
    // Feed one road-speed reading, in km/h. Used only to learn which way
    // forward is; once ready() cornering keeps working with the car stopped,
    // which a speed-derived score never could.
    void speed(double t, std::optional<double> speed_kph);

    // Seconds in the gravity average. kGravityTauS unless told otherwise, and
    // only demo mode tells it otherwise. Ignored if not positive.
    void   set_gravity_tau(double s);

    bool   ready() const { return have_fwd_; }
    double total() const;                 // combined horizontal g

    std::optional<MountAxes> export_axes() const;
    void                     restore_axes(const MountAxes& saved);

    // Positive under braking, positive in a right-hand turn. Both in g, and
    // chosen so the numbers read the way a driver's body feels them.
    double lon = 0.0;
    double lat = 0.0;
    double peak_lat       = 0.0;
    double peak_lon_brake = 0.0;
    double peak_lon_accel = 0.0;
    // The install check. 1.000 means the gauge never moved on its mount.
    std::optional<double> mount_g;
    // 0 until forward is found, 1 once settled. Drawn as the 'learning' state
    // rather than hidden: a blank circle with no explanation reads as a
    // broken gauge.
    double confidence() const;

    Vec3 down, fwd, right;

  private:
    void solve();
    void reset_window(double speed_kph, double t);

    double grav_tau_ = kGravityTauS;
    bool   have_grav_ = false, have_fwd_ = false;
    Vec3   grav_{};                     // the running average, un-normalised
    Vec3   res_{};                      // last horizontal residual
    Vec3   win_{};                      // residual integrated over the window
    double win_s_ = 0.0;
    Vec3   learn_v_{};
    double learn_w_ = 0.0;
    std::optional<double> last_t_, spd_, spd_t_;
};

}  // namespace gauge
