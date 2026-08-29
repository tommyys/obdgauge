# Drives view — the ninth view on the board

**Date:** 2026-08-29
**Status:** design, approved in chat, not yet implemented
**Depends on:** drive recording (`2026-08-27-drive-recording-design.md`), which shipped
and is now proven in the car — 27.1 minutes and 57,107 records recorded unattended on
2026-08-29 and pulled off clean.

## Why

The simulator has nine views. The board has eight. The missing one is `Drives`, and it
is the only place the gauge shows you a drive you have already taken.

Everything the recorder writes is currently invisible unless a Mac is plugged in. That
is backwards: the whole point of the flight recorder is the drive nobody was watching,
and the gauge is the thing that is always in the car. This view is how a drive is read
back without a laptop.

## What it is

A Strava-style feed of drives held in the `logs` ring, newest first, each showing what
the drive came to. Four numbers per drive:

- **time** — when it started, and how long it ran
- **distance** — kilometres covered
- **peak rpm** — the highest the engine went
- **peak speed** — the fastest it went

Tap a row and the same four numbers fill the screen, large enough to read at arm's
length. Tap again to go back to the list.

## What it is deliberately not

These were all considered and cut, in this order, because each one costs days and buys
less than it looks:

- **No scrub bar.** The simulator's card can be dragged back through the drive, which
  re-feeds the log through the metrics and moves the whole gauge with it. On the board
  that means a drive in memory and a touch drag on a round panel. Cut.
- **No replay through the other views.** The live views keep showing the car.
- **No driving score.** It was in the design until the last round. The score's maths is
  the open B3 decision (`SPEC.md` §7.5) — "spirited" is not defined, so the number is a
  guess. Four measured numbers and one invented one is worse than four numbers.
- **No speed lock-out.** The view is for when you are parked. It does not enforce that;
  nobody scrolls a drive list at 90 km/h, and a lock-out that misfires is worse than the
  behaviour it prevents.
- **No built-in demo drives.** The `drives` partition's library is for replay when no car
  is connected. This view lists what the gauge itself recorded, and nothing else. An
  empty ring shows an empty list, which is the honest answer on a fresh board.

## Where the numbers come from

**Scanned on demand, not stored.** `LogBuf::read_drive(id, sink, ctx)` already streams a
drive's records sector by sector. The view folds that stream into four numbers and keeps
the result.

This was chosen over writing a summary into the drive at record time, which is the
obvious alternative. Scanning wins because:

- **It works on drives already on the board**, including the 2026-08-29 drive, with no
  reflash-and-re-record cycle to get the feature working.
- **It changes no file format.** `Record` stays 12 bytes, the ring keeps its shape, and
  `tools/pull_drives.py`, `tools/build_drive_asset.py` and the desk replay all keep
  reading what they read now.
- **It is fast enough.** 27 minutes of driving is 688 KB. A flash read of that is well
  under a second, once, and the answer is then cached in RAM.

The fold is a new pure unit, `gauge_core/drive_stats.{h,cpp}`:

```
struct DriveStats {
    double   distance_km;
    double   peak_rpm;
    double   peak_kph;
    uint32_t duration_ms;   // from the record timestamps, not the wall clock
    bool     partial;       // true when the drive has no end marker
};
```

- **peak rpm / peak speed** — the maximum of each channel's records. One pass, no state.
- **distance** — the same trapezoid integration `gauge_core/metrics.cpp` already uses for
  the Trip view: average of consecutive speed samples times the gap, gaps over 5 seconds
  skipped so a stall or a dropped link does not integrate garbage. It must be the same
  code path, or the Drives view and the Trip view will quietly disagree about the same
  drive.
- **duration** — last record's `t_ms` minus the first's. Not the wall clock, which may
  not exist.

`DriveStats` is computed from a record stream and nothing else, so the host tests feed it
canned records and the board feeds it flash. No serial port, no panel, no ESP-IDF.

## Screens

**The list.** Four rows fit legibly on a 466 px round panel. There are more drives than
that, and **there is no existing gesture to reach them**: `ui.cpp` takes
`LV_OBJ_FLAG_SCROLLABLE` off every object and sets `LV_DIR_NONE`, deliberately — when
LVGL decides a drag is a scroll it swallows the gesture, and the carousel's left/right
swipe is that gesture.

So this view, and only this view, turns scrolling back on **restricted to the vertical
axis** (`lv_obj_set_scroll_dir(list, LV_DIR_VER)`). A vertical drag scrolls the list; a
horizontal drag is left alone and still changes view. This is the one piece of the design
that argues with an existing decision, so it is the first thing to test on the board: if a
horizontal swipe starting inside the list ever fails to change view, the fallback is a
pair of page buttons and no scrolling at all.

One row:

```
29 Aug 11:24     27 min
12.4 km   3532 rpm   90 km/h
```

- **A drive with no clock** shows `date unknown` where the date goes, and still shows its
  four numbers. Today's drive would have read that way before it was back-filled.
- **A drive still being recorded** — the one open right now — is shown with its numbers so
  far and marked as running.
- **A drive recorded under a different channel table** is listed but shows no numbers,
  because this firmware's channel ids would mislabel it. It says so rather than guessing.
- **An empty ring** shows one line: no drives recorded yet.

**The card.** The same four numbers, one screen, large. Date and duration as the heading.
Nothing else — this is a summary, not a report.

## Threading, and the rule that governs it

**The scan must not run on the UI loop.** A flash read blocks, `read_drive` takes the
recorder's lock, and the LVGL draw task holds `bsp_display_lock()`. Scanning inline would
stall the panel for the length of a flash read, which is the same failure the panel DMA
rule exists to prevent (`SPEC.md` §3, and the no-allocation rule in
`mx5-gauge-panel-dma-trap`).

So:

- The scan runs on the recorder's own task (core 0), which already owns the lock and is
  already the thing that talks to flash.
- The view asks for a drive's stats and gets either a cached answer or `reading…`, and
  redraws when the answer lands.
- **Nothing in the view's draw path allocates.** The row text is formatted into fixed
  buffers owned by the view, as the other eight views do.
- The UI loop stays on core 1 where `4dfc057` put it.

## Failure cases

| Case | What the view does |
|---|---|
| `logs` partition never mounted | one line: recorder not available |
| Ring empty | one line: no drives recorded yet |
| Drive's channel table ≠ this firmware's | row listed, numbers replaced by a note |
| Drive has no clock | `date unknown`, numbers still shown |
| Drive incomplete (power cut, or recording now) | numbers shown, marked as unfinished |
| Flash read fails mid-scan | that row shows the read failed; the list still works |

## Testing

**Host, in `gauge_core`'s existing suite:**

- `DriveStats` folds a canned record stream to known peaks, distance and duration.
- A gap longer than 5 seconds is skipped, matching `metrics.cpp` — the test asserts the
  two agree on the same input rather than asserting a constant.
- A drive with no speed channel gives zero distance, not a wrong one.
- A drive with a single record does not divide by zero.

**Cross-validation, the check that actually matters:**

`logs/drive-20260829-112400.csv` is a real 57,107-record drive off this board. The same
records fed through `DriveStats` and through the Python simulator's metrics must produce
the same distance, peak rpm and peak speed. `tools/verify_port.sh` already runs this
shape of comparison for the live channels; this extends it rather than inventing a
second harness.

**On the board:**

- The list shows the drives `LIST` reports over the console — same ids, same count.
- A drive's four numbers match what the desk app shows for the same drive pulled to CSV.
- `ui: NN fps` does not dip while a scan runs. This is the one that proves the threading.

## Cost

Two days, including the board test.
