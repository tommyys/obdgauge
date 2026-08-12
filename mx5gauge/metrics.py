"""Derived metrics: fuel economy, trip accumulators and the driving score.

Pure Python over a stream of samples — no I/O, so it is testable on the host
and ports directly to the firmware.
"""

# Fuel price (RM per litre) used for the cost readouts. Adjust to taste.
FUEL_PRICE_RM = 2.05

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
    def avg_speed_kph(self):
        if self.moving_s < 5:
            return 0.0
        return self.dist_km / (self.moving_s / 3600.0)


def instant_econ(speed_kph, fuel_rate_lph):
    """Instantaneous L/100km. None when stationary (it would be infinite)."""
    if speed_kph is None or fuel_rate_lph is None:
        return None
    if speed_kph < 3.0:
        return None
    return fuel_rate_lph / speed_kph * 100.0


class DrivingScore(object):
    """0-100 'am I driving well' score from three sub-scores.

    smooth : penalises jerky throttle use
    econ   : rewards efficient fuel use and time in the efficient rpm band
    calm   : penalises harsh acceleration / braking events
    """

    def __init__(self):
        self._last_t = None
        self._last_thr = None
        self._last_speed = None
        self.jerk_sum = 0.0
        self.jerk_n = 0
        self.eco_s = 0.0
        self.rev_s = 0.0
        self.econ_sum = 0.0
        self.econ_n = 0
        self.harsh = 0
        self.events = []

    def update(self, t, speed_kph, rpm, throttle_pct, fuel_rate_lph):
        if self._last_t is None:
            self._last_t = t
            self._last_thr = throttle_pct
            self._last_speed = speed_kph
            return
        dt = t - self._last_t
        if dt <= 0 or dt > 5.0:
            self._last_t = t
            self._last_thr = throttle_pct
            self._last_speed = speed_kph
            return

        # throttle jerk (%/s) -> smoothness
        if throttle_pct is not None and self._last_thr is not None:
            self.jerk_sum += abs(throttle_pct - self._last_thr) / dt
            self.jerk_n += 1

        # time in the efficient rev band
        if rpm is not None and rpm > 400:
            if ECO_RPM_LO <= rpm <= ECO_RPM_HI:
                self.eco_s += dt
            self.rev_s += dt

        # instantaneous economy samples
        ie = instant_econ(speed_kph, fuel_rate_lph)
        if ie is not None and ie < 40:
            self.econ_sum += ie
            self.econ_n += 1

        # harsh accel / braking from speed delta (m/s^2)
        if speed_kph is not None and self._last_speed is not None:
            a = (speed_kph - self._last_speed) / 3.6 / dt
            if a > HARSH_ACCEL:
                self.harsh += 1
                self.events.append((t, 'accel', round(a, 2)))
            elif a < HARSH_BRAKE:
                self.harsh += 1
                self.events.append((t, 'brake', round(a, 2)))

        self._last_t = t
        if throttle_pct is not None:
            self._last_thr = throttle_pct
        if speed_kph is not None:
            self._last_speed = speed_kph

    # --- sub-scores (each 0-100) -------------------------------------------
    @property
    def smooth(self):
        if self.jerk_n < 5:
            return None
        avg = self.jerk_sum / self.jerk_n          # %/s
        # 0 %/s -> 100 ; 12 %/s -> 0
        return _clamp(100.0 - avg * 8.0)

    @property
    def econ(self):
        parts = []
        if self.rev_s > 5:
            parts.append(self.eco_s / self.rev_s * 100.0)
        if self.econ_n > 5:
            avg = self.econ_sum / self.econ_n      # L/100km
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
