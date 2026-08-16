# Splitting a session into drives on ignition

**Date:** 2026-08-16
**Status:** approved, ready to implement

## The problem

A recording currently ends when the gauge process ends. If you switch the car
off, walk away and come back, everything lands in one CSV — including the dead
time in between.

The drive recorded on 2026-08-16 is the worked example throughout. It is one
file, `logs/drive-20260816-064348.csv`, spanning 06:44 to 08:00, and it holds:

| from | to | what |
|---|---|---|
| 06:44:05 | 06:59:19 | drive 1, 15.2 min |
| 06:59:19 | 07:06:13 | parked, ignition off |
| 06:59:19 | 07:27:24 | drive 2, 20.9 min |
| 07:27:24 | 07:32:26 | parked, ignition off |
| 07:32 / 07:51 / 08:00 | | three fragments under a minute |

Trip totals, economy and the drive bar all treat that as a single 33 km outing,
which it was not.

## What the data actually shows

Two signals in the stream identify an ignition event, and both are already
being logged.

**The engine stopping** shows up as every PID going silent while `volts`
keeps arriving. The adapter is powered from the OBD port, which stays live
with the ignition off, so the link survives — during both stops it kept
answering `ATRV` every ~28.5 s. The value is the tell: **12.4–12.6 V** parked
against **13.9–14.0 V** running. That is the alternator, and the gap is wide.

**The engine starting** shows up as `run_time` (PID 0x1F, seconds since engine
start) decreasing. Any decrease is unambiguously a new start.

**Silence with no `volts` at all is a different thing entirely** — a dropped
BLE link or a wedged adapter, which `sources.py` already handles with its own
reconnect path. It must not be read as an ignition event. This distinction is
the whole reason the detector requires positive evidence rather than just
watching for quiet.

## Decisions

| Decision | Choice |
|---|---|
| File model | Ignition-off closes the CSV and writes its sidecar; ignition-on opens a new one. **One file stays one drive.** |
| Scope | Live recording only. Logs already on disk are not touched or re-split. |
| Detection | Positive evidence on both edges (below). |
| Short drives | A rotated drive holding under 60 s of samples is deleted, CSV and sidecar. |
| Replay | Detector does not run. |

The file model is the load-bearing decision: because a drive remains a file,
`library.py`, the drive picker and the replay scrubber need no changes at all.
Nothing outside the live recording path learns what a segment is.

## Design

### `mx5gauge/ignition.py`

A pure state machine over the sample stream — no I/O, no clock of its own,
timestamps supplied by the caller. Same shape and rationale as `metrics.py`:
host-testable, and it ports to the firmware unchanged.

```python
ign = Ignition()
event = ign.update(t, key, value)   # -> 'off' | 'on' | None
```

State held: time of the last non-`volts` sample; last `volts` value and its
time; last `run_time`; whether the engine is currently believed off.

Transitions:

- **→ off** — no non-`volts` sample for `OFF_SILENCE_S`, and the latest `volts`
  reading is below `ALTERNATOR_V`. Evaluated only when a `volts` reading
  arrives, which is the only thing still arriving to ask the question.
- **→ on** — any non-`volts` sample arrives while off. Fires on the first PID
  reply, so the new file starts essentially immediately.
- **→ on** — `run_time` decreases, in any state.

Constants, with the reasoning that picked them:

| Name | Value | Why |
|---|---|---|
| `OFF_SILENCE_S` | 8.0 | ~100 missed fast-PID replies. Long enough that no normal stall in the poll loop reaches it. |
| `ALTERNATOR_V` | 13.0 | Midway between 12.5 parked and 13.9 running. |
| `MIN_DRIVE_S` | 60.0 | Below this a rotated drive is discarded (lives in `run.py`). |

The two on-edges are deliberately redundant. The resumption edge handles the
normal case. The `run_time` edge is the backstop for a restart whose off-edge
we never saw, because the adapter or the gauge was down across the stop — the
07:32–08:00 stretch of the example file.

**Redundant, but they must not both fire.** On the real drive the two on-edges
see the same restart moments apart: the PIDs answer at 07:06:32 and `run_time`
turns up with its reset at 07:06:38. Left alone that rotates twice and orphans
a six-second file. Whichever edge fires first therefore drops the `run_time`
baseline, disarming the other until a new baseline is established.

