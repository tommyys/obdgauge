"""Unit tests for the mounting-angle solver.

The claim under test is the one the whole g view rests on: the gauge can work
out which way it is pointing from the drive alone, with nobody measuring the
bracket. Two halves to it -- find down from gravity, then find forward from
the fact that road speed only changes when the car accelerates or brakes.

Most of this runs on synthetic drives, where the true answer is known and can
be compared against. The last block replays the real 2026-08-29 mounted drive,
because a solver that only works on maths it was handed is not evidence.
   Run: .venv/bin/python tests/test_gforce.py"""
import csv
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import gforce  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REAL_DRIVE = os.path.join(HERE, os.pardir, 'logs', 'drive-20260829-211900.csv')

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def near(name, got, want, tol):
    ok = got is not None and abs(got - want) <= tol
    if not ok:
        FAILED.append('%s: got %r want ~%r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def angle_between(a, b):
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(d))


# --- a synthetic car -------------------------------------------------------
# The gauge is bolted on at a silly angle on purpose: if the solver only works
# for a tidy mounting it is worthless, because nobody mounts anything tidily.
# Down, forward and right are a right-handed set in the car's own axes; the
# rotation below is what the accelerometer would actually report.
def rot(v, yaw, pitch):
    cy, sy = math.cos(yaw), math.sin(yaw)
    x, y, z = v
    x, y = x * cy - y * sy, x * sy + y * cy
    cp, sp = math.cos(pitch), math.sin(pitch)
    y, z = y * cp - z * sp, y * sp + z * cp
    return (x, y, z)


YAW, PITCH = 0.7, -0.4
DOWN = rot((0.0, 0.0, 1.0), YAW, PITCH)          # gravity, in gauge axes
FWD = rot((1.0, 0.0, 0.0), YAW, PITCH)           # forward, in gauge axes
RIGHT = rot((0.0, 1.0, 0.0), YAW, PITCH)


def synth(events, dt=0.1):
    """Drive the solver with a list of (seconds, accel_g, lateral_g).

    `accel_g` is positive when the car speeds up. Speed is integrated from it,
    so the OBD channel and the accelerometer agree the way they do in a car --
    which is the only reason the solver can work at all.
    """
    g = gforce.GForce()
    t, speed = 0.0, 0.0
    for secs, a, lat in events:
        n = int(secs / dt)
        for _ in range(n):
            t += dt
            speed += a * 9.81 * dt * 3.6
            speed = max(0.0, speed)
            # gravity + forward push + sideways push, as the part would see it
            s = tuple(DOWN[i] + FWD[i] * a + RIGHT[i] * lat for i in range(3))
            g.update(t, s[0], s[1], s[2])
            g.speed(t, round(speed))       # OBD speed is whole km/h
    return g


# Sixty seconds of gentle cruising teaches it down but never forward: nothing
# in it distinguishes the two flat directions.
cruise = synth([(60, 0.0, 0.0)])
check('cruising alone never finds forward', cruise.ready, False)
near('...but it does find down', angle_between(cruise.down, DOWN), 0.0, 1.0)
near('...and the mount check reads 1.000 g', cruise.down_g, 1.0, 0.01)

# Add some accelerating and braking and it has what it needs.
drive = synth([(20, 0.0, 0.0)] + [(6, 0.25, 0.0), (6, -0.25, 0.0)] * 6)
check('accelerating and braking finds forward', drive.ready, True)
near('forward is found to within a few degrees',
     angle_between(drive.fwd, FWD), 0.0, 6.0)
near('right falls out of it', angle_between(drive.right, RIGHT), 0.0, 6.0)

# Sign conventions, which are what make the dot move the way a driver expects.
brake = synth([(20, 0.0, 0.0)] + [(6, 0.25, 0.0), (6, -0.25, 0.0)] * 6
              + [(3, -0.4, 0.0)])
