"""Derived metrics: fuel economy, trip accumulators and the driving score.

Pure Python over a stream of samples — no I/O, so it is testable on the host
and ports directly to the firmware.
"""
import math

from . import gforce as _gforce

# Fuel price (RM per litre) used for the cost readouts. RM1.99 is the BUDI95
# subsidised RON95 pump price from 30 September 2025; RM2.05 was the old
# blanket-subsidy price this started life with.
FUEL_PRICE_RM = 1.99

# ---------------------------------------------------------------------------
# Driving-score tuning (SPEC.md B3, decided 2026-08-29).
#
# The score answers one question: *how tidily did you do whatever you were
# doing?* It deliberately does not answer "were you driving gently?", because
# the old score did, and that made a good backroad drive look like bad driving.
#
# Two poles, one formula. Intensity -- built from revs, g and throttle -- picks
# which pole is live, and the pole picks the bands. The weights never change,
# only the thresholds, so there is one code path and one story.
#
# Fuel economy is not in here at all any more. It is a readout on the Trip
# view. A score that marks you down for enjoying the car is the thing B3 was
# opened to stop.
# ---------------------------------------------------------------------------

W_THROTTLE = 0.30
W_BRAKING = 0.30
W_CORNERING = 0.25
W_CARE = 0.15

# Efficient cruising band for the Skyactiv-G 2.0 (rpm). Still used by the
# Fuel economy view's readout; no longer part of the score.
ECO_RPM_LO = 1200
ECO_RPM_HI = 2600

# --- the poles -------------------------------------------------------------
# The gap between the two is deliberate. A single threshold would flutter every
# time a number sat on it; a drive has to mean it to change pole.
NICE_BELOW = 0.35
SPIRITED_ABOVE = 0.55
# ...and it has to mean it for this long. Same latch idea as the mood unit.
POLE_HOLD_S = 5.0
# Intensity is a rolling read of the last half-minute, not an instant one.
# The dot on the g view is what you just did; the pole is how you have been
# driving. They are supposed to move at different speeds.
INTENSITY_TAU_S = 30.0

# Fractions of redline that map intensity 0 -> 1 on the rev axis.
RPM_CALM_FRAC = 0.30
RPM_HOT_FRAC = 0.85
# Horizontal g that counts as fully committed.
G_HOT = 0.45
# Throttle opening, in %, from "cruising" to "meaning it".
THR_CALM = 25.0
THR_HOT = 70.0

# --- the bands -------------------------------------------------------------
# Throttle movement, %/s, at which the demerit is full.
THR_RATE_NICE = 12.0
THR_RATE_SPIRITED = 40.0
# A change of throttle direction bigger than this counts as a reversal:
# on, off, on again. Cheap in a Nice segment, expensive in a Spirited one,
# where it means you did not know what you wanted the car to do.
THR_REVERSAL_PCT = 4.0
THR_REVERSAL_COST_S = 0.5

# Braking. Nice scores the size of it; Spirited scores how fast it arrives and
# leaves, because a good hard stop ramps in and bleeds off.
BRAKE_G_NICE = 0.25
BRAKE_JERK_SPIRITED = 1.5        # g per second

# Cornering. Same split: Nice scores the g, Spirited scores the sawing.
CORNER_G_NICE = 0.30
CORNER_JERK_SPIRITED = 1.2       # g per second
CORNER_ACTIVE_G = 0.20           # below this there is no corner to spoil

# Mechanical care. This is what replaced economy as the guard on the Spirited
# pole: revving a cold engine is the one thing that is wrong however tidily
# you do it.
CARE_COLD_C = 80.0
CARE_COLD_RPM = 3500.0
CARE_COLD_RPM_FULL = 5500.0
CARE_HOT_C = 105.0


