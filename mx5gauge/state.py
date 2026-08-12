"""Shared vehicle state + derived metrics, updated by whichever source is running."""
import math
import threading
import time

from . import metrics, vehicle

# Physically plausible range per channel. Anything outside is dropped rather
# than stored: a resync glitch in a capture (or a mangled BLE reply) can yield
# values like 6e-310 volts, which would poison both the log and the metrics.
RANGES = {
    'rpm': (0, 9000),
    'speed': (0, 320),
    'coolant': (-50, 200),
    'intake': (-50, 200),
    'ambient': (-50, 100),
    'oil': (-50, 250),
    'catalyst': (0, 1400),
    'fuel_rail_temp': (-50, 250),
    'throttle': (0, 100),
    'load': (0, 100),
    'abs_load': (0, 400),
    'volts': (4, 20),
    'ctrl_volt': (4, 20),
    'fuel_rate': (0, 120),
    'maf': (0, 700),
    'timing': (-70, 70),
    'map': (0, 400),
    'baro': (0, 200),
    'ref_torque': (0, 2000),
    'act_torque': (-125, 130),
    'torque_demand': (-125, 130),
    'power_kw': (0, 400),
    'fuel_level': (0, 100),
    'equiv_ratio': (0, 4),
    'ethanol': (0, 100),
    'inject_timing': (-220, 302),
    'evap_press': (-9000, 9000),
    'o2_b1s1': (0, 1.3),
    'o2_b1s2': (0, 1.3),
}

# Percent-style channels all share the same bounds.
for _k in ('stft1', 'ltft1', 'stft2', 'ltft2', 'egr_cmd', 'egr_err',
           'evap_purge', 'rel_thr', 'thr_b', 'thr_c', 'pedal', 'pedal_d',
           'pedal_e', 'pedal_f', 'thr_actuator', 'hybrid_soc'):
    RANGES[_k] = (-100, 100)
for _k in ('cat_b1s1', 'cat_b2s1', 'cat_b1s2', 'cat_b2s2'):
    RANGES[_k] = (-40, 1400)
for _k in ('fuel_press', 'rail_press', 'rail_gauge', 'rail_abs'):
    RANGES[_k] = (0, 800000)


def _first(v, *keys):
    """First of `keys` that holds a reading, else None.

    Live and replay don't always name the same channel identically — a live
    poll decodes PID 0x3C to `cat_b1s1`, while a Car Scanner capture calls it
    `catalyst` — and cars differ in which bank/sensor they populate. Checking
    for `is not None` rather than truthiness matters: 0 °C is a real reading.
    """
    for k in keys:
        if v.get(k) is not None:
            return v[k]
    return None


def plausible(key, value):
    """True if `value` is a sane reading for `key`.

    Unknown keys get a permissive default so newly-added channels still get
    logged; only obvious nonsense (NaN, absurd magnitudes) is dropped.
    """
    try:
        v = float(value)
    except (TypeError, ValueError):
        return False
    if not math.isfinite(v):
        return False
    lo, hi = RANGES.get(key, (-1e7, 1e7))
    return lo <= v <= hi


