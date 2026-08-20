"""Shared vehicle state + derived metrics, updated by whichever source is running."""
import math
import threading
import time

from . import ignition, metrics, vehicle

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


# How long the boot clip holds the screen, in milliseconds of the drive's own
# clock, and how long it then dips to black before the instruments arrive.
BOOT_MS = 2500
BOOT_FADE_MS = 600


class Boot(object):
    """Tracks the boot sequence: 'WAKING', 'FADING', then 'RUNNING'.

    The dip to black between the clip and the instruments is what keeps the
    two from colliding — the animation ends on a lit car filling the screen,
    and cutting straight from that to a dial would read as a glitch rather
    than a hand-off.

    Told the time rather than reading a clock, so a capture replayed at 8x
    splashes for the same 2.5 seconds *of the drive* that a car in front of you
    does — and so the whole thing is testable without sleeping.

    There is no 'ASLEEP'. The gauge is fed from an ignition-switched supply, so
    a car that is off is a board with no power, not a board in a state. The
    splash is the first thing that happens when the lights come on.
    """

    def __init__(self):
        self.phase = 'WAKING'
        self._t0 = None       # logical time of the first reading seen
        self._t = None        # the latest, to notice the clock going backwards

    def reset(self):
        self.__init__()

    @property
    def progress(self):
        """0 -> 1 through whichever step is playing; 1 once running.

        Measured within the current phase, so the UI can drive the clip and
        the dip to black off the same number without knowing the durations.
        """
        if self.phase == 'RUNNING' or self._t0 is None:
            return 1.0 if self.phase == 'RUNNING' else 0.0
        elapsed = self._t - self._t0
        if self.phase == 'WAKING':
            span = BOOT_MS / 1000.0
            return 1.0 if span <= 0 else max(0.0, min(1.0, elapsed / span))
        span = BOOT_FADE_MS / 1000.0
        if span <= 0:
            return 1.0
        return max(0.0, min(1.0, (elapsed - BOOT_MS / 1000.0) / span))

    def update(self, t):
        """Feed the logical timestamp of one real reading."""
        t = float(t)
        if self._t0 is None:
            self._t0 = t
        elif t < self._t:
            # Scrubbed backwards on the replay timeline. Rebase, or the boot
            # would sit waiting out an interval that has already gone by and
            # can never come round again.
            self._t0 = t
        self._t = t
        elapsed = t - self._t0
        if elapsed >= (BOOT_MS + BOOT_FADE_MS) / 1000.0:
            self.phase = 'RUNNING'
        elif elapsed >= BOOT_MS / 1000.0:
            self.phase = 'FADING'
        else:
            self.phase = 'WAKING'


# The catalyst temperature can arrive on any of several channels depending on
# which banks and sensors the car reports. `snapshot()` shows the first one
# present; a peak takes the hottest of whichever turned up, because the drive
# only got as hot as it got, whatever bank happened to measure it.
CATALYST_KEYS = ('catalyst', 'cat_b1s1', 'cat_b2s1', 'cat_b1s2', 'cat_b2s2')

# Channel -> the summary field its high-water mark belongs in. Every catalyst
# channel folds into one figure; the rest stand for themselves. These are the
# readings a drive is remembered by: how hard it was revved, how fast it went,
# and how hot it got doing it.
PEAK_FIELDS = dict([(k, k) for k in ('rpm', 'speed', 'coolant', 'intake')] +
                   [(k, 'catalyst') for k in CATALYST_KEYS])