class Trip(object):
    """Accumulates distance, fuel and time from a stream of updates."""

    def __init__(self):
        self.dist_km = 0.0
        self.fuel_l = 0.0
        self.moving_s = 0.0
        self.elapsed_s = 0.0
        self._last_t = None
        self._last_speed = None
        self._last_rate = None

    def update(self, t, speed_kph, fuel_rate_lph):
        if self._last_t is None:
            self._last_t = t
            self._last_speed = speed_kph
            self._last_rate = fuel_rate_lph
            return
        dt = t - self._last_t
        # ignore pauses/gaps — they'd otherwise integrate garbage
        if dt <= 0 or dt > 5.0:
            self._last_t = t
            self._last_speed = speed_kph
            self._last_rate = fuel_rate_lph
            return
        self.elapsed_s += dt
        if speed_kph is not None and self._last_speed is not None:
            avg = (speed_kph + self._last_speed) / 2.0
            self.dist_km += avg * dt / 3600.0
            if avg > 1.0:
                self.moving_s += dt
        if fuel_rate_lph is not None and self._last_rate is not None:
            self.fuel_l += (fuel_rate_lph + self._last_rate) / 2.0 * dt / 3600.0
        self._last_t = t
        if speed_kph is not None:
            self._last_speed = speed_kph
        if fuel_rate_lph is not None:
            self._last_rate = fuel_rate_lph

    @property
    def cost_rm(self):
        return self.fuel_l * FUEL_PRICE_RM

    @property
    def econ_l_per_100(self):
        """Trip average L/100km. None until enough distance to be meaningful."""
        if self.dist_km < 0.2:
            return None
        return self.fuel_l / self.dist_km * 100.0

    @property
    def econ_km_per_l(self):
        """Trip average km/L — the same figure the other way up."""
        return km_per_l(self.econ_l_per_100)

    @property
    def avg_speed_kph(self):
        if self.moving_s < 5:
            return 0.0
        return self.dist_km / (self.moving_s / 3600.0)


def instant_econ(speed_kph, fuel_rate_lph):
    """Instantaneous L/100km. None when stationary (it would be infinite).

    Kept in L/100km because the driving score's econ band is tuned in those
    units (§7.5 B3). Everything shown on screen goes through `km_per_l` first.
    """
    if speed_kph is None or fuel_rate_lph is None:
        return None
    if speed_kph < 3.0:
        return None
    return fuel_rate_lph / speed_kph * 100.0


def km_per_l(l_per_100km):
    """L/100km -> km/L, the unit the display uses.

    Returns None for a non-positive input as well as for no input. Zero
    L/100km is a real reading — an engine on overrun cuts the injectors
    entirely — but the reciprocal of it is infinite, which is neither
    printable nor valid JSON. It shows as '--' for the second or two it lasts.
    """
    if l_per_100km is None or l_per_100km <= 0:
        return None
    return 100.0 / l_per_100km


