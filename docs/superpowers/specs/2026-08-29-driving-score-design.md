# The driving score — design

**Date:** 2026-08-29
**Status:** decided and implemented
**Closes:** `SPEC.md` §7.5 **B3**, open since 2026-08-14
**Related:** `SPEC.md` §4 (the rpm backdrop), §6 (the views), §11

---

## 1. Why B3 was open

The old score blended three sub-scores — smoothness 0.40, economy 0.30, calm
0.30 — and every one of them rewarded driving gently. A good backroad drive
therefore scored as bad driving. B3 recorded the problem in one line: *"the
score only rewards calm/economical driving and silently punishes fun."*
Nothing in the score could be tuned until that was settled, because the
constants would have had to be re-tuned twice.

Two questions had to be answered, and they were answered on 2026-08-29:

| Question | Decision |
|---|---|
| What is a good drive? | **Doing whatever you were doing tidily.** Not doing it gently. |
| Is "spirited" a separate metric, a mode, or left out? | **A pole.** Two poles, one formula. |
| Who picks the pole? | **The gauge does, per segment**, from the data. No mode to set or forget. |
| How finely does it switch? | **A rolling 30 s window**, with a hold. Every second banks to the live pole. |
| Where does economy go? | **Out of the score entirely.** It stays a readout on the Trip view. |
| What does the view show? | **The g-ball** — the round screen is the traction circle. |

---

## 2. The model

**Two numbers, one shown.**

- **Intensity** — hidden. How hard the car is being driven right now, 0 to 1.
  It is never scored. Its only job is to pick which yardstick applies.
- **Cleanliness** — the 0–100 score on screen. How tidily you did whatever you
  were doing.

### Picking the pole

Intensity is the **loudest** of three signals, not their average: revs as a
fraction of redline, throttle opening, and total horizontal g. A car held at
6000 rpm through a long sweeper is spirited at a steady throttle, and a car
braked at 0.5 g is spirited at any rpm — averaging would hide both. That
figure is then smoothed over **30 s**.

- Below **0.35** → Nice. Above **0.55** → Spirited. The gap is deliberate: a
  single threshold would flutter every time a number sat on it.
- A pole must hold for **5 s** before it takes over. Same latch idea as the
  mood unit.
- Every second banks to whichever pole was live, so a drive ends with, say,
  34 min Nice and 6 min Spirited.

### One formula, two sets of bands

The weights never change. Only the thresholds do, which keeps one code path.

| Sub-score | Weight | Nice band | Spirited band |
|---|---|---|---|
| **Throttle** | 0.30 | full demerit at 12 %/s of pedal movement | full demerit at 40 %/s |
| **Braking** | 0.30 | full demerit at 0.25 g | full demerit at 1.5 g/s of **jerk**; any peak g allowed |
| **Cornering** | 0.25 | full demerit at 0.30 g | full demerit at 1.2 g/s of lateral jerk, above 0.20 g |
| **Care** | 0.15 | cold-engine revs, overheating | the same, and it is the only guard left on this pole |

A **throttle reversal** — the pedal turning round by more than 4 % — costs a
flat 0.5 s of demerit in both poles. Big inputs are fine; changing your mind
about them is not.

### Demerit-seconds

Every sub-score is measured in one currency. A sample contributes
`demerit × dt`, where demerit runs 0 to 1; an event contributes a fixed number
of seconds outright. A sub-score is `100 × (1 − demerit_s / observed_s)`.

This makes the maths **sample-rate independent by construction**, which is the
bug this class was rewritten around once already: rates measured against the
sample clock rather than each channel's own clock inflated acceleration ~4×
and turned every 1 km/h wiggle into a harsh event.

### Missing channel means missing sub-score

A car with no coolant channel has no **care** score, and the other three
renormalise. The same honesty rule as `gauge::view_available`. A convincing
number for data we do not have is worse than an em-dash.

---

## 3. Where the g comes from

The board's QMI8658 reports acceleration on three axes fixed to the **gauge**,
not to the car. Nothing knows which way the gauge points — it is stuck to a
dashboard by hand. `mx5gauge/gforce.py` and `gauge_core/gforce.cpp` work the
mounting out from the drive instead of being told.

**Which way is down.** Averaged over long enough, the only acceleration a car
sustains is gravity; corners and stops cancel. The slow average of the three
axes is therefore down, and its length is a free check on the install: it must
come out at 1.000 g.

**Which way is forward.** Gravity gives vertical, leaving a flat plane holding
forward and sideways with no way to tell them apart. The car breaks the tie —
road speed only changes when it accelerates or brakes, so the horizontal
direction that tracks the speed change is forward. Right falls out as
`down × forward`.

### Evidence from the real drive

`logs/drive-20260829-211900.csv` is the first drive with the gauge properly
mounted: 16.4 min, 14,260 accelerometer records.

| | 11:24 drive (loose) | 21:19 drive (mounted) |
|---|---|---|
| Gravity magnitude | 0.890 g — wrong | **1.026 g** |
| Noise, worst axis | 0.335 g rms | **0.108 g rms** |
| Longitudinal axis found | no | **Z**, correlation −0.357 with dv/dt |
| Lateral axis found | no | **Y**, correlation +0.400 with yaw rate |