class Gauge(object):
    """Holds the latest readings and the derived metrics the views render."""

    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}          # key -> latest value
        self.updated = {}         # key -> monotonic timestamp
        self.trip = metrics.Trip()
        self.score = metrics.DrivingScore()
        self.boot = Boot()
        self.ignition = ignition.Ignition()
        # Set by run.py on a live session to rotate the recording when the
        # engine stops and starts. Left None during replay: a recorded drive
        # replays its own ignition events, and acting on them would zero the
        # trip totals under you mid-scrub.
        self.on_ignition = None
        # field -> highest value seen this drive, for the channels in
        # PEAK_FIELDS. A dict rather than an attribute each, so the catalyst
        # family can collapse into one entry.
        self.peaks = {}
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
        self.current_file = None   # basename of the drive being replayed, if any
        # set by run.py to the ReplaySource currently playing, so the snapshot
        # can report where along the drive we are. None when live.
        self.replay = None

    def reset(self):
        """Clear everything a new drive must not inherit.

        Loading another drive has to start from nothing: leftover trip totals
        would silently accumulate across two unrelated drives, and stale
        channel values would keep views lit for data the new file never sends.
        Metadata (`_car`, `_supported_keys`) goes too, so the banner and the
        view gating re-derive from whatever is now playing.
        """
        with self.lock:
            self.values.clear()
            self.updated.clear()
            self.trip = metrics.Trip()
            self.score = metrics.DrivingScore()
            self.boot.reset()
            self.ignition = ignition.Ignition()
            self.peaks = {}
            self.peak_kw = 0.0
            self._t0 = None
            self.rejected = 0

    @property
    def peak_rpm(self):
        """Highest rpm this drive, or 0.0 before any rev is seen.

        Zero rather than None because the tacho and the score footer draw it
        unconditionally. The peaks added for the drive card report None when
        their channel never arrived, which is the honest answer there: a car
        that never sent a catalyst reading has no peak catalyst, and '--' says
        so where a bold 0 degrees would lie.
        """
        return self.peaks.get('rpm', 0.0)

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

            # High-water marks. Below the plausibility gate above on purpose:
            # a peak is the most memorable number on the summary card, and a
            # single bad frame would otherwise pin it there for the whole drive.
            field = PEAK_FIELDS.get(key)
            if field is not None and value > self.peaks.get(field, float('-inf')):
                self.peaks[field] = value

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
            self.boot.update(t)
            event = (self.ignition.update(t, key, value)
                     if self.on_ignition is not None else None)

        # Outside the lock: the callback rotates the recording, which is file
        # I/O and calls `reset()` — both of which would deadlock in here.
        if event is not None:
            self.on_ignition(event)

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
                    'catalyst': _first(v, *CATALYST_KEYS),
                    'fuel_rail_temp': v.get('fuel_rail_temp'),
                    'power_kw': v.get('power_kw'),
                    'oil': v.get('oil'),
                },
                'derived': {
                    'warm': warm,
                    'charge': charge,
                    # km/L, which is how fuel economy is quoted here. The
                    # score still works in L/100km internally, where its band
                    # is tuned — see metrics.instant_econ.
                    'econ_now': metrics.km_per_l(
                        metrics.instant_econ(v.get('speed'), v.get('fuel_rate'))),
                    'econ_avg': self.trip.econ_km_per_l,
                    'dist_km': self.trip.dist_km,
                    'fuel_l': self.trip.fuel_l,
                    'cost_rm': self.trip.cost_rm,
                    'elapsed_s': self.trip.elapsed_s,
                    'moving_s': self.trip.moving_s,
                    'avg_speed': self.trip.avg_speed_kph,
                    'peak_rpm': self.peak_rpm,
                    'peak_kw': self.peak_kw,
                    # None until the channel reports — see `peak_rpm`
                    'peak_speed': self.peaks.get('speed'),
                    'peak_coolant': self.peaks.get('coolant'),
                    'peak_intake': self.peaks.get('intake'),
                    'peak_catalyst': self.peaks.get('catalyst'),
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
                'file': self.current_file,
                'replay': self._replay_position(),
                # the durations travel with the phase so the page can run the
                # same sequence off its own clock on a replay refresh
                'boot': {'phase': self.boot.phase,
                         'progress': self.boot.progress,
                         'ms': BOOT_MS, 'fade_ms': BOOT_FADE_MS},
            }

    def _replay_position(self):
        """Where along the logged drive we are, or None when this is live.

        Both figures are in the capture's own seconds — the time the drive
        really took — so a replay running at 8x still reports an honest
        position. Called with the lock held.
        """
        src = self.replay
        if src is None:
            return None
        dur = getattr(src, 'duration', 0.0) or 0.0
        return {
            # the clock: where along the drive the data actually is
            'pos': min(getattr(src, 'pos', 0.0) or 0.0, dur),
            'dur': dur,
            # the bar: measured in samples, so a capture full of recording
            # holes has no dead stretches you cannot drag into
            'i': getattr(src, 'index', 0),
            'n': getattr(src, 'total', 0),
            'paused': bool(getattr(src, 'paused', False)),
        }
