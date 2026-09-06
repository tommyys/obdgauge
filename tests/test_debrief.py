"""The drive summary: the numbers, and the coaching rules over them.

Run: .venv/bin/python tests/test_debrief.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import debrief

fails = []


def check(name, got, want):
    ok = got == want
    print('%-46s %s   (%r)' % (name, 'ok  ' if ok else 'FAIL', got))
    if not ok:
        fails.append('%s: got %r, wanted %r' % (name, got, want))


def near(name, got, want, tol):
    ok = got is not None and abs(got - want) <= tol
    print('%-46s %s   (%r)' % (name, 'ok  ' if ok else 'FAIL', got))
    if not ok:
        fails.append('%s: got %r, wanted %r +/- %r' % (name, got, want, tol))


# --- the helpers, on data small enough to check by hand ---------------------
check('seconds above, integrated on its own clock',
      debrief._seconds_above([(0, 0), (1, 10), (2, 10), (3, 0)], 5), 2.0)
check('seconds above ignores a long gap',
      debrief._seconds_above([(0, 10), (99, 10)], 5), 0.0)
check('two crossings inside the gap are one event',
      debrief._count_events([(0, 1.0), (0.5, 1.0)], 0.5), 1)
check('two crossings outside the gap are two',
      debrief._count_events([(0, 1.0), (9, 1.0)], 0.5), 2)
check('distance from a steady 36 km/h for 100 s',
      round(debrief._distance_km([(t, 36.0) for t in range(101)]), 3), 1.0)

# --- the coaching rules -----------------------------------------------------
BASE = {
    'duration_min': 60.0,
    'grip': {'peak_corner': 0.5, 'peak_brake': 0.5, 'grip_limit': 0.95},
    'score': {'total': 95, 'throttle': 95, 'braking': 95, 'cornering': 95,
              'care': 100},
    'events': {'hard_corners': 10, 'hard_brakes': 10, 'reversals': 60,
               'reversals_per_min': 1.0},
    'pace': {'min_over_5000': 30.0, 'min_near_redline': 1.0, 'rpm_max': 6500,
             'redline': 6800},
    'engine': {'coolant_max': 95},
}


def with_(**over):
    d = {k: (dict(v) if isinstance(v, dict) else v) for k, v in BASE.items()}
    for key, value in over.items():
        head, _, field = key.partition('__')
        if field:
            d[head][field] = value
        else:
            d[head] = value
    return d


titles = lambda d: [x['title'] for x in debrief.lessons(d)]

check('a clean drive gets one line, not a list',
      titles(with_()), ['Nothing to fix on this one'])
check('braking far under cornering is called out',
      'Brake harder, and start braking later' in
      titles(with_(grip__peak_brake=0.2, grip__peak_corner=0.8)), True)
check('braking near cornering is not',
      'Brake harder, and start braking later' in
      titles(with_(grip__peak_brake=0.7, grip__peak_corner=0.8)), False)
check('a busy right foot is called out',
      'Settle the right foot' in
      titles(with_(events__reversals_per_min=9.0)), True)
check('cold revs are called out',
      'Let it warm up first' in titles(with_(score__care=60)), True)
check('lessons are capped at five',
      len(debrief.lessons(with_(
          grip__peak_brake=0.1, events__reversals_per_min=20,
          score__cornering=40, score__braking=40, score__care=10,
          pace__min_over_5000=0.0))) <= 5, True)

# The worst thing first: a rule cannot be useful if it is buried.
worst = debrief.lessons(with_(score__care=10, events__reversals_per_min=4.5))
check('the biggest gap is listed first', worst[0]['title'], 'Let it warm up first')
check('every lesson carries its evidence',
      all(x['evidence'] for x in worst), True)

# --- a real drive, end to end, if one has been pulled -----------------------
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
real = os.path.join(HERE, 'logs', 'drive-20260906-083304.csv')
if os.path.exists(real):
    d = debrief.replay(real)
    near('real drive: length in minutes', d['duration_min'], 67.7, 0.2)
    near('real drive: distance in km', d['dist_km'], 80.3, 1.0)
    near('real drive: score out of 100', d['score']['total'], 90.2, 0.5)
    near('real drive: hardest corner in g', d['grip']['peak_corner'], 0.77, 0.02)
    # The install check. Anything far off 1.000 means the numbers above are
    # measuring a gauge that moved on its bracket, not a car that moved.
    near('real drive: mount check', d['grip']['mount_g'], 1.000, 0.02)
    check('real drive: rings mirror the g view', [r['g'] for r in d['grip']['rings']],
          list(debrief.RING_G))
else:
    print('(no pulled drive in logs/ -- skipped the end-to-end check)')

print()
if fails:
    print('FAILURES:')
    for f in fails:
        print('  ' + f)
    sys.exit(1)
print('all debrief tests passed')
