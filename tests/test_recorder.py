"""Tests for the recording surviving a power cut.

The gauge is fed from an ignition-switched supply, so every drive in the car
ends by having its power removed mid-write — `close()` is never reached and the
`.json` summary it writes never happens. Without one, `library` can only count
rows, and the drive picker shows 'interrupted' instead of a distance. So the
summary has to be written as it goes.
   Run: .venv/bin/python tests/test_recorder.py"""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import library, recorder, state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def summary_of(g):
    snap = g.snapshot()
    return {'derived': snap['derived'], 'score': snap['score'],
            'supported_pids': snap.get('supported')}


def record(tmp, seconds, sidecar=True):
    """Drive for `seconds` of logical time into a fresh recorder."""
    g = state.Gauge()
    rec = recorder.Recorder(os.path.join(tmp, 'logs'))
    if sidecar:
        rec.summary_fn = lambda: summary_of(g)
    g.recorder = rec
    t = 0.0
    while t < seconds:
        g.sample('speed', 60.0, t)
        g.sample('rpm', 2200.0, t)
        g.sample('fuel_rate', 7.0, t)
        t += 1.0
    return g, rec


tmp = tempfile.mkdtemp(prefix='mx5-recorder-')
try:
    # -- the power cut -------------------------------------------------------
    # Drive, wait for the periodic write to come round, then walk away without
    # ever calling close() — which is exactly what pulling the fuse does.
    g, rec = record(tmp, 300.0)
    check('nothing is written before the interval elapses',
          os.path.exists(rec.path.replace('.csv', '.json')), False)

    time.sleep(recorder.SIDECAR_SECONDS + 0.2)
    g.sample('speed', 60.0, 301.0)          # the write rides on the next sample
    side = rec.path.replace('.csv', '.json')
    check('the summary is written as the drive goes', os.path.exists(side), True)

    entry = library._entry(rec.path)
    check('a cut drive still reports its kind', entry['kind'], 'drive')
    check('...and is not flagged interrupted', entry.get('partial'), None)
    check('...and quotes a real distance', round(entry['dist_km'] or 0), 5)
    check('...so the picker shows km rather than points',
          'km' in entry['summary'], True)

    # -- a clean close still works ------------------------------------------
    g, rec = record(tmp, 300.0)
    path = rec.close(summary=summary_of(g))
    check('closing cleanly returns the path', path is not None, True)
    library._cache.clear()
    entry = library._entry(path)
    check('...and the summary is complete', round(entry['dist_km'] or 0), 5)
    check('...and not flagged interrupted', entry.get('partial'), None)

    # -- without a summary_fn, nothing changes -------------------------------
    g, rec = record(tmp, 300.0, sidecar=False)
    time.sleep(recorder.SIDECAR_SECONDS + 0.2)
    g.sample('speed', 60.0, 301.0)
    check('a recorder with no summary source writes no sidecar',
          os.path.exists(rec.path.replace('.csv', '.json')), False)
    library._cache.clear()
    check('...and that drive does read as interrupted',
          library._entry(rec.path).get('partial'), True)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all recorder tests passed')
