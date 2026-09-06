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
import os

from . import gforce as _gforce
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

    for t, key, value in rows:
        if key in carried:
            carried[key] = value
        if key == 'speed':
            speed.append((t, value))
        elif key == 'rpm':
            rpm.append((t, value))
        elif key == 'coolant':
            coolant.append(value)
        if key in imu:
            imu[key] = value
            # The accelerometer's three axes arrive as three separate rows at
            # the same instant; z is written last, so that is the complete
            # sample. Feeding the score on a partial one would score a corner
            # that had only half happened.
            if key == 'imu_az':
                score.g.update(t, imu['imu_ax'], imu['imu_ay'], imu['imu_az'])
                if score.g.ready:
                    lat_series.append((t, abs(score.g.lat)))
                    brake_series.append((t, score.g.lon))
                    totals.append(score.g.total)
            continue
        score.update(t, carried['speed'], carried['rpm'], carried['throttle'],
                     carried['fuel_rate'], carried['coolant'])

    snap = score.snapshot()
    duration_s = rows[-1][0]
    moving = [v for _t, v in speed if v > 3]
    n_tot = len(totals) or 1
    reversals = [e for e in score.events if e[1] == 'reversal']
    minutes = max(duration_s / 60.0, 1e-9)

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
            'peak_brake': score.g.peak_lon_brake,
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
        },
    }
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
                        '-- only %d stops past %.1f g in %.0f minutes'
                        % (g['peak_brake'], g['peak_corner'],
                           e['hard_brakes'], HARD_BRAKE_G, d['duration_min']),
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
        out.append({
            'gap': (100 - s['care']) * 3,
            'title': 'Let it warm up first',
            'why': 'Revving a cold engine is the one thing that is wrong no '
                   'matter how tidily it is done. Keep it under 3500 rpm until '
                   'the coolant is past 80 C.',
            'evidence': 'care %.0f / 100, coolant peaked %.0f C'
                        % (s['care'], d['engine']['coolant_max'] or 0),
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