class Gauge(object):
    """Holds the latest readings and the derived metrics the views render."""

    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}          # key -> latest value
        self.updated = {}         # key -> monotonic timestamp
        self.trip = metrics.Trip()
        self.score = metrics.DrivingScore()
        self.peak_rpm = 0.0
        self.peak_kw = 0.0
        self.status = 'starting'
        self.source_kind = ''
        self.started = time.time()
        self._t0 = None
        self.recorder = None      # set by run.py; every sample is written to it
        self.rejected = 0         # implausible readings dropped
        # desk preview: 'LO-HI' asks the UI to sweep rpm instead of using the
        # recorded revs, so the visuals can be judged on a capture that idles.
        # Never set in live mode — real rpm is the point when you're driving.
        self.preview_sweep = None

    # -- ingest --------------------------------------------------------------
    def sample(self, key, value, t=None):
        """Feed one reading.

        `t` is the *logical* timestamp in seconds — wall-clock when live, but
        the capture's own timeline during replay. Metrics must use it, or a
        sped-up replay would read as violent acceleration. Freshness still
        tracks wall-clock so the UI can grey out stale channels either way.
        """
        now = time.time()
        if t is None:
            t = now
        meta = key.startswith('_')
        if not meta and not plausible(key, value):
            self.rejected += 1
            return
        # persist first, so nothing is lost even if the maths below throws.
        # `_`-prefixed keys are metadata (car identity, PID lists), not
        # readings — they belong in the session summary, not the sample log.
        if self.recorder is not None and not meta:
            self.recorder.write(key, value, t)
        with self.lock:
            if meta:
                self.values[key] = value
                return
            self.values[key] = value
            self.updated[key] = now

            if key == 'rpm' and value > self.peak_rpm:
                self.peak_rpm = value

            v = self.values
            # engine power estimate (kW) from torque % x reference torque x rpm
            if 'act_torque' in v and 'ref_torque' in v and 'rpm' in v:
                nm = v['ref_torque'] * max(0.0, v['act_torque']) / 100.0
                kw = nm * v['rpm'] * 2 * 3.14159 / 60.0 / 1000.0
                v['power_kw'] = kw
                if kw > self.peak_kw:
                    self.peak_kw = kw

            if self._t0 is None:
                self._t0 = t
            self.trip.update(t, v.get('speed'), v.get('fuel_rate'))
            self.score.update(t, v.get('speed'), v.get('rpm'),
                              v.get('throttle'), v.get('fuel_rate'))

    # -- render --------------------------------------------------------------
    def snapshot(self):
        with self.lock:
            v = dict(self.values)
            now = time.time()
            fresh = {k: (now - ts) < 5.0 for k, ts in self.updated.items()}
            coolant = v.get('coolant')
            if coolant is None:
                warm = 'UNKNOWN'
            elif coolant < 60:
                warm = 'COLD'
            elif coolant < 80:
                warm = 'WARMING'
            elif coolant <= 105:
                warm = 'READY'
            else:
                warm = 'HOT'

            volts = v.get('ctrl_volt', v.get('volts'))
            if volts is None:
                charge = 'UNKNOWN'
            elif volts < 12.2:
                charge = 'LOW'
            elif volts < 13.2:
                charge = 'NOT CHARGING'
            elif volts <= 15.0:
                charge = 'CHARGING'
            else:
                charge = 'HIGH'

            return {
                'status': self.status,
                'source': self.source_kind,
                'live': {
                    'rpm': v.get('rpm'),
                    'speed': v.get('speed'),
                    'coolant': coolant,
                    'intake': v.get('intake'),
                    'throttle': v.get('throttle'),
                    'volts': volts,
                    'fuel_rate': v.get('fuel_rate'),
                    'load': v.get('load'),
                    'maf': v.get('maf'),
                    'timing': v.get('timing'),
                    'catalyst': _first(v, 'catalyst', 'cat_b1s1', 'cat_b2s1',
                                       'cat_b1s2', 'cat_b2s2'),
                    'fuel_rail_temp': v.get('fuel_rail_temp'),
                    'power_kw': v.get('power_kw'),
                    'oil': v.get('oil'),
                },
                'derived': {
                    'warm': warm,
                    'charge': charge,
                    'econ_now': metrics.instant_econ(v.get('speed'), v.get('fuel_rate')),
                    'econ_avg': self.trip.econ_l_per_100,
                    'dist_km': self.trip.dist_km,
                    'fuel_l': self.trip.fuel_l,
                    'cost_rm': self.trip.cost_rm,
                    'elapsed_s': self.trip.elapsed_s,
                    'moving_s': self.trip.moving_s,
                    'avg_speed': self.trip.avg_speed_kph,
                    'peak_rpm': self.peak_rpm,
                    'peak_kw': self.peak_kw,
                },
                'score': {
                    'total': self.score.total,
                    'smooth': self.score.smooth,
                    'econ': self.score.econ,
                    'calm': self.score.calm,
                    'coach': self.score.coach,
                    'harsh': self.score.harsh,
                },
                'fresh': fresh,
                'supported': v.get('_supported'),
                # what car this is, and which channels it can actually feed —
                # the views use these to scale their dials and to say "n/a"
                # instead of drawing a convincing zero
                'car': v.get('_car') or vehicle.identify(),
                'channels': v.get('_supported_keys'),
                'preview_sweep': self.preview_sweep,
            }