check('braking reads as positive longitudinal g', brake.lon > 0.3, True)
accel = synth([(20, 0.0, 0.0)] + [(6, 0.25, 0.0), (6, -0.25, 0.0)] * 6
              + [(3, 0.4, 0.0)])
check('accelerating reads as negative', accel.lon < -0.3, True)
right = synth([(20, 0.0, 0.0)] + [(6, 0.25, 0.0), (6, -0.25, 0.0)] * 6
              + [(3, 0.0, 0.5)])
check('a right-hand turn reads as positive lateral g', right.lat > 0.4, True)

# Cornering must not leak into braking. A car held in a long constant-radius
# corner is not decelerating, and a solver that says it is would score every
# roundabout as a harsh stop.
corner = synth([(20, 0.0, 0.0)] + [(6, 0.25, 0.0), (6, -0.25, 0.0)] * 6
               + [(6, 0.0, 0.5)])
near('a steady corner leaks no longitudinal g', corner.lon, 0.0, 0.08)

# --- persistence -----------------------------------------------------------
# A bracket does not move between drives. The six minutes of learning is paid
# once, not every time the key is turned.
saved = drive.export_axes()
fresh = gforce.GForce()
fresh.restore_axes(saved)
check('a restored gauge is ready immediately', fresh.ready, True)
near('and points the same way', angle_between(fresh.fwd, drive.fwd), 0.0, 0.01)
junk = gforce.GForce()
junk.restore_axes({'fwd': [1, 1, 1], 'down': [0, 0, 1]})
check('a corrupt saved axis is refused, not trusted', junk.ready, False)
junk2 = gforce.GForce()
junk2.restore_axes(None)
check('nothing saved yet is not an error', junk2.ready, False)

# --- the real drive --------------------------------------------------------
# logs/drive-20260829-211900.csv is the first drive recorded with the gauge
# properly mounted: 16.4 minutes, 14,260 accelerometer records. Nobody
# measured the bracket, so there is no ground truth to compare against -- what
# is checkable is that the answer is self-consistent and physically sane.
if os.path.exists(REAL_DRIVE):
    g = gforce.GForce()
    pend = {}
    for row in csv.reader(open(REAL_DRIVE)):
        if row[0] == 'iso':
            continue
        try:
            t, v = float(row[1]), float(row[3])
        except ValueError:
            continue
        if row[2] == 'speed':
            g.speed(t, v)
        elif row[2] in ('imu_ax', 'imu_ay', 'imu_az'):
            pend[row[2]] = v
            if len(pend) == 3:
                g.update(t, pend['imu_ax'], pend['imu_ay'], pend['imu_az'])
                pend = {}
    check('the real drive solves', g.ready, True)
    # The install check. 1.000 means the gauge did not move on its mount all
    # drive; the earlier 11:24 drive of the same day reads 0.890 because it
    # was loose, and that is how we know which drive to trust.
    near('the mount reads 1.00 g, so it never moved', g.down_g, 1.0, 0.05)
    # Independently derived: correlating each raw axis against the speed
    # derivative puts longitudinal on Z (-0.357) and lateral on Y (+0.400 vs
    # yaw rate). The solver must agree, which means forward lies almost along
    # -Z and right almost along +Y.
    check('forward comes out along -Z, as the correlation said',
          g.fwd[2] < -0.8, True)
    check('right comes out along +Y, as the correlation said',
          g.right[1] > 0.8, True)
    check('the two axes are perpendicular',
          abs(sum(a * b for a, b in zip(g.fwd, g.right))) < 1e-6, True)
    # Peaks a road car can actually produce. Anything past ~1.1 g on road
    # tyres is the gauge being knocked, not the car.
    check('peak braking is plausible for a road drive',
          0.1 < g.peak_lon_brake < 1.1, True)
    check('peak cornering is plausible for a road drive',
          0.1 < g.peak_lat < 1.1, True)
else:
    print('(real drive log absent -- skipped)')

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all gforce tests passed')
