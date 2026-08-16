"""Unit tests for ignition detection.

The detector's job is to tell three silences apart: the engine stopped, the
link dropped, and the poll loop merely stalled. Only the first is an ignition
event, and getting that wrong fragments a real drive into pieces — so most of
what is tested here is the cases that must NOT fire.
   Run: .venv/bin/python tests/test_ignition.py"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import ignition  # noqa: E402

FAILED = []
HERE = os.path.dirname(os.path.abspath(__file__))


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def running(ign, t0, seconds, volts=13.9, step=0.1):
    """Feed a normally running engine: fast PIDs plus the odd voltage read."""
    t = t0
    events = []
    while t < t0 + seconds:
        for key, val in (('rpm', 2200.0), ('speed', 60.0), ('throttle', 20.0)):
            events.append(ign.update(t, key, val))
            t += step
        if int(t - t0) % 5 == 0:
            events.append(ign.update(t, 'volts', volts))
    return [e for e in events if e]


def parked(ign, t0, seconds, volts=12.5, step=28.5):
    """Feed an ignition-off stretch: nothing but ATRV voltage reads."""
    t, events = t0, []
    while t < t0 + seconds:
        events.append(ign.update(t, 'volts', volts))
        t += step
    return [e for e in events if e]


# -- the clean case ----------------------------------------------------------

ign = ignition.Ignition()
check('a running engine raises nothing', running(ign, 0.0, 60.0), [])
check('...and is not believed to be off', ign.off, False)

evs = parked(ign, 60.0, 400.0)
check('silence with resting volts is an ignition-off', evs, ['off'])
check('...and it only fires once', evs.count('off'), 1)
check('...and the detector holds that belief', ign.off, True)

check('the first PID reply back is an ignition-on',
      [e for e in [ign.update(460.0, 'rpm', 780.0)] if e], ['on'])
check('...and the engine is believed running again', ign.off, False)


# -- silences that must not fire ---------------------------------------------

ign = ignition.Ignition()
running(ign, 0.0, 30.0)
# link dropped: no PIDs *and* no volts, because the adapter went with it
check('silence with no volts at all is a dead link, not ignition',
      [e for e in [ign.update(300.0, 'rpm', 780.0)] if e], [])

ign = ignition.Ignition()
running(ign, 0.0, 30.0)
check('silence at alternator voltage is a stalled poll, not ignition',
      parked(ign, 30.0, 200.0, volts=13.9), [])

ign = ignition.Ignition()
# A tired battery at idle can sag under the alternator floor while the engine
# is very much running. The PIDs still answering is what saves us.
check('a low reading with the PIDs still answering is not ignition',
      running(ign, 0.0, 60.0, volts=12.5), [])

ign = ignition.Ignition()
running(ign, 0.0, 30.0)
check('a brief gap under the silence threshold is not ignition',
      [e for e in [ign.update(35.0, 'volts', 12.5),
                   ign.update(35.5, 'rpm', 2200.0)] if e], [])


# -- run_time, the backstop edge ---------------------------------------------

ign = ignition.Ignition()
running(ign, 0.0, 10.0)
ign.update(10.0, 'run_time', 1245.0)
check('run_time climbing normally is silent',
      [e for e in [ign.update(11.0, 'run_time', 1246.0)] if e], [])
check('run_time going backwards is an ignition-on, with no off seen first',
      [e for e in [ign.update(12.0, 'run_time', 25.0)] if e], ['on'])

ign = ignition.Ignition()
check('the first run_time of a session is not a reset',
      [e for e in [ign.update(0.0, 'run_time', 500.0)] if e], [])


# -- the real drive ----------------------------------------------------------
# Cut from logs/drive-20260816-064348.csv around each edge: two genuine stops,
# at 06:59:19 and 07:27:24, with the volts-only parked stretches between them.

ign = ignition.Ignition()
seen = []
with open(os.path.join(HERE, 'fixtures', 'ignition-edges.csv')) as fh:
    for row in csv.DictReader(fh):
        try:
            val = float(row['value'])
        except ValueError:
            continue
        ev = ign.update(float(row['t']), row['key'], val)
        if ev:
            seen.append((ev, row['iso'][11:19]))

offs = [iso for ev, iso in seen if ev == 'off']
ons = [iso for ev, iso in seen if ev == 'on']


def secs(hms):
    h, m, s = hms.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


def after(name, got, truth, within):
    """The detected edge lands after the real one, but not long after."""
    lag = secs(got) - secs(truth) if got else None
    ok = lag is not None and 0 <= lag <= within
    if not ok:
        FAILED.append('%s: %s is %r s after %s' % (name, got, lag, truth))
    print('%-58s %s  (%s, +%.0fs)' % (name, 'ok ' if ok else 'FAIL', got,
                                      lag if lag is not None else -1))


check('the real drive contains exactly two ignition-offs', len(offs), 2)
check('the real drive contains exactly two restarts', len(ons), 2)
# The off-edge can only be noticed when a voltage read arrives, and with the
# engine stopped those are sparse and irregular — 27 s after the first stop,
# 74 s after the second. It does not matter that it is late: nothing rotates
# on the off-edge, it only arms the on-edge that does.
after('...the first off follows the real 06:59:19 stop',
      offs[0] if offs else None, '06:59:19', 90)
after('...the second follows the real 07:27:24 stop',
      offs[1] if len(offs) > 1 else None, '07:27:24', 90)
# The on-edge fires on the first PID reply, so it is prompt.
after('...the first restart is caught at once', ons[0] if ons else None,
      '07:06:32', 1)
after('...and so is the second', ons[1] if len(ons) > 1 else None,
      '07:32:43', 1)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all ignition tests passed')
