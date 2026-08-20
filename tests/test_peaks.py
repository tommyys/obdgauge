"""Unit tests for the drive summary's high-water marks.

The summary card quotes the peaks of a finished drive: the highest it revved,
the fastest it went and how hot it ran. These are maxima, which makes them
quietly dangerous — a peak set by one bad frame stays pinned there for the
whole drive, and a peak missed by one dropped frame is invisible in every
other figure. So the rules worth pinning down are what counts as a reading,
what a missing channel reports, and that a new drive inherits nothing.
   Run: .venv/bin/python tests/test_peaks.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-56s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def feed(pairs):
    """A gauge fed `(key, value)` in order, one second apart."""
    g = state.Gauge()
    for i, (k, v) in enumerate(pairs):
        g.sample(k, v, float(i))
    return g


def derived(g):
    return g.snapshot()['derived']


# --- the ordinary case ------------------------------------------------------
g = feed([('rpm', 2200), ('rpm', 6300), ('rpm', 3100),
          ('speed', 40), ('speed', 112), ('speed', 0),
          ('coolant', 71), ('coolant', 94), ('coolant', 88),
          ('intake', 30), ('intake', 41)])
d = derived(g)
check('peak rpm is the highest, not the last', d['peak_rpm'], 6300)
check('peak speed is the highest, not the last', d['peak_speed'], 112)
check('peak coolant is the highest, not the last', d['peak_coolant'], 94)
check('peak intake is the highest, not the last', d['peak_intake'], 41)

# --- a channel the car never sent -------------------------------------------
# 0 would be a lie a bold number tells convincingly: it reads as a measured
# cold catalyst rather than as no catalyst sensor at all.
check('an absent channel has no peak, not a zero', d['peak_catalyst'], None)

# --- the catalyst family ----------------------------------------------------
# Cars report catalyst temperature on whichever bank and sensor they have.
# The drive only got as hot as it got, whichever channel happened to say so.
g = feed([('cat_b1s1', 410), ('cat_b2s1', 655), ('cat_b1s1', 480)])
check('peak catalyst spans the whole family', derived(g)['peak_catalyst'], 655)

g = feed([('catalyst', 500), ('cat_b1s2', 300)])
check('...and takes the max, not the last bank seen',
      derived(g)['peak_catalyst'], 500)

# --- implausible readings ---------------------------------------------------
# `plausible()` already drops these before anything else sees them; the point
# here is that peaks sit behind that gate and not in front of it. One garbage
# frame is a spike no average would notice but a maximum would enshrine.
g = feed([('rpm', 3000), ('rpm', 60000), ('coolant', 90), ('coolant', 9000)])
d = derived(g)
check('a wild rpm frame cannot pin the peak', d['peak_rpm'], 3000)
check('nor a wild coolant frame', d['peak_coolant'], 90)

# --- a new drive starts from nothing ----------------------------------------
# reset() runs when another drive is loaded and on every ignition rotation.
# Peaks carried across would credit this drive with the last one's redline.
g = feed([('rpm', 6800), ('speed', 130), ('coolant', 99), ('cat_b1s1', 700)])
g.reset()
d = derived(g)
check('reset clears the rev peak', d['peak_rpm'], 0.0)
check('reset clears the speed peak', d['peak_speed'], None)
check('reset clears the thermal peaks',
      (d['peak_coolant'], d['peak_intake'], d['peak_catalyst']),
      (None, None, None))

# --- before any sample at all -----------------------------------------------
# peak_rpm is the one that answers 0.0: the tacho and the score footer draw it
# unconditionally, and have always been given a number to draw.
d = derived(state.Gauge())
check('a fresh gauge reports 0 revs', d['peak_rpm'], 0.0)
check('and nothing for the rest',
      (d['peak_speed'], d['peak_coolant'], d['peak_intake'],
       d['peak_catalyst']),
      (None, None, None, None))

print('')
if FAILED:
    for f in FAILED:
        print('FAILED: %s' % f)
    sys.exit(1)
print('all peak tests passed')