**A voltage freshness window was designed in and then dropped.** The intent was
to ignore ancient `volts` evidence, but because the off-edge is only ever
evaluated when a `volts` reading arrives, the evidence is fresh by
construction — the check could not fire, and an untestable constant is worse
than none. The guard that actually does the work is the silence test: a low
reading while the PIDs are still answering (a tired battery at idle) never
reaches the off-edge at all.

### `mx5gauge/pids.py`

`0x1F` joins `POLL_FAST`. It is a cheap PID, and the backstop edge above is
only as good as how often it is read — on the slow sweep it came round every
~28 s.

This does mean `run_time` stops appearing in the slow sweep and starts
appearing between every slow PID, so recorded files will carry many more
`run_time` rows than before. Harmless: it is one small integer per row and
nothing consumes it by count.

### `mx5gauge/state.py`

`Gauge.sample()` already sees every reading and already owns `reset()`, so the
detector lives there:

- `self.ignition = ignition.Ignition()`
- `self.on_ignition = None` — callback set by `run.py`; when it is `None` the
  detector does not run at all, which is how replay is gated.
- After a reading is stored, feed the detector. On an event, call the callback
  outside the lock — it does file I/O and must not be holding the state lock.

`reset()` gains the detector: a new drive must not inherit the old one's
belief about the engine.

### `run.py`

The shutdown block that closes the recorder with a summary is today written
inline in the `finally`. Extract it to `finish_drive(g, rec, here)` returning
the saved path, and add `rotate(g)`:

1. `finish_drive(...)` — close, write sidecar, print the "session saved" block.
2. If the drive held under `MIN_DRIVE_S`, delete its CSV and sidecar.
3. Open a new `Recorder` with a fresh timestamp; point `g.recorder` at it.
4. `g.reset()` — trip, score and peaks start from zero.

Rotation only ever happens on ignition-**on**. An ignition-off leaves the
current file open and idling: the car may be off for ten seconds at a barrier,
and there is no cost to waiting for the restart before deciding a drive ended.

Deleting is scoped to the exact pair of paths the recorder just returned. It
never touches a file this session did not create.

### `mx5gauge/recorder.py`

Recording filenames carry a timestamp only to the second, which was unique
enough when one drive lasted as long as the process. Rotation can open the
next recorder in the same second as the one it just closed, and the new drive
would then open on top of the finished one and erase it. `Recorder` now
resolves its path to the first free `-2`, `-3`... variant.

## Failure modes

**False rotation** fragments a real drive — the one that matters. Guarded three
ways: the off-edge cannot fire without positive `volts` evidence, so a dead
link cannot trigger it; `reset()` is reachable only through `rotate()`, so
there is a single path to audit; and a false rotation that immediately
re-detects `on` leaves a sub-60 s stub, which is deleted.

**Missed rotation** degrades to exactly today's behaviour: one file, two
drives, nothing lost. Thresholds are chosen to fail in this direction.

**Delete of a wanted file** is bounded by `MIN_DRIVE_S`. A genuinely short hop
— shuffling the car on the driveway — is discarded. Accepted.

## Testing

All host-side, no adapter.

`tests/test_ignition.py`:

- clean off then on
- silence with no `volts` — must **not** fire (dead link)
- silence with `volts` at 14 V — must **not** fire (running, poll stalled)
- a low reading with the PIDs still answering — must **not** fire (tired
  battery at idle)
- a gap shorter than `OFF_SILENCE_S` — must **not** fire
- `run_time` reset with no preceding off — fires `on`
- `run_time` climbing normally, and the first `run_time` of a session — silent
- `tests/fixtures/ignition-edges.csv`, cut from the 2026-08-16 log around each
  real edge, asserting exactly two offs and exactly two ons. The offs are
  checked as *bounded lateness* after the true stop rather than at an exact
  second: they can only be noticed when a voltage read arrives, which parked
  is sparse and irregular — 27 s after the first stop, 74 s after the second.
  That lateness costs nothing, because nothing rotates on the off-edge.

`tests/test_rotation.py`, against a real recorder in a temp directory:

- summary written, new path opened, trip totals zeroed
- a sub-`MIN_DRIVE_S` drive is removed, CSV and sidecar
- a drive over the threshold is kept, with its distance in the sidecar
- two rotations leave two distinct files
- an ignition-off on its own closes nothing and disturbs no totals