class DrivingScore(object):
    """0-100 "how tidily am I driving" from four sub-scores.

    throttle  : jerky or indecisive use of the right pedal
    braking   : how hard you stop (Nice) or how abruptly (Spirited)
    cornering : how much lateral g (Nice) or how much sawing (Spirited)
    care      : revving the engine before it is warm, or cooking it

    **Everything is measured in demerit-seconds.** Each sample contributes
    `demerit * dt`, where demerit runs 0 (faultless) to 1 (as bad as the band
    goes), and an event like a throttle reversal contributes a fixed number of
    seconds outright. A sub-score is then `100 * (1 - demerit_s / observed_s)`.
    One currency for rates and events alike means the maths is sample-rate
    independent by construction, which is the bug this class was rewritten
    around once already (see the note on per-channel clocks below) and the
    reason the C++ port can be a line-for-line mirror.

    **Every rate here is measured against its own channel's clock.** The gauge
    feeds this on every sample of *any* channel, carrying the last-known value
    of the rest, so `t` advances far faster than any single channel updates: on
    a real 35-channel drive samples arrive every 0.077 s while speed refreshes
    every 0.330 s. Dividing a change by the time since the last *sample* rather
    than the last *reading* inflated acceleration roughly 4x and turned every
    1 km/h wiggle into a harsh event -- 2285 of them on a drive that really
    contained 4. It also made the score depend on how many channels a car
    happens to report, so the same driving scored differently in different
    cars.

    **Missing channel means missing sub-score**, not a zero. A car with no
    coolant channel has no `care` score and the other three renormalise. This
    is the same honesty rule as `gauge::view_available`: a convincing number
    for data we do not have is worse than an em-dash.
    """

    def __init__(self, gforce=None):
        self.g = gforce if gforce is not None else _gforce.GForce()
        self._last_t = None
        self._thr = None             # last throttle value and when it was seen
        self._thr_t = None
        self._thr_dir = 0            # +1 opening, -1 closing, 0 unknown
        self._thr_pivot = None       # where the pedal last changed direction
        self._lon = None             # last longitudinal g and when
        self._lon_t = None
        self._lat = None
        self._lat_t = None
        # observed time and demerit-seconds, per sub-score
        self.thr_s = 0.0
        self.thr_bad = 0.0
        self.brake_s = 0.0
        self.brake_bad = 0.0
        self.corner_s = 0.0
        self.corner_bad = 0.0
        self.care_s = 0.0
        self.care_bad = 0.0
        # the pole
        self.intensity = 0.0
        self.pole = 'NICE'
        self._cand = 'NICE'
        self._cand_since = None
        self.nice_s = 0.0
        self.spirited_s = 0.0
        self.rev_s = 0.0
        self.rpm_red = 7000.0        # overwritten from the car profile
        self.events = []

    # -- ingest --------------------------------------------------------------
    def imu(self, t, ax, ay, az):
        """Feed one accelerometer sample. Separate from `update` because the
        IMU is read on its own clock -- on the board far faster than the OBD
        channels, which is the whole point of using it for jerk."""
        self.g.update(t, ax, ay, az)

    def _rebase(self, t, throttle_pct):
        """Drop the derivative baselines. Used at the start and across gaps, so
        a pause in the recording is never read as violent driving."""
        self._last_t = t
        self._thr, self._thr_t = throttle_pct, t
        self._thr_pivot = throttle_pct
        self._thr_dir = 0
        self._lon = self._lat = None
        self._lon_t = self._lat_t = None

    def update(self, t, speed_kph, rpm, throttle_pct, fuel_rate_lph,
               coolant_c=None):
        self.g.speed(t, speed_kph)
        if self._last_t is None:
            self._rebase(t, throttle_pct)
            return
        dt = t - self._last_t
        if dt == 0:
            # Two channels sampled in the same millisecond -- a tie, not a gap.
            # Timestamps are written to 3 decimals and a 35-channel sweep fills
            # those easily. Rebasing here was the real defect: it dragged the
            # per-channel baseline timestamps forward while their values stayed
            # put, so the next genuine change was divided by a fraction of the
            # time it actually took.
            return
        if dt < 0 or dt > 5.0:
            self._rebase(t, throttle_pct)
            return
        self._last_t = t
        if rpm is not None and rpm > 400:
            self.rev_s += dt

        self._intensity(dt, rpm, throttle_pct)
        self._pole(t, dt)
        spirited = self.pole == 'SPIRITED'
        self._throttle(t, throttle_pct, spirited)
        self._braking(t, dt, spirited)
        self._cornering(t, dt, spirited)
        self._care(dt, rpm, coolant_c)

    # -- the pole ------------------------------------------------------------
    def _intensity(self, dt, rpm, throttle_pct):
        """How hard the car is being driven, 0-1. Never scored, only used to
        pick which yardstick the sub-scores are measured against."""
        parts = []
        if rpm is not None and self.rpm_red > 0:
            lo = self.rpm_red * RPM_CALM_FRAC
            hi = self.rpm_red * RPM_HOT_FRAC
            parts.append((rpm - lo) / (hi - lo))
        if throttle_pct is not None:
            parts.append((throttle_pct - THR_CALM) / (THR_HOT - THR_CALM))
        if self.g.ready:
            parts.append(self.g.total / G_HOT)
        if not parts:
            return
        # The loudest signal wins. A car held at 6000 rpm through a long
        # sweeper is spirited even at a steady throttle, and a car braked at
        # 0.5 g is spirited at any rpm -- averaging would hide both.
        raw = _clamp(max(parts), 0.0, 1.0)
        k = 1.0 - math.exp(-dt / INTENSITY_TAU_S)
        self.intensity += (raw - self.intensity) * k

    def _pole(self, t, dt):
        if self.intensity >= SPIRITED_ABOVE:
            want = 'SPIRITED'
        elif self.intensity <= NICE_BELOW:
            want = 'NICE'
        else:
            want = self.pole          # in the gap, nothing changes
        if want != self._cand:
            self._cand, self._cand_since = want, t
        if (want != self.pole and self._cand_since is not None
                and t - self._cand_since >= POLE_HOLD_S):
            self.pole = want
            self.events.append((round(t, 2), 'pole', want))
        if self.pole == 'SPIRITED':
            self.spirited_s += dt
        else:
            self.nice_s += dt

    # -- sub-scores ----------------------------------------------------------
    def _throttle(self, t, throttle_pct, spirited):
        if throttle_pct is None:
            return
        if self._thr is None:
            self._thr, self._thr_t = throttle_pct, t
            self._thr_pivot = throttle_pct
            return
        d = t - self._thr_t
        if d <= 0:
            return
        move = throttle_pct - self._thr
        self._thr, self._thr_t = throttle_pct, t
        self.thr_s += d
        # How fast the pedal is moving, against the band for this pole.
        band = THR_RATE_SPIRITED if spirited else THR_RATE_NICE
        self.thr_bad += _clamp(abs(move) / d / band, 0.0, 1.0) * d
        # A reversal: the pedal turned round by more than the deadband. Big
        # inputs are fine, changing your mind about them is not.
        if abs(move) < 0.05:
            return
        direction = 1 if move > 0 else -1
        if self._thr_dir == 0:
            self._thr_dir, self._thr_pivot = direction, throttle_pct
            return
        if direction == self._thr_dir:
            self._thr_pivot = throttle_pct
            return
        if abs(throttle_pct - self._thr_pivot) >= THR_REVERSAL_PCT:
            self.thr_bad += THR_REVERSAL_COST_S
            self.events.append((round(t, 2), 'reversal', round(throttle_pct, 1)))
        self._thr_dir, self._thr_pivot = direction, throttle_pct

    def _braking(self, t, dt, spirited):
        """Nice scores the size of the stop. Spirited scores its edges."""
        if not self.g.ready:
            return
        lon = self.g.lon
        self.brake_s += dt
        if spirited:
            if self._lon is not None and t > self._lon_t:
                jerk = abs(lon - self._lon) / (t - self._lon_t)
                self.brake_bad += _clamp(jerk / BRAKE_JERK_SPIRITED,
                                         0.0, 1.0) * dt
        else:
            self.brake_bad += _clamp(abs(lon) / BRAKE_G_NICE, 0.0, 1.0) * dt
        self._lon, self._lon_t = lon, t

    def _cornering(self, t, dt, spirited):
        """Nice scores the lateral g. Spirited scores sawing at the wheel."""
        if not self.g.ready:
            return
        lat = self.g.lat
        self.corner_s += dt
        if spirited:
            if (self._lat is not None and t > self._lat_t
                    and abs(lat) >= CORNER_ACTIVE_G):
                jerk = abs(lat - self._lat) / (t - self._lat_t)
                self.corner_bad += _clamp(jerk / CORNER_JERK_SPIRITED,
                                          0.0, 1.0) * dt
        else:
            self.corner_bad += _clamp(abs(lat) / CORNER_G_NICE, 0.0, 1.0) * dt
        self._lat, self._lat_t = lat, t

    def _care(self, dt, rpm, coolant_c):
        if coolant_c is None or rpm is None or rpm <= 400:
            return
        self.care_s += dt
        if coolant_c >= CARE_HOT_C:
            self.care_bad += dt
            return
        if coolant_c < CARE_COLD_C and rpm > CARE_COLD_RPM:
            span = CARE_COLD_RPM_FULL - CARE_COLD_RPM
            self.care_bad += _clamp((rpm - CARE_COLD_RPM) / span, 0.0, 1.0) * dt

    @staticmethod
    def _sub(bad, seconds, floor=5.0):
        """A sub-score, or None when the channel never gave us enough to say."""
        if seconds < floor:
            return None
        return _clamp(100.0 * (1.0 - bad / seconds))

    @property
    def throttle(self):
        return self._sub(self.thr_bad, self.thr_s)

    @property
    def braking(self):
        return self._sub(self.brake_bad, self.brake_s)

    @property
    def cornering(self):
        return self._sub(self.corner_bad, self.corner_s)

    @property
    def care(self):
        return self._sub(self.care_bad, self.care_s)

    @property
    def total(self):
        subs = [(self.throttle, W_THROTTLE), (self.braking, W_BRAKING),
                (self.cornering, W_CORNERING), (self.care, W_CARE)]
        have = [(v, w) for v, w in subs if v is not None]
        if not have:
            return None
        wsum = sum(w for _, w in have)
        return sum(v * w for v, w in have) / wsum

    @property
    def coach(self):
        """One word on the weakest area, in the language of the live pole."""
        t = self.total
        if t is None:
            return 'WARMING UP'
        if t >= 85:
            return 'CLEAN'
        if t >= 70:
            return 'TIDY'
        spirited = self.pole == 'SPIRITED'
        subs = [(self.throttle, 'INDECISIVE' if spirited else 'JERKY'),
                (self.braking, 'SNATCHY' if spirited else 'HARSH'),
                (self.cornering, 'SAWING' if spirited else 'FAST IN'),
                (self.care, 'COLD REVS')]
        have = [(v, lbl) for v, lbl in subs if v is not None]
        return min(have)[1] if have else 'OK'

    def snapshot(self):
        return {
            'total': self.total,
            'throttle': self.throttle,
            'braking': self.braking,
            'cornering': self.cornering,
            'care': self.care,
            'coach': self.coach,
            'pole': self.pole,
            'intensity': self.intensity,
            'nice_s': self.nice_s,
            'spirited_s': self.spirited_s,
            'g': self.g.snapshot(),
        }


def _clamp(v, lo=0.0, hi=100.0):
    return max(lo, min(hi, v))
