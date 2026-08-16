# B1 — Boot splash, and surviving a power cut

**Date:** 2026-08-16
**Status:** approved, ready to implement
**Supersedes:** `2026-08-12-ignition-animations-design.md` and the plan at
`docs/superpowers/plans/2026-08-16-ignition-animations.md`, both written before
ignition detection existed.

## What changed since the original B1

Two things, and together they cut the feature roughly in half.

**Ignition detection now exists.** `mx5gauge/ignition.py` reads the engine
stopping and starting from `volts` and `run_time`. The old B1 design inferred
ignition from *rpm plus link state* and put a class called `Ignition` in
`state.py`; that name and that file are now taken, and the inference it
described is strictly worse than what shipped. The old plan cannot be executed
as written.

**The board dies with the ignition.** Decided 2026-08-16: the gauge is fed from
an ignition-switched supply, so key-off cuts power instantly. There is no frame
left to draw in, so **the shutdown animation is dropped** — along with the
`SLEEPING`/`ASLEEP` phases and the deep-sleep seam the earlier design carried.
What remains of B1 is the boot splash.

## Consequence: every drive would end `interrupted`

`Recorder.close()` writes the `.json` sidecar holding distance, economy and
score. A power cut never reaches it. Without a sidecar `library.py` falls back
to counting rows, which is why an existing log already reads:

```
13 Aug 20:41   drive   16.9k pts · 27 min · interrupted
```

Under "ignition off = power off" that becomes every drive in the car, forever —
the drive picker would never again show a distance. So this pass also makes the
sidecar survive a hard cut. It is not scope creep: the power decision is what
creates the problem, and fixing it here is what keeps B1 from shipping a
regression alongside a splash screen.

## Design

### `mx5gauge/state.py` — the boot phase

A tiny pure tracker, told the time rather than reading a clock, so a replay at
8x behaves exactly like a car in front of you.

```python
BOOT_MS = 2500

class Boot(object):
    phase     # 'WAKING' then 'RUNNING'
    progress  # 0.0 -> 1.0 through the splash, 1.0 once running
    update(t) # fed the logical timestamp of every real reading
    reset()
```

It starts `WAKING` on the first reading it ever sees and flips to `RUNNING`
once `BOOT_MS` of the drive's own clock has passed. There is no `ASLEEP`: the
device is not asleep when the car is off, it is unpowered, and modelling a
state the hardware cannot be in would be fiction.

Time going backwards — a scrub on the replay timeline — rebases the start
rather than stranding the splash part-way through an animation that can never
finish.

`Gauge` gains `self.boot`, updated inside `sample()` alongside the trip and
score, cleared by `reset()` so loading another drive splashes again. The
snapshot carries `'boot': {'phase': ..., 'progress': ...}`.

### `mx5gauge/web/index.html` — the placeholder splash

A full-screen round `<canvas>` above the views, drawn purely from
`(phase, progress)`. The carousel is **withheld, not merely covered**: nothing
of the instruments shows until the splash finishes, which is how a real cluster
behaves and which also hides the first second of half-populated channels.

Placeholder art, labelled as such: a ring sweeps one lap while the car's name
fades in. Replacing it later touches `drawBoot()` and nothing else.

### `mx5gauge/recorder.py` — a sidecar that survives

`Recorder` gains `summary_fn`, a callable returning the summary dict, and
writes the sidecar every `SIDECAR_SECONDS` (10) alongside its existing
one-second data flush. `close()` still writes the final one, so a clean
shutdown is unchanged and a hard cut costs at most ten seconds of summary.

The write happens **outside** the recorder's lock, because `summary_fn` reaches
into `Gauge.snapshot()` and takes the gauge lock — the two must never be held
in the same order twice.

`run.py` supplies `summary_fn`, building the same dict the shutdown path
already builds.

## Testing

`tests/test_boot.py` — host-side, no clock:

- starts `WAKING` on the first reading, `progress` climbing
- reaches `RUNNING` after `BOOT_MS` of logical time
- a replay at speed behaves identically, because the clock is the drive's
- scrubbing backwards leaves it in a sane phase with `progress` in range
- `reset()` splashes again
- driven through `Gauge`: the snapshot carries the phase, and metadata-only
  samples do not start the splash

`tests/test_recorder.py` — the power-cut case:

- record, let the periodic sidecar fire, then abandon the recorder **without**
  calling `close()` — simulating the cut
- `library.scan()` reports a distance and no `interrupted` flag
- a recorder closed cleanly still writes the final summary

## Out of scope

- The real animation asset.
- Firmware, LVGL, deep-sleep — the board's power story is settled and needs
  none of it.
- Anything touching the driving score (B3) or the ignition detector's rules.
