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


# --- a synthetic g drive ---------------------------------------------------
# The band tests need known lateral and longitudinal g. They get it the same
# way the car does -- through the real solver, with the mounting angle handed
# to it up front -- rather than through a stub only the tests use. The C++
# suite runs the identical helper, so the two report the same numbers and a
# divergence in either port shows up here rather than on the road.
def g_drive(pattern, steps, pole, step=0.25):
    """Replay a repeating [(lat, lon), ...] pattern with the pole forced.

    Forcing the pole keeps these cases about the bands. Whether the intensity
    latch picks the right pole is tested separately, below."""
    down, fwd = (0.0, 0.0, 1.0), (1.0, 0.0, 0.0)
    right = (0.0, 1.0, 0.0)                       # = down x fwd
    sc = metrics.DrivingScore()
    sc.g.restore_axes({'fwd': list(fwd), 'down': list(down), 'weight': 1.0})
    sc.update(0.0, 30.0, 2000, 20.0, 6.0)
    t = 0.0
    # Settle the gravity average on level ground first. The solver seeds it
    # from the very first sample, so starting a case with a held 0.5 g would
    # teach it that 0.5 g is which way down is -- and the residual, which is
    # what braking is measured from, would come out as zero. A car boots
    # parked, so this is what actually happens; the test has to do it too.
    for _ in range(400):
        t += step
        sc.imu(t, down[0], down[1], down[2])
        sc.update(t, 30.0, 2000, 20.0, 6.0)
    sc.thr_s = sc.thr_bad = 0.0
    sc.brake_s = sc.brake_bad = 0.0
    sc.corner_s = sc.corner_bad = 0.0
    for i in range(steps):
        lat, lon = pattern[i % len(pattern)]
        t += step
        # lon is positive under braking, which is a backwards push.
        sc.imu(t, down[0] - fwd[0] * lon + right[0] * lat,
                  down[1] - fwd[1] * lon + right[1] * lat,
                  down[2] - fwd[2] * lon + right[2] * lat)
        sc.pole = pole
        sc.update(t, 30.0, 2000, 20.0, 6.0)
        sc.pole = pole
    return sc


class NoG(object):
    """A gauge whose axes were never solved, for the honesty cases."""
    ready = False
    lat = lon = 0.0
    peak_lat = peak_lon_brake = peak_lon_accel = 0.0
    total = 0.0

    def update(self, *a):
        pass

    def speed(self, *a):
        pass

    def snapshot(self):
        return {'ready': False}


# --- the bug this file exists for -----------------------------------------
# The score is fed on every sample of *any* channel. Its rates must therefore
# be measured against each channel's own clock, or a car that reports more
# PIDs scores differently for identical driving. This used to inflate throttle
# and acceleration rates roughly 4x on a real 35-channel drive.
steady = drive([50] * 30, throttle=[20.0] * 30)
near('steady throttle -> throttle 100', steady.throttle, 100.0)
steady_dense = drive([50] * 30, filler_per_step=10, throttle=[20.0] * 30)
near('steady throttle, densely sampled -> still 100',
     steady_dense.throttle, 100.0)

saw = [20.0 if i % 2 else 40.0 for i in range(40)]       # 20% every 0.33s
a = drive([50] * 40, throttle=saw)
b = drive([50] * 40, filler_per_step=9, throttle=saw)
check('jerky throttle scores the same at both sample rates',
      a.throttle is not None and b.throttle is not None
      and abs(a.throttle - b.throttle) < 1.0, True)
check('jerky throttle scores worse than steady',
      a.throttle < steady.throttle, True)

# --- ties: two channels in the same millisecond ---------------------------
# Timestamps are written to 3 decimals, so a fast sweep produces exact ties.
# Treating a tie as a gap used to drag the per-channel baseline forward while
# its value stayed put, shrinking the next real delta's dt.
sc = metrics.DrivingScore()
sc.update(0.000, 15.0, 2000, 20.0, 6.0)
for _ in range(6):
    sc.update(0.100, 15.0, 2000, 20.0, 6.0)     # six samples, identical stamp
sc.update(0.430, 13.0, 2000, 20.0, 6.0)
near('a tied timestamp costs no throttle demerit', sc.thr_bad, 0.0, tol=1e-9)

# --- gaps -----------------------------------------------------------------
# A pause in the recording is not the car teleporting.
sc = metrics.DrivingScore()
sc.update(0.0, 100.0, 3000, 50.0, 12.0)
sc.update(30.0, 0.0, 800, 0.0, 1.0)             # 30s gap, 100 -> 0 km/h
near('a 30s gap costs no demerit at all', sc.thr_bad, 0.0, tol=1e-9)

# --- throttle reversals ---------------------------------------------------
# Big inputs are fine. Changing your mind about them is not: on-off-on means
# you did not know what you wanted the car to do.
# Both of these move the pedal 5% every half second, so they earn exactly the
# same rate demerit. The only difference is direction, which is what isolates
# the reversal cost.
one_way = metrics.DrivingScore()
for i in range(21):                             # 0 -> 100% in one movement
    one_way.update(i * 0.5, 30.0, 3000, i * 5.0, 6.0)
check('a big single-direction input is not a reversal',
      [e for e in one_way.events if e[1] == 'reversal'], [])
flutter = metrics.DrivingScore()
for i in range(21):                             # +-5% about 45%
    flutter.update(i * 0.5, 30.0, 3000, 45.0 + (5.0 if i % 2 else -5.0), 6.0)
check('fluttering the pedal is caught as reversals',
      len([e for e in flutter.events if e[1] == 'reversal']) > 5, True)
