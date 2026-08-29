"""Derived metrics: fuel economy, trip accumulators and the driving score.

Pure Python over a stream of samples — no I/O, so it is testable on the host
and ports directly to the firmware.
"""

# Fuel price (RM per litre) used for the cost readouts. RM1.99 is the BUDI95
# subsidised RON95 pump price from 30 September 2025; RM2.05 was the old
# blanket-subsidy price this started life with.
FUEL_PRICE_RM = 1.99

# Driving-score tuning. All weights sum to 1.0.
W_SMOOTH = 0.40
W_ECON = 0.30
W_CALM = 0.30

# Efficient cruising band for the Skyactiv-G 2.0 (rpm)
ECO_RPM_LO = 1200
ECO_RPM_HI = 2600

# Harsh-event thresholds (m/s^2). ~2.5 m/s^2 is a firm but normal stop.
HARSH_ACCEL = 2.5
HARSH_BRAKE = -3.0


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
    """0-100 'am I driving well' score from three sub-scores.

    smooth : penalises jerky throttle use
    econ   : rewards efficient fuel use and time in the efficient rpm band
    calm   : penalises harsh acceleration / braking events

    **Every rate here is measured against its own channel's clock.** The gauge
    feeds this on every sample of *any* channel, carrying the last-known value
    of the rest, so `t` advances far faster than any single channel updates: on
    a real 35-channel drive samples arrive every 0.077 s while speed refreshes
    every 0.330 s. Dividing a speed change by the time since the last *sample*
    rather than the last *speed* reading inflated acceleration roughly 4x and
    turned every 1 km/h wiggle into a harsh event — 2285 of them on a drive
    that really contained 4. It also made the score depend on how many channels
    a car happens to report, so the same driving scored differently in
    different cars. Rates are now per-channel and sample-rate independent.
    """

    def __init__(self):
        self._last_t = None          # any sample; used to integrate time
        self._thr = None             # last throttle value and when it was seen
        self._thr_t = None
        self._spd = None             # last speed value and when it was seen
        self._spd_t = None
        self.thr_travel = 0.0        # total throttle movement, %
        self.thr_seconds = 0.0       # seconds of throttle observation
        self.eco_s = 0.0
        self.rev_s = 0.0
        self.econ_sum = 0.0          # economy weighted by time, not by sample
        self.econ_s = 0.0
        self.harsh = 0
        self.events = []

    def _rebase(self, t, speed_kph, throttle_pct):
        """Drop the derivative baselines. Used at the start and across gaps, so
        a pause in the recording is never read as violent driving."""
        self._last_t = t
        self._thr, self._thr_t = throttle_pct, t
        self._spd, self._spd_t = speed_kph, t

    def update(self, t, speed_kph, rpm, throttle_pct, fuel_rate_lph):
        if self._last_t is None:
            self._rebase(t, speed_kph, throttle_pct)
            return
        dt = t - self._last_t
        if dt == 0:
            # Two channels sampled in the same millisecond — a tie, not a gap.
            # Timestamps are written to 3 decimals and a 35-channel sweep fills
            # those easily. Rebasing here was the real defect: it dragged the
            # per-channel baseline timestamps forward while their values stayed
            # put, so the next genuine speed change was divided by a fraction of
            # the time it actually took. A 15->13 km/h lift became -4.6 m/s².
            return
        if dt < 0 or dt > 5.0:
            self._rebase(t, speed_kph, throttle_pct)
            return
        self._last_t = t

        # Throttle movement per second. Total travel over total time rather than
        # an average of per-sample jerk: holding a steady throttle correctly
        # contributes time but no travel, whatever the sample rate.
        if throttle_pct is not None:
            if self._thr is None:
                self._thr, self._thr_t = throttle_pct, t
            else:
                d = t - self._thr_t
                if d > 0:
                    self.thr_travel += abs(throttle_pct - self._thr)
                    self.thr_seconds += d
                    self._thr, self._thr_t = throttle_pct, t

        # time in the efficient rev band — integrating time, so the every-sample
        # dt is the right one to use here
        if rpm is not None and rpm > 400:
            if ECO_RPM_LO <= rpm <= ECO_RPM_HI:
                self.eco_s += dt
            self.rev_s += dt

        # economy, weighted by time so a densely-sampled channel cannot
        # outvote a sparse one
        ie = instant_econ(speed_kph, fuel_rate_lph)
        if ie is not None and ie < 40:
            self.econ_sum += ie * dt
            self.econ_s += dt

        # Harsh accel / braking, from one speed reading to the next actual
        # change — never from a stale value against a fresh timestamp.
        if speed_kph is not None:
            if self._spd is None:
                self._spd, self._spd_t = speed_kph, t
            elif speed_kph != self._spd:
                d = t - self._spd_t
                if d > 0:
                    a = (speed_kph - self._spd) / 3.6 / d
                    if a > HARSH_ACCEL:
                        self.harsh += 1
                        self.events.append((t, 'accel', round(a, 2)))
                    elif a < HARSH_BRAKE:
                        self.harsh += 1
                        self.events.append((t, 'brake', round(a, 2)))
                self._spd, self._spd_t = speed_kph, t

    # --- sub-scores (each 0-100) -------------------------------------------
    @property
    def smooth(self):
        if self.thr_seconds < 5:
            return None
        rate = self.thr_travel / self.thr_seconds  # %/s of throttle movement
        # 0 %/s -> 100 ; 12 %/s -> 0
        return _clamp(100.0 - rate * 8.0)

    @property
    def econ(self):
        parts = []
        if self.rev_s > 5:
            parts.append(self.eco_s / self.rev_s * 100.0)
        if self.econ_s > 5:
            avg = self.econ_sum / self.econ_s      # L/100km, time-weighted
            # 5 L/100 -> 100 ; 15 L/100 -> 0
            parts.append(_clamp((15.0 - avg) * 10.0))
        if not parts:
            return None
        return sum(parts) / len(parts)

    @property
    def calm(self):
        mins = max(self.rev_s, 1.0) / 60.0
        rate = self.harsh / mins                   # events per minute
        return _clamp(100.0 - rate * 25.0)

    @property
    def total(self):
        subs = [(self.smooth, W_SMOOTH), (self.econ, W_ECON), (self.calm, W_CALM)]
        have = [(v, w) for v, w in subs if v is not None]
        if not have:
            return None
        wsum = sum(w for _, w in have)
        return sum(v * w for v, w in have) / wsum

    @property
    def coach(self):
        """One-word feedback on the weakest area."""
        t = self.total
        if t is None:
            return 'WARMING UP'
        subs = [(self.smooth, 'JERKY'), (self.econ, 'THIRSTY'), (self.calm, 'HARSH')]
        have = [(v, lbl) for v, lbl in subs if v is not None]
        if t >= 85:
            return 'SMOOTH'
        if t >= 70:
            return 'GOOD'
        if have:
            worst = min(have)[1]
            return worst
        return 'OK'


def _clamp(v, lo=0.0, hi=100.0):
    return max(lo, min(hi, v))
