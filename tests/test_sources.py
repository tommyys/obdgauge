"""Unit tests for replay seeking.

Seeking has one job: leave the gauge in exactly the state it would have been in
had you sat and watched the drive up to that moment. So every test here is the
same shape — seek to `t`, play straight through to `t`, and demand the two
agree. If they ever diverge, the summary card is quoting numbers the drive
never produced.
   Run: .venv/bin/python tests/test_sources.py"""
import csv
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import sources, state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-56s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def near(name, got, want, tol=1e-6):
    # a sub-score is legitimately None before there is enough data to compute
    # it, and "both still None" is a match, not a failure
    if got is None or want is None:
        ok = got is want
    else:
        ok = abs(got - want) <= tol
    if not ok:
        FAILED.append('%s: got %r want ~%r' % (name, got, want))
    print('%-56s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def write_csv(path):
    """A 60-second drive: accelerating away, cruising, then braking to a stop."""
    speeds = [0, 8, 19, 31, 44, 52, 58, 60, 61, 60, 60, 61, 60, 59, 60, 60,
              58, 47, 33, 18, 6, 0]
    with open(path, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['iso', 't', 'key', 'value'])
        for i, v in enumerate(speeds):
            t = i * 3.0
            w.writerow(['', '%.3f' % t, 'speed', v])
            w.writerow(['', '%.3f' % t, 'rpm', 900 + v * 55])
            w.writerow(['', '%.3f' % t, 'throttle', min(95.0, 5.0 + v)])
            w.writerow(['', '%.3f' % t, 'fuel_rate', 0.8 + v * 0.09])
    return path


def played_to(path, t_stop):
    """A gauge fed every row up to `t_stop` the plain way — the reference."""
    src = sources.ReplaySource(path, loop=False)
    g = state.Gauge()
    for ts, key, val in src.rows:
        if ts > t_stop:
            break
        g.sample(key, val, ts)
    return g


def sought_to(path, t):
    """A gauge the source seeked to `t`."""
    src = sources.ReplaySource(path, loop=False)
    g = state.Gauge()
    src.seek(t, g)
    return g, src


def compare(label, path, t):
    ref = played_to(path, t)
    got, src = sought_to(path, t)
    for field in ('dist_km', 'fuel_l', 'elapsed_s', 'moving_s'):
        near('%s: trip.%s' % (label, field),
             getattr(got.trip, field), getattr(ref.trip, field), tol=1e-9)
    for field in ('total', 'smooth', 'econ', 'calm', 'harsh'):
        near('%s: score.%s' % (label, field),
             getattr(got.score, field), getattr(ref.score, field), tol=1e-9)
    near('%s: peak rpm' % label, got.peak_rpm, ref.peak_rpm)
    check('%s: speed reading' % label,
          got.values.get('speed'), ref.values.get('speed'))
    return src


tmp = tempfile.mkdtemp()
CSV = write_csv(os.path.join(tmp, 'drive-20260814-120000.csv'))
src0 = sources.ReplaySource(CSV, loop=False)
DUR = src0.duration

near('duration is the capture timeline, not wall-clock', DUR, 63.0)
check('a fresh source sits at the start', src0.pos, 0.0)

# The three positions that matter: partway in, the very end (which is what
# clicking a drive does), and back to the start.
compare('mid-drive seek', CSV, 30.0)
compare('seek to the end', CSV, DUR)
compare('seek to zero', CSV, 0.0)

# Out-of-range seeks are clamped rather than refused: a scrub gesture on a
# round screen routinely overshoots both ends by a pixel or two.
_g, src = sought_to(CSV, -50.0)
near('a negative seek clamps to the start', src.pos, 0.0)
_g, src = sought_to(CSV, DUR + 500.0)
near('a seek past the end clamps to the end', src.pos, DUR)

# Seeking twice must not accumulate: the gauge is reset each time, so landing
# on 20 s from the end reads the same as landing on it from the start.
src = sources.ReplaySource(CSV, loop=False)
g = state.Gauge()
src.seek(DUR, g)
src.seek(20.0, g)
ref = played_to(CSV, 20.0)
near('seeking back from the end does not accumulate',
     g.trip.dist_km, ref.trip.dist_km, tol=1e-9)
near('...nor its fuel', g.trip.fuel_l, ref.trip.fuel_l, tol=1e-9)

# After a seek the paced loop must carry on from there, not from row 0.
src = sources.ReplaySource(CSV, loop=False)
src.seek(30.0, state.Gauge())
check('playback resumes after the seek point',
      all(ts >= 30.0 for ts, _k, _v in src.rows[src._cursor:]), True)
check('nothing before the seek point is left to play',
      src.rows[src._cursor][0] >= 30.0, True)

# The gauge's own metadata has to survive a seek, or the banner and the view
# gating blank out every time you scrub.
src = sources.ReplaySource(CSV, loop=False)
g = state.Gauge()
src.seek(15.0, g)
check('the car identity survives a seek', bool(g.values.get('_car')), True)
check('the channel list survives a seek',
      g.values.get('_supported_keys'), src.supported_keys)

# --- the bar is measured in samples, not seconds -------------------------
# A capture can span an hour and hold a minute of data. Seeking by fraction of
# the samples is what makes every part of the bar reach something real.
holey = os.path.join(tmp, 'drive-20260814-130000.csv')
with open(holey, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['iso', 't', 'key', 'value'])
    for i in range(50):                       # 50 samples in the first 50 s
        w.writerow(['', '%.3f' % i, 'speed', 40])
    for i in range(50):                       # then a 3000 s hole, 50 more
        w.writerow(['', '%.3f' % (3050 + i), 'speed', 40])

hs = sources.ReplaySource(holey, loop=False)
check('holey drive: 100 samples', hs.total, 100)
near('holey drive: spans ~3100 s', hs.duration, 3099.0, tol=1.0)

g = state.Gauge()
hs.seek_index(50, g)
near('halfway by sample lands at the hole edge', hs.pos, 49.0, tol=0.01)
check('...and has played exactly half the samples', hs.index, 50)

# The same halfway point measured in *seconds* lands in the hole, which is
# precisely the dead zone that made scrubbing feel broken.
hs.seek(hs.duration / 2.0, g)
near('halfway by clock falls in the hole, at its start', hs.pos, 49.0, tol=0.01)

marks = hs.marks(129)
check('marks span the whole drive', (marks[0], round(marks[-1])),
      (0.0, round(hs.duration)))
check('marks are one per requested step', len(marks), 129)
check('marks never go backwards',
      all(marks[i] <= marks[i + 1] for i in range(len(marks) - 1)), True)
# No mark may land inside the hole. That is the whole point: every position on
# the bar corresponds to a real sample, so there is nowhere to drag the handle
# that means nothing. Marks either side of the hole jump straight across it.
check('no mark falls inside the recording hole',
      [m for m in marks if 49.0 < m < 3050.0], [])
check('marks do cover both sides of it',
      (any(m <= 49.0 for m in marks), any(m >= 3050.0 for m in marks)),
      (True, True))

# fraction seeking is clamped the same way
check('frac beyond the end clamps', hs.seek_index(10 ** 6, g), 100)
check('negative index clamps', hs.seek_index(-5, g), 0)

# and a seek by index agrees with playing that many samples through
ref = state.Gauge()
for ts, key, val in hs.rows[:37]:
    ref.sample(key, val, ts)
got = state.Gauge()
hs.seek_index(37, got)
near('seek_index matches playing 37 samples', got.trip.dist_km,
     ref.trip.dist_km, tol=1e-9)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all source tests passed')
