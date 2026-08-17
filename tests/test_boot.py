"""Unit tests for the boot splash phase.

The tracker is told the time rather than reading a clock, so every test drives
it with explicit timestamps. A replay at 8x and a car idling in front of you
must produce identical transitions.
   Run: .venv/bin/python tests/test_boot.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def near(name, got, want, tol=1e-6):
    ok = got is not None and abs(got - want) <= tol
    if not ok:
        FAILED.append('%s: got %r want ~%r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


SPLASH = state.BOOT_MS / 1000.0
FADE = state.BOOT_FADE_MS / 1000.0
WHOLE = SPLASH + FADE

# -- the splash ---------------------------------------------------------------

b = state.Boot()
check('starts waking', b.phase, 'WAKING')
near('...with nothing drawn yet', b.progress, 0.0)

b.update(100.0)
check('the first reading starts the splash', b.phase, 'WAKING')
near('...still at the beginning', b.progress, 0.0)

b.update(100.0 + SPLASH / 2)
check('half way through it is still waking', b.phase, 'WAKING')
near('...and says so', b.progress, 0.5, tol=0.02)

b.update(100.0 + SPLASH + 0.01)
check('the clip ends and it dips to black', b.phase, 'FADING')
near('...at the start of the dip', b.progress, 0.0, tol=0.03)

b.update(100.0 + SPLASH + FADE / 2)
check('mid-dip it is still fading', b.phase, 'FADING')
near('...and says how far through', b.progress, 0.5, tol=0.03)

b.update(100.0 + WHOLE + 0.01)
check('then it hands over to the instruments', b.phase, 'RUNNING')
near('running reports a finished animation', b.progress, 1.0)

b.update(100.0 + WHOLE + 60.0)
check('and it stays running', b.phase, 'RUNNING')

# -- the drive's own clock, not ours ------------------------------------------
# A capture replayed at 8x hands over timestamps 8x apart. The splash must last
# BOOT_MS of the *drive*, which is what makes live and replay behave alike.

b = state.Boot()
b.update(0.0)
b.update(SPLASH - 0.01)
check('a fraction short of the clip ending is still waking', b.phase, 'WAKING')

# -- scrubbing ----------------------------------------------------------------

b = state.Boot()
b.update(500.0)
check('waking after opening a drive part-way in', b.phase, 'WAKING')
b.update(10.0)                        # dragged backwards on the timeline
check('time running backwards leaves it in a real phase',
      b.phase in ('WAKING', 'RUNNING'), True)
check('...and progress stays in range',
      0.0 <= b.progress <= 1.0, True)
b.update(10.0 + WHOLE + 0.01)
check('...and it can still finish', b.phase, 'RUNNING')

# -- reset --------------------------------------------------------------------

b = state.Boot()
b.update(0.0)
b.update(WHOLE + 0.01)
check('running before the reset', b.phase, 'RUNNING')
b.reset()
check('reset splashes again', b.phase, 'WAKING')
near('...from the beginning', b.progress, 0.0)

# -- driven through the Gauge, the way the app does it ------------------------

g = state.Gauge()
check('a fresh gauge is waking', g.boot.phase, 'WAKING')
check('the snapshot carries the phase',
      g.snapshot()['boot']['phase'], 'WAKING')

g.sample('_car', {'label': 'MX-5'})
check('metadata alone does not start the splash',
      g.snapshot()['boot']['progress'], 0.0)

g.sample('rpm', 850.0, 0.0)
check('a real reading starts it', g.boot.phase, 'WAKING')
g.sample('speed', 40.0, SPLASH + 0.1)
check('any channel can carry it into the dip', g.boot.phase, 'FADING')
g.sample('speed', 40.0, WHOLE + 0.1)
check('...and over the line', g.boot.phase, 'RUNNING')
near('the snapshot follows', g.snapshot()['boot']['progress'], 1.0)

g.reset()
check('loading another drive splashes again', g.boot.phase, 'WAKING')

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all boot tests passed')
