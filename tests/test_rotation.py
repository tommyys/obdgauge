"""Tests for splitting a recording into drives on ignition.

Covers the half of the feature that touches disk: closing one drive with its
summary, throwing away fragments, and starting the next from zero.
   Run: .venv/bin/python tests/test_rotation.py"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import run  # noqa: E402
from mx5gauge import state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def drive(g, seconds, speed=60.0, t0=0.0):
    """Feed a plain steady drive, long enough to accumulate real distance."""
    t = t0
    while t < t0 + seconds:
        g.sample('speed', speed, t)
        g.sample('rpm', 2200.0, t)
        g.sample('fuel_rate', 7.0, t)
        t += 1.0


def fresh(tmp):
    g = state.Gauge()
    g.recorder = run.recorder.Recorder(os.path.join(tmp, 'logs'))
    return g


tmp = tempfile.mkdtemp(prefix='mx5-rotation-')
try:
    # -- a drive worth keeping ----------------------------------------------
    g = fresh(tmp)
    drive(g, 300.0)
    path = run.finish_drive(g, g.recorder, tmp, quiet=True)
    check('a real drive is saved', path is not None, True)
    check('...the CSV is on disk', os.path.exists(path), True)
    side = path.replace('.csv', '.json')
    check('...with its summary beside it', os.path.exists(side), True)
    with open(side) as fh:
        meta = json.load(fh)
    check('...and the summary carries the distance',
          round(meta['derived']['dist_km']), 5)

    # -- a fragment ----------------------------------------------------------
    g = fresh(tmp)
    drive(g, 20.0)
    frag = g.recorder.path
    path = run.finish_drive(g, g.recorder, tmp, quiet=True)
    check('a 20s fragment is not saved', path, None)
    check('...its CSV is deleted', os.path.exists(frag), False)
    check('...and so is its summary',
          os.path.exists(frag.replace('.csv', '.json')), False)

    # -- rotation ------------------------------------------------------------
    g = fresh(tmp)
    g.on_ignition = run.on_ignition(g, tmp)
    drive(g, 300.0)
    first = g.recorder.path
    check('the first drive has covered ground',
          round(g.snapshot()['derived']['dist_km']), 5)

    g.on_ignition('on')
    check('rotating saved the first drive', os.path.exists(first), True)
    check('...and opened a different file', g.recorder.path != first, True)
    check('...with the trip totals back to zero',
          g.snapshot()['derived']['dist_km'], 0.0)

    drive(g, 300.0, t0=1000.0)
    second = g.recorder.path
    run.finish_drive(g, g.recorder, tmp, quiet=True)
    check('the second drive is a file of its own', os.path.exists(second), True)
    check('...and the two are distinct drives',
          len([n for n in os.listdir(os.path.join(tmp, 'logs'))
               if n.endswith('.csv')]), 3)

    # -- an ignition-off on its own does not rotate --------------------------
    g = fresh(tmp)
    g.on_ignition = run.on_ignition(g, tmp)
    drive(g, 120.0)
    held = g.recorder.path
    g.on_ignition('off')
    check('switching off alone does not close the drive',
          g.recorder.path, held)
    check('...and the trip totals are left alone',
          round(g.snapshot()['derived']['dist_km']), 2)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all rotation tests passed')