The solver, given only the log, returns forward ≈ `(0.38, 0.04, −0.92)` and
right ≈ `(−0.15, 0.99, −0.03)` — which is exactly what an independent
correlation of the raw axes against the speed derivative says. **Nobody
measured the bracket.** This closes the other half of B3, which had been
waiting on "a moving car in the final mounting orientation".

### Tuning notes

- **`kMinSpeedDt = 0.9 s`.** OBD speed arrives in whole km/h, so over 0.33 s
  one count of rounding is 0.086 g of imaginary acceleration. Over 1 s the
  same count is worth a third of that, and several accelerometer samples fall
  inside the window to average.
- **`kForwardConfidence = 0.20`.** Locked at a range of values on the mounted
  drive and measured how far the axis then moved: 0.05 locks at 5.3 min and
  drifts 17°, 0.20 locks at 6.5 min and drifts 5.8°, 0.60 locks at 7.1 min and
  drifts 0.7°. The final direction is the same to two decimals at every
  threshold — the axis is a property of the bracket, not of the tuning.
- **`kPeakSaneG = 1.5`.** A road car on road tyres does not make 1.5 g; a
  gauge knocked with an elbow makes far more in one sample. The live dot still
  shows it — refusing would lie about what the part reported — but the drive's
  high-water marks must not be pinned by one knock.

### The angle is cached

Six minutes of learning is paid once, not every key cycle. The simulator
writes `logs/mount.json`; the board writes an NVS blob
(`firmware/main/mount_cache.cpp`), saved the moment the axes are first solved
and never per frame. Learning continues from the restored answer rather than
stopping at it, so a mount that really was moved corrects itself.

Until forward is found, the view says **LEARNING n%** rather than drawing a
dot. A guessed axis would draw a convincing cornering figure that is really a
bump.

---

## 4. The view: the g-ball

The screen is round, so it already **is** a traction circle.

- **The dot** is where the car is being pushed this instant. Left and right is
  cornering, up and down is braking — the way a driver's body feels it.
- **A fading trail**, about 3 s, draws the shape of the corner. A good one is
  an arc; a clumsy one is a scribble. You see the mistake as a shape rather
  than as a number that dropped.
- **Rings at 0.2 / 0.4 / 0.6 g, rim at 0.8 g.** The mounted drive sat 95 %
  under 0.24 g and peaked at 0.56 g braking, so 0.8 keeps ordinary driving
  visible in the middle instead of pinned to a dot in the centre. Only 0.4 g
  is labelled: labelling all three turns the middle of the screen into a
  table, labelling none leaves the rings as decoration.
- **Past the rim, the rim itself lights.** A dot you cannot see is the one
  moment you most want to know about.
- **Peak ghosts** mark the drive's hardest stop and hardest corner, left where
  they happened.
- **The circle is smaller than the screen** (radius 130 of 233). At full radius
  the dot under a hard stop climbs through the pole word at the top, and a
  hard stop is the one moment you must be able to read.

### The intensity signal, and why the two ports differ

The pole is shown as colour: deep green → ember → hot red, with the handover
at the middle so it sits near the pole threshold rather than at either end.

- **Simulator:** the existing **rpm backdrop** (`SPEC.md` §11), handed
  intensity instead of revs. Not a second tinted disc — the first cut drew its
  own, and the two fighting produced a flat orange floodlight with the score,
  the coach word and the ring labels washed out of it. The backdrop already
  keeps its colour at the rim and the centre black, which is exactly what a
  readable centre needs. Its opacity gets a floor of 0.42 here, because a
  gentle drive showing nothing is indistinguishable from a gauge that died.
- **Board:** the **rim ring** carries the same ramp. A full-screen backdrop is
  not an option on this board — it covers the panel, so every change repaints
  the panel, measured at 5–20 fps. That is the same reasoning that put the
  tacho's heat on the rim in `SPEC.md` §4. A ring is a small object, and
  intensity moves slowly, so the signal costs nothing. The colour is stepped
  onto the glow's own 40-step grid, so it repaints rarely.

### Rules the board view keeps

1. **No allocation on the draw path.** The trail is a fixed set of LVGL
   objects built once and moved, and its history is a fixed ring buffer of
   pixel positions. This is the panel DMA trap's rule; undoing it kills the
   display after the first swipe.
2. **A label is written only when its text changed.**
3. **The availability screen still applies.** A car with no rpm, speed or
   throttle has nothing to score.

---

## 5. The IMU sample rate

The recorder task was reading the part at **5 Hz** and writing every read to
flash. The score measures jerk, and at 5 Hz a whole hard stop is one or two
samples — not a rate of anything.

The read is now **20 Hz**, and only every fourth read reaches flash, so the
ring keeps the 5 Hz it already held. The recorder task is the only thing
allowed to touch the I2C bus, so it **publishes** the latest sample behind a
sequence counter and the UI loop reads that. A torn read is one skipped g
sample; blocking the draw path on the recorder's lock would be far worse.

In replay the board takes the accelerometer from the recorded drive instead —
feeding the real part in as well would hand the solver two clocks and a
stationary car.

