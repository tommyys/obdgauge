"""Raw accelerometer readings -> lateral and longitudinal g.

The board's QMI8658 reports acceleration on three axes fixed to the *gauge*,
not to the car. Nothing in the firmware knows which way the gauge is pointing:
it is stuck to a dashboard by hand, at whatever angle looked right that day.
So this unit works the mounting out from the data instead of being told.

Two things have to be found:

**Which way is down.** Averaged over long enough, the only acceleration a car
sustains is gravity -- everything else is a corner or a stop, and those cancel.
The slow average of the three axes is therefore the down vector, and its length
is a free check on the install: it must come out at 1.000 g. Measured on the
two 2026-08-29 drives it read 0.890 g for the loose mount and 1.026 g for the
mounted one, which is how we know the second drive is the trustworthy one.

**Which way is forward.** Gravity gives the vertical axis, which leaves a flat
plane containing forward and sideways, but no way to tell them apart -- the two
look identical to an accelerometer. The car itself breaks the tie: it reports
road speed over OBD, and speed only changes when the car accelerates or brakes.
So the horizontal direction that tracks the speed change *is* forward. Proven
on `logs/drive-20260829-211900.csv`: axis Z correlates -0.357 with the speed
derivative and axis Y correlates 0.000, while Y is the one that matches yaw
rate. Z is longitudinal, Y is lateral, and no one had to measure the bracket.

Until forward is found, `ready` is False and the caller must show nothing --
the same honesty rule as `gauge::view_available`. A guessed axis would draw a
convincing cornering figure that is really a bump.
"""

import math

# Seconds of history in the gravity average. Long enough that a 30-second
# motorway curve cannot lean the vertical over, short enough that the gauge
# being knocked into a new angle is forgiven within a minute.
GRAVITY_TAU_S = 30.0

# How much evidence forward needs before it is believed, as the summed square
# of the observed longitudinal acceleration in g. Braking is what pays this
# off. Tuned on the 2026-08-29 mounted drive by locking at a range of values
# and measuring how far the axis then moved for the rest of the drive: 0.05
# locks at 5.3 min and drifts 17 deg afterwards, 0.20 locks at 6.5 min and
# drifts 5.8 deg, 0.60 locks at 7.1 min and drifts 0.7 deg. 0.20 buys almost
# all of the accuracy for almost none of the wait. The final direction is the
# same to two decimals at every threshold, which is the real result: the axis
# is a property of the bracket, not of the tuning.
FORWARD_CONFIDENCE = 0.20

# Speed changes are only usable as a forward reference across these gaps.
# The lower bound is not about the channel refreshing -- it refreshes at 3 Hz.
# It is about quantisation: OBD speed arrives in whole km/h, so over 0.33 s one
# count of rounding *is* 0.086 g of imaginary acceleration, which swamps the
# thing we are trying to measure. Over 1 s the same count is worth a third of
# that, and several accelerometer samples fall inside the window to average.
MIN_SPEED_DT = 0.9
MAX_SPEED_DT = 3.0

# A speed-derived acceleration smaller than this is not worth learning from.
MIN_LEARN_G = 0.04

# Horizontal g past which a sample is not believed as a peak. A road car on
# road tyres does not make 1.5 g; a gauge knocked with an elbow makes far more
# in one sample. The live dot still shows it -- suppressing the reading would
# be a lie about what the part reported -- but the drive's high-water marks
# are what get quoted on the summary card afterwards, and one knock must not
# pin them there for the rest of the drive. Same reasoning as the plausibility
# gate the peaks in state.py already sit behind.
PEAK_SANE_G = 1.5


