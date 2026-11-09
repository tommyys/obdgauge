"""One drive, read back afterwards: what happened, and what to do next time.

The gauge itself can only ever say what is happening *now* -- a driver reading
a number mid-corner is a driver not looking at the road, which is why the g
view was stripped back to one word (gauge_ui/gball.cpp). This is the other
half of that decision: everything worth knowing about a drive, in one page,
read at a desk with the engine off.

Pure and no I/O beyond reading the CSV, so it is testable on its own and the
server route below it is three lines. The numbers come from replaying the
drive through the same DrivingScore and GForce the gauge ran live -- not from
a second, parallel idea of what a good drive is, which would drift away from
the gauge's own within a month.

The lessons are rules over measured quantities, and every one of them quotes
the number that fired it. A coaching line without its evidence is an opinion,
and the driver cannot tell an opinion from a measurement once it is on a
screen.
"""
import math
import os
import time

from . import gforce as _gforce
from . import library as _library
from . import metrics as _metrics
from . import recorder as _recorder
from . import vehicle as _vehicle

# The channels the score wants, carried forward between samples the way the
# gauge carries them: it is fed every sample of every channel with the last
# known value of the rest (see DrivingScore.update).
_CARRIED = ('speed', 'rpm', 'throttle', 'fuel_rate', 'coolant')
_IMU = ('imu_ax', 'imu_ay', 'imu_az')

# Rings on the g view, in g. Mirrors gauge_ui/gball.cpp's kRingG -- the point
# of the summary is to explain the thing the driver was looking at, so a
# different set of rings here would teach the wrong picture.
RING_G = (0.2, 0.4, 0.6, 0.8)
FULL_G = 1.0

# What the tyres will actually give on this car, near enough. Not a limit the
# gauge enforces -- it is the yardstick "how much of the car did you use" is
# measured against, and every number quoted as a percentage of grip uses it.
GRIP_G = 0.95

# A corner or a stop has to reach this to be counted as one, and two of them
# inside this many seconds are the same event.
HARD_CORNER_G = 0.5
HARD_BRAKE_G = 0.4
EVENT_GAP_S = 2.0

# --- stretches -------------------------------------------------------------
# A long drive is several drives: town, a highway, a mountain road, the
# highway home. Averaged together, the highway's quiet minutes dilute the
# mountain's numbers until neither means anything. So the drive is cut into
# slices, each slice is named by what the car was doing, and runs of the same
# kind are joined into stretches.
SLICE_S = 120.0
# Named in this order; the first that fits wins.
TWISTY_LAT_G = 0.35     # 95th-percentile cornering g in the slice...
TWISTY_MAX_KPH = 100.0  # ...on a road slower than a highway: fast highway
                        # bends and slip roads reach 0.4 g too
HIGHWAY_KPH = 95.0      # average while moving
TOWN_KPH = 40.0
# Moving for less than this share of the slice: parked, or a stop.
STOPPED_FRAC = 0.25
# A stretch shorter than this is folded into its neighbour. A two-minute
# bend on the highway is not a mountain road.
MIN_STRETCH_S = 240.0
# Trail braking: still braking while already cornering, both at once.
TRAIL_BRAKE_G = 0.2
TRAIL_LAT_G = 0.3
TRAIL_MIN_S = 0.5
KIND_TITLE = {'twisty': 'Twisty road', 'highway': 'Highway',
              'town': 'Town', 'road': 'Open road', 'stopped': 'Stopped'}


def _clock_s(t, start_epoch):
    """The short form of an instant, for a table cell: the clock time to
    the second when the file carries a date, else minutes into the drive."""
    if t is None:
        return None
    if start_epoch is None:
        return '%d min' % int(t // 60)
    return time.strftime('%H:%M:%S', time.localtime(start_epoch + t))


def _moment(t, start_epoch):
    """One instant in the drive, for a reader: the clock time to the second
    when the file carries a date, and always how far into the drive it was.
    The clock finds it in a dashcam clip; the minutes find it on the map."""
    if t is None:
        return None
    into = '%d min in' % int(t // 60)
    if start_epoch is None:
        return into
    return '%s (%s)' % (time.strftime('%H:%M:%S',
                                      time.localtime(start_epoch + t)), into)


def _peak_at(series):
    """(value, t) of the largest value in [(t, value)], or (None, None)."""
    best = max(series, key=lambda tv: tv[1], default=None)
    return (best[1], best[0]) if best else (None, None)


def _pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(q / 100.0 * len(s)))]


