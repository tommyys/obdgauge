"""Unit tests for the derived metrics, especially the driving score's rates.

The score is fed on every sample of *any* channel, carrying the last-known
value of the rest, so its rates must depend on the car's physics and not on how
fast or how many channels happen to be polled.
   Run: .venv/bin/python tests/test_metrics.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import metrics  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-56s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def near(name, got, want, tol=0.51):
    ok = got is not None and abs(got - want) <= tol
    if not ok:
        FAILED.append('%s: got %r want ~%r' % (name, got, want))
    print('%-56s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def drive(speed_series, filler_per_step=0, throttle=None, rpm=2000, step=0.33):
    """Replay a speed series, optionally with `filler_per_step` extra samples
    of other channels between each speed reading — which is what a car with
    many supported PIDs actually produces."""
    sc = metrics.DrivingScore()
    t = 0.0
    for i, v in enumerate(speed_series):
        thr = throttle[i] if throttle else 20.0
        sc.update(t, v, rpm, thr, 6.0)
        for j in range(filler_per_step):
            # same values, later timestamp: a non-speed channel reporting in
            t += step / (filler_per_step + 1)
            sc.update(t, v, rpm, thr, 6.0)
        t += step / (filler_per_step + 1) if filler_per_step else step
    return sc


# --- the bug this file exists for -----------------------------------------
# A gentle 1 km/h-per-sample deceleration is not harsh driving. It used to be
# counted as harsh once other channels were interleaved, because the speed
# delta was divided by the gap to the last sample of ANY channel.
gentle = [40 - i for i in range(20)]                     # -0.84 m/s^2 per step
check('gentle decel, speed only          -> no events',
      drive(gentle).harsh, 0)
check('gentle decel, 4 other channels    -> still none',
      drive(gentle, filler_per_step=4).harsh, 0)
check('gentle decel, 20 other channels   -> still none',
      drive(gentle, filler_per_step=20).harsh, 0)

# a genuinely hard stop must still be caught, whatever else is being polled
hard = [60, 55, 48, 40, 31, 21, 10, 0]                   # ~-4 to -8 m/s^2
check('hard stop is caught, speed only',
      drive(hard).harsh > 0, True)
check('hard stop is caught with 20 channels interleaved',
      drive(hard, filler_per_step=20).harsh > 0, True)
check('and reports the same count either way',
      drive(hard).harsh == drive(hard, filler_per_step=20).harsh, True)

# --- ties: two channels in the same millisecond ---------------------------
# Timestamps are written to 3 decimals, so a fast sweep produces exact ties.
# Treating a tie as a gap used to drag the per-channel baseline forward while
# its value stayed put, shrinking the next real delta's dt.
sc = metrics.DrivingScore()
sc.update(0.000, 15.0, 2000, 20.0, 6.0)
for _ in range(6):
    sc.update(0.100, 15.0, 2000, 20.0, 6.0)     # six samples, identical stamp
sc.update(0.430, 13.0, 2000, 20.0, 6.0)         # -2 km/h over 0.43s = -1.3
check('a tied timestamp invents no harsh event', sc.harsh, 0)

# --- gaps -----------------------------------------------------------------
# A pause in the recording is not the car teleporting.
sc = metrics.DrivingScore()
sc.update(0.0, 100.0, 3000, 50.0, 12.0)
sc.update(30.0, 0.0, 800, 0.0, 1.0)             # 30s gap, 100 -> 0 km/h
check('a 30s gap invents no harsh event', sc.harsh, 0)

# --- smoothness is a rate over time, not a per-sample average ------------
steady = drive([50] * 30, throttle=[20.0] * 30)
near('steady throttle -> smooth 100', steady.smooth, 100.0)
steady_dense = drive([50] * 30, filler_per_step=10, throttle=[20.0] * 30)
near('steady throttle, densely sampled -> still 100', steady_dense.smooth, 100.0)

saw = [20.0 if i % 2 else 40.0 for i in range(40)]       # 20% every 0.33s
a = drive([50] * 40, throttle=saw)
b = drive([50] * 40, filler_per_step=9, throttle=saw)
check('jerky throttle scores the same at both sample rates',
      a.smooth is not None and b.smooth is not None
      and abs(a.smooth - b.smooth) < 1.0, True)
check('jerky throttle scores worse than steady', a.smooth < steady.smooth, True)

# --- trip accumulators ---------------------------------------------------
tr = metrics.Trip()
for i in range(101):                            # 100s at a steady 36 km/h
    tr.update(i * 1.0, 36.0, 7.2)
near('100s at 36km/h -> 1.0 km', tr.dist_km, 1.0, tol=0.02)
near('7.2 L/h for 100s -> 0.2 L', tr.fuel_l, 0.2, tol=0.01)
near('cost is litres x the pump price', tr.cost_rm, 0.2 * metrics.FUEL_PRICE_RM, tol=0.01)
near('avg speed', tr.avg_speed_kph, 36.0, tol=0.5)

tr = metrics.Trip()
tr.update(0.0, 100.0, 10.0)
tr.update(600.0, 100.0, 10.0)                   # 10-minute gap
check('a gap adds no distance', tr.dist_km, 0.0)

near('instant econ: 36km/h on 7.2L/h -> 20 L/100km',
     metrics.instant_econ(36.0, 7.2), 20.0, tol=0.01)
check('instant econ is None when stationary',
      metrics.instant_econ(0.0, 1.0), None)

# The display quotes km/L; the score keeps working in L/100km, where its band
# is tuned. The converter is the only place the two meet.
near('20 L/100km reads as 5 km/L', metrics.km_per_l(20.0), 5.0, tol=0.001)
near('5 L/100km reads as 20 km/L', metrics.km_per_l(5.0), 20.0, tol=0.001)
check('no reading converts to no reading', metrics.km_per_l(None), None)
# an engine on overrun cuts fuel entirely: a real reading whose reciprocal is
# infinite, so it is reported as absent rather than as a number or an Infinity
# the browser could not parse
check('zero consumption has no km/L to quote', metrics.km_per_l(0.0), None)
check('a negative reading is refused too', metrics.km_per_l(-3.0), None)

tr = metrics.Trip()
for i in range(101):                            # 100s at 36 km/h on 7.2 L/h
    tr.update(i * 1.0, 36.0, 7.2)
# 20 L/100km the other way up
near('trip average in km/L', tr.econ_km_per_l, 5.0, tol=0.05)
near('...is the reciprocal of the L/100km figure',
     tr.econ_km_per_l, 100.0 / tr.econ_l_per_100, tol=1e-9)

tr = metrics.Trip()
check('too little distance to quote km/L', tr.econ_km_per_l, None)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all metrics tests passed')