check('a reversal costs throttle score',
      flutter.throttle < one_way.throttle, True)

# --- braking: Nice scores the size, Spirited scores the edges -------------
soft = g_drive([(0.0, 0.10)], 40, 'NICE')
firm = g_drive([(0.0, 0.50)], 40, 'NICE')
check('a nice segment marks down a hard stop',
      firm.braking < soft.braking, True)
near('0.5g held is past the nice band, so braking bottoms out',
     firm.braking, 0.0, tol=0.5)

# The same 0.5g, judged as a spirited segment: sustained is fine, snatched is
# not. This is the whole point of the two poles.
held = g_drive([(0.0, 0.5)], 40, 'SPIRITED')
snatch = g_drive([(0.0, 0.5), (0.0, 0.0)], 40, 'SPIRITED')
# Not exactly 100: over ten seconds of held braking the gravity average starts
# to absorb a little of it, and a slowly shrinking g is a small jerk. That is
# real rather than an artefact -- a car cannot hold 0.5 g for ten seconds
# without the reference drifting -- so the tolerance allows for it.
near('a spirited segment does not punish sustained 0.5g braking',
     held.braking, 100.0, tol=5.0)
check('but it does punish snatching the same 0.5g on and off',
      snatch.braking < held.braking - 20, True)
check('...and the nice band would have failed the sustained one',
      g_drive([(0.0, 0.5)], 40, 'NICE').braking < held.braking, True)

# --- cornering ------------------------------------------------------------
gentle_turn = g_drive([(0.15, 0.0)], 40, 'NICE')
hard_turn = g_drive([(0.60, 0.0)], 40, 'NICE')
check('a nice segment marks down a fast corner',
      hard_turn.cornering < gentle_turn.cornering, True)
smooth_fast = g_drive([(0.60, 0.0)], 40, 'SPIRITED')
sawing = g_drive([(0.60, 0.0), (0.25, 0.0)], 40, 'SPIRITED')
near('a spirited segment does not punish a steady 0.6g corner',
     smooth_fast.cornering, 100.0, tol=5.0)
check('but sawing at the wheel inside one is punished',
      sawing.cornering < smooth_fast.cornering - 20, True)

# --- missing channel means missing sub-score ------------------------------
# The same honesty rule as gauge::view_available. A convincing number for data
# we do not have is worse than an em-dash.
no_g = metrics.DrivingScore(gforce=NoG())
for i in range(60):
    no_g.update(i * 0.25, 40.0, 2500, 25.0, 6.0, coolant_c=90.0)
check('no g axes yet -> no braking score', no_g.braking, None)
check('no g axes yet -> no cornering score', no_g.cornering, None)
check('...but the score still exists on what is left',
      no_g.total is not None, True)
no_cool = metrics.DrivingScore(gforce=NoG())
for i in range(60):
    no_cool.update(i * 0.25, 40.0, 2500, 25.0, 6.0)
check('no coolant channel -> no care score', no_cool.care, None)
check('with only throttle left, the total is the throttle score',
      abs(no_cool.total - no_cool.throttle) < 1e-9, True)

# --- the pole -------------------------------------------------------------
# Intensity is a rolling read of the last half-minute, so a pole has to be
# earned over time, not by one loud sample.
calm = metrics.DrivingScore()
for i in range(600):                            # 150s of 2000rpm, 20% throttle
    calm.update(i * 0.25, 40.0, 2000, 20.0, 6.0)
check('a gentle drive stays nice', calm.pole, 'NICE')

hot = metrics.DrivingScore()
for i in range(600):                            # 150s of 6000rpm, 80% throttle
    hot.update(i * 0.25, 90.0, 6000, 80.0, 20.0)
check('a hard drive earns the spirited pole', hot.pole, 'SPIRITED')
check('...and banks its seconds there',
      hot.spirited_s > 60 and hot.nice_s < hot.spirited_s, True)
check('a gentle drive banks none', calm.spirited_s, 0.0)

blip = metrics.DrivingScore()
for i in range(600):
    r, th = (6500, 90.0) if 100 <= i < 104 else (1800, 15.0)
    blip.update(i * 0.25, 40.0, r, th, 6.0)
check('one blip of throttle does not flip the pole', blip.pole, 'NICE')

# --- economy is out of the score -----------------------------------------
# B3's decision: a score that marks you down for enjoying the car is the thing
# it was opened to stop. km/L is a readout on the Trip view now.
thirsty = metrics.DrivingScore()
frugal = metrics.DrivingScore()
for i in range(200):
    thirsty.update(i * 0.25, 40.0, 2500, 25.0, 30.0, coolant_c=90.0)
    frugal.update(i * 0.25, 40.0, 2500, 25.0, 3.0, coolant_c=90.0)
check('fuel rate no longer moves the score',
      abs(thirsty.total - frugal.total) < 1e-9, True)

# --- mechanical care ------------------------------------------------------
cold_revs = metrics.DrivingScore()
warm_revs = metrics.DrivingScore()
for i in range(200):
    cold_revs.update(i * 0.25, 60.0, 5000, 60.0, 12.0, coolant_c=45.0)
    warm_revs.update(i * 0.25, 60.0, 5000, 60.0, 12.0, coolant_c=90.0)
check('revving a cold engine costs care', cold_revs.care < 50, True)
near('the same revs warm cost nothing', warm_revs.care, 100.0)
overheat = metrics.DrivingScore()
for i in range(200):
    overheat.update(i * 0.25, 60.0, 2000, 20.0, 8.0, coolant_c=110.0)
near('cooking the engine bottoms out care', overheat.care, 0.0, tol=0.5)

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