---

## 6. What is verified, and what is not

**Verified.**

- `tests/test_gforce.py` and `firmware/test/host/test_gforce.cpp` are mirrors,
  and agree to five decimals (forward found to 1.079° on a synthetic drive at
  a deliberately untidy mounting angle; a steady corner leaks −0.0158 g
  longitudinally in both).
- `tests/test_metrics.py` and `test_metrics.cpp` are mirrors and agree on the
  band arithmetic to four decimals (96.5358 and 96.0235 on the two sustained
  cases, in both ports).
- `tools/verify_port.sh`: **0 divergences over 129,848 samples** on all six
  real logs, including both IMU drives, across 29 derived fields.
- The real mounted drive scores **81 TIDY**, entirely in the Nice pole — which
  is honest, because it never passed 3000 rpm.
- A synthetic hard drive built on the real axes scores **94 CLEAN** and banks
  as Spirited. That is B3's fix demonstrated: driving hard is no longer
  punished.
- The firmware builds.

**Not verified.**

- **No real spirited drive exists yet.** Nothing on file contains one, so that
  half of the score has never been fed real data. The bands are reasoned, not
  measured.
- **The board view has not been seen on the panel.** It compiles and follows
  the no-allocation rule, but nobody has looked at it.
- **The 20 Hz IMU read has not run in the car.** It costs four times the I2C
  traffic on the recorder task.
- **The mounting angle is one bracket's.** Move the gauge and the first drive
  after it re-learns; that path is tested only synthetically.

---

## 7. What the first two real drives showed (2026-08-31)

Two drives, `logs/drive-20260831-114800.csv` (10.6 min) and
`logs/drive-20260831-125224.csv` (5.6 min). Both dated by hand from times
Tommy supplied — the board had no clock, because `pull_drives.py --list` was
not run first (BOARD-CHECKS A7).

**Both were gentle.** Neither passed 2900 rpm or 67 km/h, so neither earned the
Spirited pole, and the Spirited half is still untested against real data.
Judged over the windows where the gauge was actually upright, they score
**80 TIDY** and **78 TIDY**. The solved forward axis came out as essentially
−Z on both, matching 2026-08-29 — the bracket is repeatable.

Three defects, in the order they were found.

**1. The IMU log rate went down, not up — fixed 2026-08-31.** The drives
recorded at 2.6 and 2.9 Hz, against 3.6–4.8 Hz before the change that was
meant to raise it. The recorder loop delays 50 ms, and the read had been given
a 50 ms interval of its own; a pass that came back a hair early failed
`now - last > 50 ms`, so it fired every *other* pass. 10 Hz of reads, a
quarter of them logged, 2.5 Hz on disk. The read now happens once per pass
with no interval of its own — the loop's delay *is* the sample rate — and the
flash write keeps a 200 ms deadline that **advances by one period** rather than
being reset to now, so it cannot beat the same way one level down.

**2. Moving the gauge silently corrupts the g — open.** Tommy lays the gauge
flat to park, and started the first drive with it flat. Gravity is re-learned
within 30 s, but **forward is not**: it stays in the old gauge frame and is
simply wrong. That produced 1.20 g of "braking" on a drive that never passed
67 km/h, pinned the drive's peaks, and pushed intensity to Spirited while
parked. `kPeakSaneG` at 1.5 was too generous to catch it.

*The fix, when it is wanted:* remember the down vector that forward was solved
against, and when the live down moves more than about 25° from it, drop
forward, go back to LEARNING, and stop reporting g. 25° clears hill pitch and
brake dive; laying the gauge flat is 70–90°. Deferred 2026-08-31 — Tommy will
mount it properly, and with a fixed mount it cannot happen.

**3. The yaw cross-check is specified but was never written — open.** Section
4 of this document, and the comment in `gball.h`, both say lateral g is
cross-checked against speed × yaw rate so a pothole is not scored as a corner.
No such code exists. The second drive's largest "corner" is **0.71 g at
19 km/h**, where the car cannot exceed 0.60 g even on full steering lock — the
gauge rocking on its mount after a bump, scored as bad cornering.

*The fix, when it is wanted:* the cheap half needs no gyro at all. Lateral g is
bounded by `v² / r_min`, and an MX-5 ND's tightest radius is about 4.7 m, so
speed alone says what is possible. On these drives that gate rejects 0 % of the
first and 2.0 % of the second, and takes that 0.71 g down to 0.47 g. The exact
version needs the gyro projected onto down, which means logging `imu_gx` and
`imu_gy` as well — only `imu_gz` is recorded today, and the gauge is not
mounted with Z vertical, so the logged channel is not yaw. Deferred
2026-08-31 for the same reason as 2.

## 8. Next

1. Run `pull_drives.py --list` **before** driving, every time, or the drive has
   no timestamp for ever.
2. Mount it firmly, then drive it properly — a backroad, not a commute. That is
   the only thing that can tune the Spirited bands, and it also settles whether
   2 and 3 above still matter.
3. Confirm the log comes back at 5 Hz. The fix cannot be tested on the bench:
   the IMU is only read once the car has actually spoken.