def _norm(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _scale(v, k):
    return (v[0] * k, v[1] * k, v[2] * k)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


class GForce(object):
    """Learns the mounting angle, then reports g in the car's own axes.

    Feed it every accelerometer sample with `update`, and every road-speed
    reading with `speed`. Read `lat`, `lon` and `ready`.

    Sign convention, chosen so the numbers read the way a driver thinks:
    `lon` is positive under braking and negative under acceleration, `lat` is
    positive in a right-hand turn. Both are in g.
    """

    def __init__(self):
        self.down = None          # unit vector, gauge axes
        self.down_g = None        # its length before normalising, ~1.000
        self.fwd = None           # unit vector, gauge axes, points forward
        self.right = None         # unit vector, gauge axes, points right
        self.lat = 0.0
        self.lon = 0.0
        self.peak_lat = 0.0
        self.peak_lon_brake = 0.0
        self.peak_lon_accel = 0.0
        self._grav = None         # the running average, un-normalised
        self._last_t = None
        self._spd = None
        self._spd_t = None
        self._learn_v = (0.0, 0.0, 0.0)   # sum of residual * reference accel
        self._learn_w = 0.0               # sum of reference accel squared
        self._res = (0.0, 0.0, 0.0)       # last horizontal residual
        # The residual averaged across the current speed window. A speed delta
        # describes what the car did over a whole second, so it has to be
        # matched against what the accelerometer saw over that same second --
        # not against whichever single sample happened to land last.
        self._win = (0.0, 0.0, 0.0)
        self._win_s = 0.0

    @property
    def ready(self):
        """True once both down and forward are known well enough to quote."""
        return self.fwd is not None

    @property
    def total(self):
        """Combined horizontal g -- how hard the car is being worked."""
        return math.sqrt(self.lat * self.lat + self.lon * self.lon)

    def speed(self, t, speed_kph):
        """Feed a road-speed reading, in km/h, on the drive's own clock.

        Only used to learn which way forward is. Once `ready` is True this
        stops mattering, and cornering keeps working with the car stationary
        in a way a speed-derived score never could.
        """
        if speed_kph is None:
            return
        if self._spd is None:
            self._spd, self._spd_t = speed_kph, t
            return
        dt = t - self._spd_t
        if dt < MIN_SPEED_DT:
            return                       # keep filling the window
        if dt > MAX_SPEED_DT or self._grav is None or self._win_s <= 0:
            self._reset_window(speed_kph, t)
            return
        # m/s^2 -> g. Positive means the car sped up.
        a = (speed_kph - self._spd) / 3.6 / dt / 9.81
        r = _scale(self._win, 1.0 / self._win_s)
        self._reset_window(speed_kph, t)
        if abs(a) < MIN_LEARN_G:
            return
        # Accumulate residual*a. Windows where the car was accelerating hard
        # dominate, which is what we want: they carry the direction.
        self._learn_v = (self._learn_v[0] + r[0] * a,
                         self._learn_v[1] + r[1] * a,
                         self._learn_v[2] + r[2] * a)
        self._learn_w += a * a
        self._solve()

    def _reset_window(self, speed_kph, t):
        self._spd, self._spd_t = speed_kph, t
        self._win = (0.0, 0.0, 0.0)
        self._win_s = 0.0

    def update(self, t, ax, ay, az):
        """Feed one accelerometer sample, in g, on the drive's own clock."""
        if ax is None or ay is None or az is None:
            return
        a = (float(ax), float(ay), float(az))
        if self._grav is None:
            self._grav = a
            self._last_t = t
            return
        dt = t - self._last_t
        self._last_t = t
        if dt <= 0 or dt > 5.0:
            return
        # Exponential average towards the true vertical.
        k = 1.0 - math.exp(-dt / GRAVITY_TAU_S)
        self._grav = (self._grav[0] + (a[0] - self._grav[0]) * k,
                      self._grav[1] + (a[1] - self._grav[1]) * k,
                      self._grav[2] + (a[2] - self._grav[2]) * k)
        g = _norm(self._grav)
        if g < 0.5:
            return                      # nonsense; the part is not reporting
        self.down_g = g
        self.down = _scale(self._grav, 1.0 / g)
        # Take gravity out. What is left is flat against the road.
        self._res = _sub(a, _scale(self.down, _dot(a, self.down)))
        self._win = (self._win[0] + self._res[0] * dt,
                     self._win[1] + self._res[1] * dt,
                     self._win[2] + self._res[2] * dt)
        self._win_s += dt
        if self.fwd is None:
            return
        # Braking is a backwards push, so negate to make braking positive.
        self.lon = -_dot(self._res, self.fwd)
        self.lat = _dot(self._res, self.right)
        if self.total > PEAK_SANE_G:
            return
        if abs(self.lat) > self.peak_lat:
            self.peak_lat = abs(self.lat)
        if self.lon > self.peak_lon_brake:
            self.peak_lon_brake = self.lon
        if -self.lon > self.peak_lon_accel:
            self.peak_lon_accel = -self.lon

    def _solve(self):
        """Turn the accumulated evidence into a forward and a right axis."""
        if self._learn_w < FORWARD_CONFIDENCE or self.down is None:
            return
        v = self._learn_v
        # Flatten it: the answer must lie in the road plane, and a little
        # vertical leaks in from bumps hit while braking.
        f = _sub(v, _scale(self.down, _dot(v, self.down)))
        n = _norm(f)
        if n < 1e-6:
            return
        self.fwd = _scale(f, 1.0 / n)
        # Right = down x forward. With down pointing into the road and
        # forward down the road, this comes out as the driver's right.
        r = _cross(self.down, self.fwd)
        n = _norm(r)
        if n < 1e-6:
            self.fwd = None
            return
        self.right = _scale(r, 1.0 / n)

    # -- persistence ---------------------------------------------------------
    # A bracket does not move between drives, so the six minutes of learning
    # should be paid once, not every time the key is turned. The caller saves
    # `export_axes()` when a drive ends and hands it back at the next boot;
    # learning carries on regardless, so a mount that *was* moved corrects
    # itself instead of trusting a stale answer for ever.
    def export_axes(self):
        if not self.ready:
            return None
        return {'fwd': list(self.fwd), 'down': list(self.down),
                'weight': self._learn_w}

    def restore_axes(self, saved):
        if not saved:
            return
        try:
            fwd = tuple(float(x) for x in saved['fwd'])
            down = tuple(float(x) for x in saved['down'])
            w = float(saved.get('weight', FORWARD_CONFIDENCE))
        except (KeyError, TypeError, ValueError):
            return
        if abs(_norm(fwd) - 1.0) > 0.01 or abs(_norm(down) - 1.0) > 0.01:
            return
        self.fwd, self.down = fwd, down
        r = _cross(down, fwd)
        n = _norm(r)
        if n < 1e-6:
            self.fwd = None
            return
        self.right = _scale(r, 1.0 / n)
        # Seed the evidence too, so one bumpy minute cannot outvote a whole
        # drive's worth of learning that already agreed with this.
        self._learn_v = _scale(fwd, w)
        self._learn_w = w

    def snapshot(self):
        return {
            'ready': self.ready,
            'lat': self.lat if self.ready else None,
            'lon': self.lon if self.ready else None,
            'total': self.total if self.ready else None,
            'peak_lat': self.peak_lat if self.ready else None,
            'peak_brake': self.peak_lon_brake if self.ready else None,
            'peak_accel': self.peak_lon_accel if self.ready else None,
            # 1.000 means the gauge did not move on its mount all drive.
            'mount_g': self.down_g,
            # 0 until forward is found, 1 once it is settled. Drawn as the
            # 'learning' state on the g view rather than hidden, because a
            # blank circle with no explanation reads as a broken gauge.
            'confidence': min(1.0, self._learn_w / FORWARD_CONFIDENCE),
        }