def _seconds_above(series, threshold, max_gap=2.0):
    """Seconds spent above a threshold, from (t, value) pairs.

    Integrated over each sample's own gap rather than counted as samples: the
    channels arrive at different rates, and counting samples would weight a
    3 Hz channel three times heavier than a 1 Hz one.
    """
    total = 0.0
    for i in range(1, len(series)):
        dt = series[i][0] - series[i - 1][0]
        if 0 < dt < max_gap and series[i][1] > threshold:
            total += dt
    return total


def _count_events(series, threshold, gap=EVENT_GAP_S):
    """How many separate times a value went over a threshold."""
    n, last = 0, None
    for t, v in series:
        if v >= threshold:
            if last is None or t - last > gap:
                n += 1
            last = t
    return n


def _distance_km(speed):
    """Distance from the speed trace, trapezoid between samples.

    Derived here rather than taken from a Trip: the summary is built from a
    file long after the drive, and nothing carried the odometer with it.
    """
    km = 0.0
    for i in range(1, len(speed)):
        dt = speed[i][0] - speed[i - 1][0]
        if 0 < dt < 5.0:
            km += (speed[i][1] + speed[i - 1][1]) / 2.0 * dt / 3600.0
    return km


def _slice_kind(speeds, lats):
    moving = [v for v in speeds if v > 3]
    if len(moving) < STOPPED_FRAC * len(speeds) or not moving:
        return 'stopped'
    mean = sum(moving) / len(moving)
    lat95 = _pct(lats, 95) or 0.0
    if lat95 >= TWISTY_LAT_G and mean < TWISTY_MAX_KPH:
        return 'twisty'
    if mean >= HIGHWAY_KPH:
        return 'highway'
    if mean < TOWN_KPH:
        return 'town'
    return 'road'


def _join(kinds, slice_s=SLICE_S, min_s=MIN_STRETCH_S):
    """[kind per slice] -> [[kind, first slice, last slice + 1]], with runs
    shorter than min_s folded into the longer of their neighbours."""
    runs = []
    for i, k in enumerate(kinds):
        if runs and runs[-1][0] == k:
            runs[-1][2] = i + 1
        else:
            runs.append([k, i, i + 1])
    while len(runs) > 1:
        short = [i for i, r in enumerate(runs) if (r[2] - r[1]) * slice_s < min_s]
        if not short:
            break
        # The shortest first, so one stray slice cannot split a long stretch.
        i = min(short, key=lambda j: runs[j][2] - runs[j][1])
        prev = runs[i - 1] if i > 0 else None
        nxt = runs[i + 1] if i + 1 < len(runs) else None
        into = prev if nxt is None or (prev is not None and
                                       prev[2] - prev[1] >= nxt[2] - nxt[1]) else nxt
        into[1], into[2] = min(into[1], runs[i][1]), max(into[2], runs[i][2])
        del runs[i]
        merged = []
        for r in runs:
            if merged and merged[-1][0] == r[0]:
                merged[-1][2] = r[2]
            else:
                merged.append(r)
        runs = merged
    return runs


def _trail_brakes(g_series):
    """Times when braking and cornering overlapped long enough to count."""
    out, start, last = [], None, None
    for t, lat, lon in g_series:
        if lon >= TRAIL_BRAKE_G and lat >= TRAIL_LAT_G:
            if start is None:
                start = t
            last = t
        elif start is not None:
            if last - start >= TRAIL_MIN_S:
                out.append(start)
            start = None
    if start is not None and last - start >= TRAIL_MIN_S:
        out.append(start)
    return out


