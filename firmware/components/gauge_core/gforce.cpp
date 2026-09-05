#include "gforce.h"

#include <algorithm>
#include <cmath>

namespace gauge {
namespace {

double norm(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
Vec3 scale(const Vec3& v, double k) { return {v.x * k, v.y * k, v.z * k}; }
Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

}  // namespace

double GForce::total() const { return std::sqrt(lat * lat + lon * lon); }

double GForce::confidence() const {
    return std::min(1.0, learn_w_ / kForwardConfidence);
}

void GForce::set_gravity_tau(double s) {
    if (s > 0.0) grav_tau_ = s;
}

void GForce::reset_window(double speed_kph, double t) {
    spd_   = speed_kph;
    spd_t_ = t;
    win_   = Vec3{};
    win_s_ = 0.0;
}

void GForce::speed(double t, std::optional<double> speed_kph) {
    if (!speed_kph) return;
    if (!spd_) {
        spd_ = *speed_kph;
        spd_t_ = t;
        return;
    }
    double dt = t - *spd_t_;
    if (dt < kMinSpeedDt) return;              // keep filling the window
    if (dt > kMaxSpeedDt || !have_grav_ || win_s_ <= 0) {
        reset_window(*speed_kph, t);
        return;
    }
    // m/s^2 -> g. Positive means the car sped up.
    double a = (*speed_kph - *spd_) / 3.6 / dt / 9.81;
    Vec3   r = scale(win_, 1.0 / win_s_);
    reset_window(*speed_kph, t);
    if (std::fabs(a) < kMinLearnG) return;
    // Accumulate residual*a. Windows where the car was working hardest
    // dominate, which is what we want: they carry the direction.
    learn_v_.x += r.x * a;
    learn_v_.y += r.y * a;
    learn_v_.z += r.z * a;
    learn_w_ += a * a;
    solve();
}

void GForce::update(double t, std::optional<double> ax, std::optional<double> ay,
                    std::optional<double> az) {
    if (!ax || !ay || !az) return;
    Vec3 a{*ax, *ay, *az};
    if (!have_grav_) {
        grav_      = a;
        have_grav_ = true;
        last_t_    = t;
        return;
    }
    double dt = t - *last_t_;
    last_t_   = t;
    if (dt <= 0 || dt > 5.0) return;
    // Exponential average towards the true vertical.
    double k = 1.0 - std::exp(-dt / grav_tau_);
    grav_.x += (a.x - grav_.x) * k;
    grav_.y += (a.y - grav_.y) * k;
    grav_.z += (a.z - grav_.z) * k;
    double g = norm(grav_);
    if (g < 0.5) return;                // nonsense; the part is not reporting
    mount_g = g;
    down    = scale(grav_, 1.0 / g);
    // Take gravity out. What is left is flat against the road.
    res_ = sub(a, scale(down, dot(a, down)));
    win_.x += res_.x * dt;
    win_.y += res_.y * dt;
    win_.z += res_.z * dt;
    win_s_ += dt;
    if (!have_fwd_) return;
    // Braking is a backwards push, so negate to make braking positive.
    lon = -dot(res_, fwd);
    lat = dot(res_, right);
    if (total() > kPeakSaneG) return;
    peak_lat       = std::max(peak_lat, std::fabs(lat));
    peak_lon_brake = std::max(peak_lon_brake, lon);
    peak_lon_accel = std::max(peak_lon_accel, -lon);
}

void GForce::solve() {
    if (learn_w_ < kForwardConfidence || !have_grav_) return;
    // Flatten it: the answer must lie in the road plane, and a little vertical
    // leaks in from bumps hit while braking.
    Vec3   f = sub(learn_v_, scale(down, dot(learn_v_, down)));
    double n = norm(f);
    if (n < 1e-6) return;
    fwd = scale(f, 1.0 / n);
    // Right = down x forward. With down into the road and forward along it,
    // this comes out as the driver's right.
    Vec3 r = cross(down, fwd);
    n      = norm(r);
    if (n < 1e-6) return;
    right    = scale(r, 1.0 / n);
    have_fwd_ = true;
}

std::optional<MountAxes> GForce::export_axes() const {
    if (!have_fwd_) return std::nullopt;
    return MountAxes{fwd, down, learn_w_};
}

void GForce::restore_axes(const MountAxes& saved) {
    if (std::fabs(norm(saved.fwd) - 1.0) > 0.01) return;
    if (std::fabs(norm(saved.down) - 1.0) > 0.01) return;
    Vec3   r = cross(saved.down, saved.fwd);
    double n = norm(r);
    if (n < 1e-6) return;
    fwd   = saved.fwd;
    down  = saved.down;
    right = scale(r, 1.0 / n);
    have_fwd_ = true;
    // Seed the evidence too, so one bumpy minute cannot outvote a whole
    // drive's worth of learning that already agreed with this.
    learn_v_ = scale(fwd, saved.weight);
    learn_w_ = saved.weight;
}

}  // namespace gauge