def stretches(duration_s, speed, g_series, reversal_times, start_epoch=None):
    """Cut a drive into stretches and measure each one on its own.

    speed: [(t, kph)]. g_series: [(t, |lateral g|, longitudinal g)], braking
    positive. reversal_times: [t]. start_epoch names the clock times when the
    file carries a date; without one the stretches are labelled by minute.
    """
    n = max(1, int(math.ceil(duration_s / SLICE_S)))
    sp = [[] for _ in range(n)]
    la = [[] for _ in range(n)]
    for t, v in speed:
        sp[min(n - 1, int(t // SLICE_S))].append(v)
    for t, lat, _lon in g_series:
        la[min(n - 1, int(t // SLICE_S))].append(lat)
    runs = _join([_slice_kind(sp[i], la[i]) for i in range(n)])
    trails = _trail_brakes(g_series)
    out = []
    for kind, a, b in runs:
        t0, t1 = a * SLICE_S, min(b * SLICE_S, duration_s)
        inside = lambda t: t0 <= t < t1
        spd = [(t, v) for t, v in speed if inside(t)]
        lat = [(t, l) for t, l, _ in g_series if inside(t)]
        brk = [(t, lon) for t, _, lon in g_series if inside(t)]
        tot = [math.hypot(l, lon) for t, l, lon in g_series if inside(t)]
        moving = [v for _t, v in spd if v > 3]
        mins = max((t1 - t0) / 60.0, 1e-9)
        revs = sum(1 for t in reversal_times if inside(t))

        pc, pc_t = _peak_at(lat)
        pb, pb_t = _peak_at(brk)

        def clock(t):
            if start_epoch is None:
                return '%d min' % round(t / 60.0)
            return time.strftime('%H:%M', time.localtime(start_epoch + t))
        out.append({
            'kind': kind,
            'title': KIND_TITLE[kind],
            'from': clock(t0),
            'to': clock(t1),
            'minutes': mins,
            'dist_km': _distance_km(spd),
            'mean_moving_kph': (sum(moving) / len(moving)) if moving else None,
            'top_kph': max((v for _t, v in spd), default=None),
            'hard_corners': _count_events(lat, HARD_CORNER_G),
            'hard_brakes': _count_events(brk, HARD_BRAKE_G),
            'peak_corner': pc,
            'peak_corner_t': pc_t,
            'peak_corner_at': _moment(pc_t, start_epoch),
            'peak_corner_clock': _clock_s(pc_t, start_epoch),
            'peak_brake': pb,
            'peak_brake_t': pb_t,
            'peak_brake_at': _moment(pb_t, start_epoch),
            'peak_brake_clock': _clock_s(pb_t, start_epoch),
            'past_04_pct': (100.0 * sum(1 for x in tot if x >= 0.4) / len(tot))
                           if tot else None,
            'reversals': revs,
            'reversals_per_min': revs / mins,
            'trail_brakes': sum(1 for t in trails if inside(t)),
        })
    return out


def twisty_total(parts):
    """Every twisty stretch added together: the spirited driving on its own,
    with the highway and town left out. None when there was none."""
    tw = [p for p in parts if p['kind'] == 'twisty']
    if not tw:
        return None
    mins = sum(p['minutes'] for p in tw)
    revs = sum(p['reversals'] for p in tw)
    corner = max((p for p in tw if p['peak_corner'] is not None),
                 key=lambda p: p['peak_corner'], default=None)
    brake = max((p for p in tw if p['peak_brake'] is not None),
                key=lambda p: p['peak_brake'], default=None)
    return {
        'stretches': len(tw),
        'minutes': mins,
        'dist_km': sum(p['dist_km'] for p in tw),
        'hard_corners': sum(p['hard_corners'] for p in tw),
        'hard_brakes': sum(p['hard_brakes'] for p in tw),
        'peak_corner': corner and corner['peak_corner'],
        'peak_corner_at': corner and corner['peak_corner_at'],
        'peak_brake': brake and brake['peak_brake'],
        'peak_brake_at': brake and brake['peak_brake_at'],
        'past_04_pct': sum((p['past_04_pct'] or 0) * p['minutes'] for p in tw) / mins,
        'reversals': revs,
        'reversals_per_min': revs / mins,
        'trail_brakes': sum(p['trail_brakes'] for p in tw),
    }


def replay(path, profile=None):
    """Replay one recorded drive and return everything worth reporting."""
    rows = _recorder.load_csv(path)
    if not rows:
        return None
    profile = profile or _vehicle.DEFAULT_PROFILE
    redline = profile.get('rpm_red') or 6800

    score = _metrics.DrivingScore()
    carried = dict.fromkeys(_CARRIED)
    imu = dict.fromkeys(_IMU)
    speed, rpm, coolant = [], [], []
    lat_series, brake_series, totals = [], [], []
    g_series = []
    # The worst cold rev, by the same rule the care score uses: revs above
    # CARE_COLD_RPM while the coolant is under CARE_COLD_C. Kept so the
    # warm-up lesson can quote the moment itself; the care score rounds a
    # short one to "100 / 100", which is no evidence at all.
    cold = {'cold_rev_rpm': None, 'cold_rev_c': None, 'cold_rev_t': None,
            'cold_rev_s': 0.0}
    last_rpm_t = None
    # When the gauge's own peaks were set. Taken from GForce itself rather
    # than re-derived, so the time always belongs to the number shown.
    peak_corner_t = peak_brake_t = None

    for t, key, value in rows:
        if key in carried:
            carried[key] = value
        if key == 'speed':
            speed.append((t, value))
        elif key == 'rpm':
            rpm.append((t, value))
            c = carried['coolant']
            if c is not None and c < _metrics.CARE_COLD_C and \
                    value > _metrics.CARE_COLD_RPM:
                if last_rpm_t is not None and 0 < t - last_rpm_t < 2.0:
                    cold['cold_rev_s'] += t - last_rpm_t
                if cold['cold_rev_rpm'] is None or value > cold['cold_rev_rpm']:
                    cold.update(cold_rev_rpm=value, cold_rev_c=c, cold_rev_t=t)
            last_rpm_t = t
        elif key == 'coolant':
            coolant.append(value)
        if key in imu:
            imu[key] = value
            # The accelerometer's three axes arrive as three separate rows at
            # the same instant; z is written last, so that is the complete
            # sample. Feeding the score on a partial one would score a corner
            # that had only half happened.
            if key == 'imu_az':
                was_c, was_b = score.g.peak_lat, score.g.peak_lon_brake
                score.g.update(t, imu['imu_ax'], imu['imu_ay'], imu['imu_az'])
                if score.g.peak_lat > was_c:
                    peak_corner_t = t
                if score.g.peak_lon_brake > was_b:
                    peak_brake_t = t
                if score.g.ready:
                    lat_series.append((t, abs(score.g.lat)))
                    brake_series.append((t, score.g.lon))
                    totals.append(score.g.total)
                    g_series.append((t, abs(score.g.lat), score.g.lon))
            continue
        score.update(t, carried['speed'], carried['rpm'], carried['throttle'],
                     carried['fuel_rate'], carried['coolant'])

    snap = score.snapshot()
    duration_s = rows[-1][0]
    moving = [v for _t, v in speed if v > 3]
    n_tot = len(totals) or 1
    reversals = [e for e in score.events if e[1] == 'reversal']
    minutes = max(duration_s / 60.0, 1e-9)
    start_epoch = _library._time_from_name(path)

    out = {
        'name': os.path.basename(path),
        'duration_min': duration_s / 60.0,
        'dist_km': _distance_km(speed),
        'pace': {
            'top_kph': max((v for _t, v in speed), default=None),
            'p95_kph': _pct([v for _t, v in speed], 95),
            'mean_moving_kph': (sum(moving) / len(moving)) if moving else None,
            'min_over_120': _seconds_above(speed, 120) / 60.0,
            'min_over_150': _seconds_above(speed, 150) / 60.0,
            'rpm_max': max((v for _t, v in rpm), default=None),
            'rpm_p95': _pct([v for _t, v in rpm], 95),
            'min_over_5000': _seconds_above(rpm, 5000) / 60.0,
            'min_near_redline': _seconds_above(rpm, redline * 0.9) / 60.0,
            'redline': redline,
            'stopped_pct': 100.0 * (1 - len(moving) / len(speed)) if speed else None,
        },
        'grip': {
            'full_scale': FULL_G,
            'grip_limit': GRIP_G,
            'peak_corner': score.g.peak_lat,
            'peak_corner_t': peak_corner_t,
            'peak_corner_at': _moment(peak_corner_t, start_epoch),
            'peak_brake': score.g.peak_lon_brake,
            'peak_brake_t': peak_brake_t,
            'peak_brake_at': _moment(peak_brake_t, start_epoch),
            'peak_accel': score.g.peak_lon_accel,
            'mount_g': score.g.down_g,
            'corner_pct_of_grip': 100.0 * score.g.peak_lat / GRIP_G,
            'brake_pct_of_grip': 100.0 * score.g.peak_lon_brake / GRIP_G,
            'rings': [{'g': r, 'pct': 100.0 * sum(1 for x in totals if x >= r) / n_tot}
                      for r in RING_G],
        },
        'score': {
            'total': snap['total'],
            'throttle': snap['throttle'],
            'braking': snap['braking'],
            'cornering': snap['cornering'],
            'care': snap['care'],
            'pole': snap['pole'],
            'nice_min': snap['nice_s'] / 60.0,
            'spirited_min': snap['spirited_s'] / 60.0,
        },
        'events': {
            'hard_corners': _count_events(lat_series, HARD_CORNER_G),
            'hard_brakes': _count_events(brake_series, HARD_BRAKE_G),
            'reversals': len(reversals),
            'reversals_per_min': len(reversals) / minutes,
        },
        'engine': {
            'coolant_max': max(coolant) if coolant else None,
            'coolant_mean': (sum(coolant) / len(coolant)) if coolant else None,
            **cold,
        },
    }
    out['stretches'] = stretches(
        duration_s, speed, g_series, [e[0] for e in reversals], start_epoch)
    out['twisty'] = twisty_total(out['stretches'])
    out['lessons'] = lessons(out)
    return out


def lessons(d):
    """The coaching, worst gap first. Each one carries the number behind it.

    Ranked by `gap`, a rough "how much is there to win here", so the driver
    reads the thing worth working on first rather than the thing that happened
    to be checked first. Five at most: a list longer than that is not a lesson,
    it is a report card nobody acts on.
    """
    out = []
    g, s, e, p = d['grip'], d['score'], d['events'], d['pace']

    if g['peak_brake'] and g['peak_corner'] and \
            g['peak_brake'] < 0.7 * g['peak_corner']:
        out.append({
            'gap': (g['peak_corner'] - g['peak_brake']) * 100,
            'title': 'Brake harder, and start braking later',
            'why': 'You corner much harder than you brake. The tyres will '
                   'stop the car at least as hard as they will turn it, so '
                   'the braking is where the easy time is.',
            'evidence': 'hardest stop %.2f g against %.2f g in the corners '
                        '-- %s past %.1f g in %.0f minutes'
                        % (g['peak_brake'], g['peak_corner'],
                           ('only %d stops' % e['hard_brakes'])
                           if e['hard_brakes'] else 'no stops',
                           HARD_BRAKE_G, d['duration_min']),
        })

    if e['reversals_per_min'] > 4.0:
        out.append({
            'gap': e['reversals_per_min'] * 6,
            'title': 'Settle the right foot',
            'why': 'A reversal is the throttle going on, off, and on again -- '
                   'changing your mind about an input you had already made. '
                   'It usually means arriving a little too fast and correcting '
                   'mid-corner. Decide before the corner, then hold it.',
            'evidence': '%d throttle reversals, %.1f a minute'
                        % (e['reversals'], e['reversals_per_min']),
        })

    if s['cornering'] is not None and s['cornering'] < 88:
        out.append({
            'gap': (88 - s['cornering']) * 2.5,
            'title': 'One steering input per corner',
            'why': 'The cornering mark falls when the wheel is added to and '
                   'taken off again mid-bend. Turn once, hold the arc, unwind '
                   'once.',
            'evidence': 'cornering %.0f / 100, %d corners past %.1f g'
                        % (s['cornering'], e['hard_corners'], HARD_CORNER_G),
        })

    if s['braking'] is not None and s['braking'] < 88:
        out.append({
            'gap': (88 - s['braking']) * 2.5,
            'title': 'Squeeze the brake, then bleed it off',
            'why': 'The braking mark is about the edges of the stop, not its '
                   'size. Press in progressively and release progressively; '
                   'the car stays settled and turns in better for it.',
            'evidence': 'braking %.0f / 100, hardest stop %.2f g'
                        % (s['braking'], g['peak_brake']),
        })

    if p['min_near_redline'] is not None and p['min_over_5000'] < \
            0.1 * d['duration_min']:
        out.append({
            'gap': 20,
            'title': 'Use the top of the rev range',
            'why': 'The engine makes its power near the red line and holding a '
                   'gear longer means one fewer shift mid-corner. On a '
                   'mountain road that is worth more than an early upshift.',
            'evidence': '%.1f min above 5000 rpm out of %.0f, peak %.0f rpm '
                        'against a %.0f red line'
                        % (p['min_over_5000'], d['duration_min'],
                           p['rpm_max'] or 0, p['redline']),
        })

    if s['care'] is not None and s['care'] < 100:
        eng = d['engine']
        if eng.get('cold_rev_rpm'):
            evidence = ('%.0f rpm at %.0f C, %.0f s after start -- %.0f s '
                        'above %.0f rpm before the coolant reached %.0f C'
                        % (eng['cold_rev_rpm'], eng['cold_rev_c'],
                           eng['cold_rev_t'], eng['cold_rev_s'],
                           _metrics.CARE_COLD_RPM, _metrics.CARE_COLD_C))
        else:
            evidence = ('care %.1f / 100, coolant peaked %.0f C'
                        % (s['care'], eng['coolant_max'] or 0))
        out.append({
            'gap': (100 - s['care']) * 3,
            'title': 'Let it warm up first',
            'why': 'Revving a cold engine is the one thing that is wrong no '
                   'matter how tidily it is done. Keep it under 3500 rpm until '
                   'the coolant is past 80 C.',
            'evidence': evidence,
        })

    out.sort(key=lambda x: -x['gap'])
    out = out[:5]
    if not out:
        out.append({
            'gap': 0,
            'title': 'Nothing to fix on this one',
            'why': 'Every sub-score is in the clear. Go faster and see what '
                   'breaks first.',
            'evidence': 'total %.0f / 100' % (d['score']['total'] or 0),
        })
    return out
