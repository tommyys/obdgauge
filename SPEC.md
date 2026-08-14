# Project spec — MX-5 ND3 custom OBD gauge

Context, decisions and roadmap. `README.md` covers *how to run* the simulator;
this file covers *what we're building, why, and what's next* — enough to pick
the project up cold.

---

## 1. Goal

A custom OBD-II gauge for a **Mazda MX-5 ND3 (2024, 6-speed automatic)**. A
1.28" round touch display on the dash, driven by an ESP32, showing what the
factory cluster doesn't. Firmware written from scratch — the build is the point,
not the destination.

**Guiding principle:** *only show what the factory dash doesn't.* That's why
coolant in real °C, battery voltage, fuel economy and the driving score earn
their place, and why gear/speed largely don't.

**Current state:** a complete **Mac simulator** (this repo) that talks to the
real car over BLE and renders the full UI. Hardware not yet purchased. The
simulator is deliberately a firmware prototype — the decode maths, metrics and
ELM327 flow are meant to port to C++ largely as-is.

---

## 2. The car — confirmed facts

Established from four real captures plus live BLE sessions. Don't re-litigate
these; they cost real driving to establish.

| Fact | Detail |
|---|---|
| **Oil temp is NOT available** | No oil-temperature PID exists. Confirmed against 4 captures *including* the Mazda `WWH-OBD + CAN and extra PIDs (2026->)` extended profile, which returned **zero new channels**. `grep -i oil` across all raw captures: no matches. ND owners confirm the dash gauge reads coolant. → **coolant is the home-screen hero**; real oil temp needs a physical sender (~RM150–300 + install). |
| **Gear IS available** | `Transmission Actual Gear Ratio` (3.54 = 1st on the 6AT). Usable for a derived gear readout. |
| **~66–94 channels reported** | Standard Mode 01 PIDs are rich on this car. 56 are decoded here. |
| **Adapter speaks BLE** | Capture header reads `BTLE vLinker MC-IOS`. iOS forbids Bluetooth Classic SPP for generic accessories, so the adapter runs BLE natively — which validates the BLE-only board choice below. |
| **OBD pin 16 is always live** | The port has power with the ignition off. Powering the gauge from it would slowly flatten the battery. |
| **Car Scanner loses data** | iOS suspends the backgrounded app: a 54-minute recording contained only ~7 minutes of samples, in bursty ~10 Hz spurts. This is *why* this project logs on the Mac instead. |

**Safety constraint:** read-only, passive. The 2024 car has a CAN gateway and is
under warranty. We only *read* standard PIDs. No writes, no gateway pokes.
Changing car settings needs dealer tooling (Mazda MDARS + a J2534 interface) and
is explicitly out of scope.

---

## 3. Hardware

### Chosen board

**Waveshare ESP32-S3-Touch-LCD-1.28** (~RM100–140) — all-in-one:

- round **GC9A01** 240×240 IPS LCD
- **CST816** capacitive touch → the swipeable view carousel
- **QMI8658** 6-axis IMU → harsh-event detection for the driving score

*Trade-off accepted:* the ESP32-S3 has **no Bluetooth Classic**, so it talks to
the adapter over **BLE**. That's fine — the vLinker already does BLE with the
iPhone every day. Bundling touch + IMU + round display on one board outweighs
the slightly fiddlier transport.

> If you ever swap to a modular build, note the **classic ESP32-WROOM-32** has
> Bluetooth Classic (simpler SPP transport) but needs a separate LCD, touch
> controller and IMU.

### Bill of materials (Malaysia, typical street prices)

| Item | Model | RM |
|---|---|---|
| OBD adapter | **Vgate vLinker MC+** (owned) | 130–180 |
| Board | Waveshare ESP32-S3-Touch-LCD-1.28 | 100–140 |
| Power | DC-DC buck, 8–40 V → 5 V 3 A | 8–15 |
| OBD extension | male→female / Y-splitter cable | 15–30 |
| 12 V tap | add-a-circuit fuse tap + inline fuse | 5–10 |
| Wiring | JST connectors, heat-shrink | 10–20 |
| *(later)* GPS | u-blox NEO-M8N | 18–35 |
| *(later)* SD | microSD breakout | 3–6 |

**Avoid RM20 ELM327 clones** — flaky firmware, dropped links, inconsistent PIDs.

### Power (in-car install)

Feed the buck converter from an **ignition-switched 12 V source**, *not* OBD
pin 16, and have the firmware **deep-sleep** when the link goes idle as a
backstop. Bench and early in-car testing run on **USB** — no wiring or soldering
needed until the permanent install.

---

## 4. Design decisions (and why)

Recording the reasoning so it isn't relitigated:

- **No dedicated g-force view.** OBD exposes no accelerometer. The
  "acceleration" channel in Car Scanner logs is just speed differentiated —
  coarse and laggy. The IMU stays (it's free on the board) and feeds the
  driving score instead.
- **Tacho was dropped, then added back as view 1.** The factory dash has a big
  RPM meter, so it was originally excluded. It returned because seeing live RPM
  is the clearest confirmation the link is working.
- **Coolant is the home hero, not oil.** Forced by section 2. Still worth a view
  despite the dash having a coolant gauge: the factory needle is heavily damped
  and parks mid-scale almost regardless, whereas this shows the real number
  (logs show a clean 72 → 95 °C warm-up the stock gauge hides).
- **Views degrade honestly.** Anything the car isn't reporting shows `--` or
  `n/a`, never a plausible-looking zero — and a view whose channels are all
  missing says so outright rather than drawing an empty dial. No dead "not
  available" labels either: the oil-temp line only appears on a car that
  actually reports PID 0x5C.
- **Log everything, not just what's drawn.** Polling only display channels made
  the log a narrow slice. Now every supported PID is swept, with rpm/speed/
  throttle interleaved between each so the needle stays responsive.
- **Replay uses the file's own timeline.** Using wall-clock made a 10× replay
  report phantom harsh-braking (84 false events). Metrics must see real-world
  seconds.
- **The rpm backdrop puts red at the rim, not the centre** (`glow=rim`).
  Compared all three side by side on the real gauge at 2500 / 4500 / 6800 rpm
  (`web/compare.html`). `centre` was the most aggressive but washed out the
  *small* labels under the big number — `RPM`, and the `KM/H · THR · PEAK` row —
  as it approached the redline; the big number itself survived, the supporting
  text didn't. `band` (red ring, dark core) was the punchiest while staying
  legible, but busier. **Chosen: `rim`** — the centre stays pure black, so every
  readout holds full contrast on a gauge you glance at for a fraction of a
  second. The other two modes remain in the code only so the comparison page
  keeps working for future visual decisions. See §11.

---

## 5. Architecture

```
BLE / replay  ->  ELM327  ->  PID decode  ->  VehicleState
                                                  |
                                    metrics (economy / trip / score)
                                                  |
                                       HTTP :8420 -> round gauge UI
```

| Module | Role | Ports to firmware as |
|---|---|---|
| `mx5gauge/pids.py` | PID table, decode formulas, response parsing | `src/obd/pid.cpp` |
| `mx5gauge/metrics.py` | economy, trip accumulators, driving score | `src/metrics/` |
| `mx5gauge/sources.py` | BLE + ELM327 client; `.brc`/CSV replay | `src/obd/ble_transport.*`, `elm327.*` |
| `mx5gauge/state.py` | shared state, range validation, derived snapshot | `VehicleState` struct |
| `mx5gauge/recorder.py` | CSV logging + JSON summary | SD-card logging (phase 3) |
| `mx5gauge/web/index.html` | the nine views | LVGL screens |
| `mx5gauge/library.py` | the replayable-drive library | SD-card index |
| `mx5gauge/vehicle.py` | VIN decode, per-car dial profiles | `src/vehicle.*` |

`pids.py` and `metrics.py` are pure and host-tested — that's deliberate, so the
maths is proven before it ever runs on the board.

### Tunables

- `metrics.FUEL_PRICE_RM` — fuel price for cost readouts (currently 2.05)
- `metrics.W_SMOOTH / W_ECON / W_CALM` — driving-score weights (0.40 / 0.30 / 0.30)
- `metrics.ECO_RPM_LO/HI` — efficient rev band (1200–2600)
- `metrics.HARSH_ACCEL / HARSH_BRAKE` — harsh-event thresholds (m/s²)
- `state.RANGES` — per-channel plausibility bounds

---

## 6. The nine views

| # | View | Fed by |
|---|---|---|
| 1 | Tacho | rpm, speed, throttle |
| 2 | Engine (home) | coolant + COLD/WARMING/READY, battery, intake |
| 3 | Fuel economy | fuel rate ÷ speed, trip totals, RM cost |
| 4 | Driving score | smoothness + economy + calm, coach word |
| 5 | Trip | distance, time, avg speed, fuel, cost |
| 6 | Power | actual torque % × reference torque × rpm |
| 7 | Thermals | coolant, intake, catalyst |
| 8 | Electrical | control-module voltage / ATRV, charge status |
| 9 | Drives | the replayable-drive library (§12), and one drive's summary + timeline (§13) |

Fuel rail temperature was dropped from Thermals: across every capture it
returned **2 distinct values in 375 samples** (72/73 °C), so it is a canned
number rather than a sensor — and there is no standard mode-01 PID for rail
*temperature* anyway (only pressures), so it could never have worked live.
Catalyst, by contrast, is real: 1765 samples, 971 distinct values, 503–688 °C.

The carousel wraps infinitely: each view is placed at its shortest signed
distance from the current one, so the last-to-first step is one slide rather
than a rewind of the whole strip.

---

## 7. Roadmap

**Done**
- Reverse-engineered the Car Scanner `.brc` format (`mx5gauge/brc.py`)
- Established what the car does and doesn't expose (section 2)
- Mac simulator: live BLE + replay, eight views, full logging, session replay
- **Live BLE connection to the car verified working**

**Next**
1. **Capture a proper drive** — 15–20 min with variety (town, open road, a few
   pulls). Needed to tune the driving-score weights against real numbers.
2. **Buy the board** (section 3) — the long pole; order early.
3. **Phase 0 — bench bring-up** (USB, no soldering): display test pattern, touch
   events, IMU readings, BLE connect + ELM327 init.
4. **Phase 1 — firmware MVP**: port `pids` + `metrics`, render the engine-vitals
   home screen, add ignition-off deep-sleep.
5. **Phase 2 — the carousel**: touch swiping, the remaining views, the
   user-supplied startup animation.
6. **Phase 3 — extras**: SD logging, Wi-Fi sync, GPS, shift LEDs, 3D-printed
   enclosure, permanent switched-12 V install.

**Open questions**
- Driving-score weights are untuned guesses — needs a real drive log.
- Whether to fit a physical oil-temp sender.
- Mounting position and enclosure design.
- Exact ignition-switched 12 V tap point in the car.

---

## 7.5 Backlog — new feature requests (opened 2026-08-12)

Five items raised this session. We work them **one at a time**; each lands here
first so nothing gets lost, then graduates into section 5/6/7 once built.

| # | Item | Status |
|---|---|---|
| B1 | Startup + shutdown animation on ignition on/off | designed, not built (`docs/superpowers/specs/2026-08-12-ignition-animations-design.md`) |
| B2 | In-UI view to browse and pick a past drive to replay | **shipped (simulator) — §12** |
| B3 | Define the driving score: what is "spirited", what is "harsh"? | **open question — see below** |
| B4 | Does OBD expose convertible-roof up/down? | **answered: NO — see below** |
| B5 | Make it a *universal* gauge; show car make/model at the top | **shipped in the simulator — §10** |

### B1 — Ignition on/off animations

Play a startup animation when the car wakes and a shutdown animation when it
sleeps. Ignition state is inferred, not a dedicated signal:

- **On:** BLE link establishes *and* first live RPM > 0 (engine cranking/running).
- **Off:** RPM falls to 0 and the link goes idle → the shutdown animation runs,
  then the board deep-sleeps (existing backstop in the roadmap).

Note the earlier decision that *the user supplies the startup animation asset* —
so this item is the **trigger + playback machinery**, plus a placeholder
animation in the simulator. To design once we pick it up.

### B2 — Replay picker view

Today past drives are listed only via the CLI (`run.py --sessions`). This adds
an **on-screen** way to scroll a list of recorded drives (date, duration,
distance, score) and pick one to replay — the touch-carousel equivalent of the
sessions list. Simulator-first; on hardware it reads the SD-card log index.

### B3 — Driving-score definition (open — needs a decision)

**Current logic** (`metrics.py`, all host-tested) blends three 0–100 sub-scores:

- **smooth (0.40):** average throttle jerk (%/s); 0 → 100, 12 %/s → 0.
- **econ (0.30):** time in the 1200–2600 rpm band + average instant L/100km.
- **calm (0.30):** harsh events per minute; each event drops it 25 pts/min.

**"Harsh" today** = longitudinal acceleration from the *speed delta*
(`Δspeed / dt`), thresholds **> +2.5 m/s² (accel)** and **< −3.0 m/s² (brake)**.
This is coarse and laggy — speed differentiated at ~1 Hz — and it is the *only*
input to "calm".

**What's missing:**
- **"Spirited" is not defined at all.** The score only rewards calm/economical
  driving and silently punishes fun. On a scenic drive that's backwards. Need to
  decide whether spirited is (a) a *separate* positive metric, (b) a driving
  *mode* that reweights the score, or (c) left out.
- **The on-board IMU is unused.** The board has a QMI8658 6-axis IMU; real
  lateral/longitudinal g would replace the speed-delta proxy and finally make
  "harsh" (and any "spirited") meaningful. Decide the thresholds against a real
  IMU + drive log, not guesses.

→ This is a design decision to make deliberately, then re-tune the constants.

### B4 — Roof up/down over OBD? **No.**

Enumerated **all 82 channels** across every capture (standard `Mazda OBD-II /
EOBD` profile *and* the extended `WWH-OBD + CAN and extra PIDs (2026->)`
profile). **Zero** relate to the convertible top — no roof, latch, hood or
top-position channel exists. `grep -i` for roof/top/convertible/latch across the
raw captures: no matches. The ND's soft top is a **manual mechanical latch** with
no electronic position sensor on the standard OBD bus. Same conclusion as oil
temp: if we want roof state it needs a **physical sensor** (a reed/hall switch on
the latch wired to a spare GPIO), not OBD. Out of scope for the OBD path.

### B5 — Universal gauge + make/model banner

Generalise beyond the MX-5 so the same firmware works on any OBD-II car (the
premise from last session: OBD is universal, the *data availability* isn't), and
show the car's **make/model as a small banner at the top of the display**.

- The `.brc` header already carries a car name + `carprofile` string; live, the
  make/model can come from a **config value** or be read from the **VIN**
  (Mode 09 PID 02) and mapped to make/model.
- "Universal" means: probe supported PIDs at connect (Mode 01 PID 00 bitmasks),
  light up only the views a given car actually feeds, and degrade the rest to
  `n/a` — exactly the honest-degradation rule already in section 4.
- Keep the MX-5 as the reference profile; add a small per-car override table.

---

## 8. Reference

Illustrated companions (design mockups, diagrams, findings):

- Build plan & wiring — <https://claude.ai/code/artifact/327b7128-1d54-4c12-bb9c-af5b6f387df4>
- View carousel mockup — <https://claude.ai/code/artifact/8d205285-05fe-46d2-b9dc-9aadc33a14ca>
- Coffee-run playback — <https://claude.ai/code/artifact/ac87105c-a07f-4e00-bc58-9edd9874c084>
- Session log & oil-temp verdict — <https://claude.ai/code/artifact/d0c26230-78bf-4281-b749-74d78bc879e1>

## 9. Picking this up in a new session

1. Read this file, then `README.md` for the commands.
2. `.venv/bin/python tests/test_pids.py` — should print `all decode tests passed`.
3. `.venv/bin/python run.py --replay` — the gauge should come up at
   <http://127.0.0.1:8420> playing the sample capture.
4. `.venv/bin/python run.py --sessions` — lists any drives recorded locally
   (they're gitignored, so a fresh clone shows none).

Recorded drives and three of the four captures stay local by design — they're
personal telemetry. One sample capture ships so replay works out of the box.

---

## 10. The universal layer (B5, shipped in the simulator)

The gauge no longer assumes an MX-5. Three things adapt to whatever car is
plugged in, and the car is named on the display itself.

### Identity — `mx5gauge/vehicle.py`

Pure, host-tested (`tests/test_vehicle.py`, 39 checks), ports to firmware
beside `pids` and `metrics`.

| From the VIN | Reliable? | How |
|---|---|---|
| **Make** | yes | WMI (first 3 chars) against a built-in table (~120 entries incl. Proton/Perodua) |
| **Model year** | yes | position 10, with the 30-year cycle resolved by rejecting implausibly-future years (`W` → 1998, not 2028) |
| **Model name** | **no** | genuinely impossible from a VIN alone — needs a commercial VIN database |

The VIN is read live once per connection via **mode 09 PID 02**, reassembled
from its multi-frame reply. Because the model name can't be derived, it comes
from `--model` (or `MODEL_HINTS` for a known VIN prefix). Everything degrades
honestly: unknown WMI shows `WMI XXX`, no VIN at all shows `OBD-II`.

Captures carry no VIN, so replay identifies the car from the Car Scanner
profile string instead (`Mazda OBD-II / EOBD` → Mazda) and leaves the year
blank rather than inventing one.

### Dial scaling — `vehicle.PROFILES`

An 8000/7000 tach is right for an MX-5 and wrong for almost everything else,
so redline, rpm ceiling and the power-dial top come from a per-car profile
(`'Make Model'` beats `'Make'` beats a wide default). The tacho and power dials
**rebuild themselves** when the profile changes — ticks are placed by value, so
a 6500 ceiling still lands its numbering correctly.

### Honest view gating

Each view declares the channels it needs (`data-needs`). The car's real channel
set comes from the supported-PID bitmasks when live, and from what the file
actually contains when replaying. A view with none of its channels dims and
states why — *"NO TORQUE DATA · power needs actual + reference torque, which
this car does not report"* — instead of drawing a convincing zero. Verified on
the sample capture: exactly one view (Power) gates, because that capture holds
no torque channels.

Battery voltage is treated as always available: it comes from the adapter's
`ATRV` command, not a PID, so no bitmask advertises it.

### Desk preview

`Gauge — REPLAY (desk).command` — double-click, no arguments, no car. It picks
the capture with the most actual driving, frees a stale port, and opens the
browser. `run.py` alone does the same from a terminal.

---

## 11. The rpm-reactive backdrop

The display warms up as the engine does: near-black at idle, a dim ember
through the mid-range, an intense red approaching the redline.

Two decisions worth keeping:

- **Red at the rim, black in the middle** — chosen over a centre bloom and a
  mid-band ring after comparing all three on the real gauge (§4). Every view
  puts its main readout dead centre, so tinting there costs legibility. The
  backdrop is a vignette: transparent to ~24% radius, then ramping to
  near-opaque red at the edge. Verified readable at redline — `6800` stays
  crisp on a full-red screen.

  `web/compare.html` renders all three shapes at three rev levels as live
  gauges, using the preview switches `?glow=<mode>`, `?rpm=<n>` (pin the revs,
  since a desk capture rarely passes 2000) and `?bare=1` (hide the browser
  chrome). Reach for it the next time a visual choice needs settling.
- **Keyed to the car's own redline, not a fixed 8000.** `f` is derived from
  `rpm / rpm_red` (from the §10 profile), so the colour means the same thing in
  any car. Tint starts at 22% of redline — town driving shows barely anything,
  which is the point — and `f` is raised to the power 1.35 so the mid-range
  stays restrained and the last part of the tacho bites.

Measured ramp on an MX-5 profile (redline 7000): 800 rpm → nothing ·
2000 → 0.03 ember · 4000 → 0.31 · 6000 → 0.70 · 7000 → 0.92 full red.

It lives on `.screen`, outside the sliding `.track`, so the colour **persists
across all views** rather than restarting per view. Note the z-index: `0` and
first in the DOM, *not* a negative z-index — `.screen` isn't a stacking
context, so a negative child would escape it and hide behind the bezel.

Cost control: opacity is rewritten every frame (cheap), but the gradient string
only when the colour moves a visible step. It reuses the tacho's existing eased
rpm, so there's no extra timer and the colour can't strobe on a jittery reading.

On the board this becomes a background gradient redrawn on the same rpm easing —
no per-frame allocation needed.

---

## 12. The Drives view (B2, shipped in the simulator)

A ninth view in the carousel lists every past drive and loads one on tap, so
picking a drive no longer means restarting the process from a terminal.

### What counts as a drive — `mx5gauge/library.py`

Two kinds of file are replayable and both belong in one list: `logs/*.csv`
(drives the gauge recorded itself, usually with a `.json` summary beside them)
and `captures/*.brc` (Car Scanner recordings that predate the project). On the
board this module becomes the SD-card index — same shape, cheaper source.

Summarising a `.brc` means parsing it, so results are cached against each
file's size and mtime. A cold scan of four captures takes ~90 ms; a warm one is
instant.

Two details that matter more than they look:

- **The date comes from the filename, not mtime.** Every capture in the repo
  shares an mtime (they were copied in together), which made the picker's most
  useful column read `12 Aug 19:10` on every row. Both naming schemes carry the
  real moment — `2026-08-11 21-43-36.brc` and `drive-20260812-212705.csv` — so
  that is parsed out, and used for sorting too.
- **Captures do not quote a distance.** Integrating speed from a Car Scanner
  file gives nonsense (§2: iOS suspends the app, leaving gaps), so a capture row
  quotes what is solid — channel count, sample count, duration, and whether the
  file contains any driving at all. Only our own logs quote km and a score.

### Loading a drive

`GET /sessions` lists the library plus what is currently playing.
`POST /select {"name": …}` switches. `run.py`'s `Player` owns the running
source: the HTTP thread only records the request and pokes the event loop, and
the swap itself happens on the loop, cancelling the old source before starting
the new one.

`Gauge.reset()` runs on every load. Without it a second drive would inherit the
first one's trip totals, and stale channel values would keep views lit for data
the new file never sends. Metadata goes too, so the car banner and the view
gating re-derive from whatever is now playing.

**Switching is replay-only.** In the car the live link is the point, so the view
still lists your drives but the rows are inert and say why; `POST /select`
answers 409. Same reasoning as `--sweep`.

**A request can only ever select a file the library already listed.** `resolve`
matches on basename against the real listing, so a crafted path cannot reach
anything else on disk — covered by `tests/test_library.py`, which asserts
`../mx5gauge/server.py`, `/etc/passwd` and friends are all refused.

### Tapping had to be made possible

The carousel grabbed the pointer on `pointerdown`, which retargets everything
that follows to the screen and swallows clicks on anything inside it — the rows
could never have been tapped. Capture is now deferred until the pointer has
actually travelled 8px, so a tap stays a tap and a drag still swipes.

### The terminal and the view agree

`run.py --sessions` and `--replay last` read the same library, so neither can
disagree with what the Drives view shows. Before this they globbed `logs/` only
and reported "no sessions" while the picker listed four captures.

---

## 13. Reviewing a drive: the timeline (shipped in the simulator)

Replay existed to *watch* a drive; this makes it possible to *review* one. Three
changes, agreed 2026-08-13 and built 2026-08-14.

### One primitive: `ReplaySource.seek(t, gauge)`

Everything here rests on a single operation. Seeking to `t` resets the gauge,
then pushes every row before `t` through `gauge.sample()` with its own logical
timestamp and no sleeping at all, before playback carries on from there.

The point of doing it that way is that there is no second implementation of the
maths. The trip totals and the driving score at any scrub position are produced
by exactly the code that produces them during playback, so the summary can
never drift from what the gauge would have shown had you sat and watched. The
rows are already in memory, so even seeking to the end of a 3000-second capture
is instant. `tests/test_sources.py` asserts the equivalence directly: seek to
`t` and play straight through to `t` must agree on every trip and score field.

Seeking to the very end **parks**: the source holds that frame and waits rather
than looping round, because "opened at the end" is a drive's summary, not a
replay that ran out. Anywhere else resumes playing.

### 1. Position bar

`ReplaySource` tracks `pos` and `duration` along the **capture's own timeline**,
which is the time the drive really took — playing it back at 8x does not make
the drive eight times shorter, and the bar must not claim it did. `snapshot()`
reports `replay: {pos, dur, paused}`, or `null` when live: there is no recorded
timeline to sit on when the data is the car in front of you.

The bar is a chord across the bottom of the circle, not a full-width strip — at
the rim the round screen has already clipped most of the width away.

### 2. The drive card

Tapping a row in Drives no longer just starts the drive. It opens that drive **at
its end**, and replaces the list with a summary card: distance (or sample count
— captures still refuse to quote km, §12), elapsed, score, channel count, over a
full-width bar sitting at 100%. Scrubbing back from full is what replays it;
`POST /seek {"t": …}` carries the request, and the seek runs on the event loop,
never the HTTP thread, so two writers never interleave into one set of totals.

Three details:

- **The seek fires on release, not on every move.** Each seek re-feeds the whole
  log up to that point; one per pixel would swamp the loop.
- **The card follows playback, except while a thumb is down.** Otherwise the
  100 ms poll yanks the handle out from under the drag.
- **The scrub target is far taller than the track it draws,** and stops
  propagation on both pointer and arrow-key events — the carousel listens on the
  same gestures, and scrubbing must not slide the view out from under you.

### 3. Replay shows the revs the drive actually turned

The REPLAY launcher passed `--sweep 1000-7000`, which made the rev-reactive
visuals easy to admire on a capture that idles — and made replay useless for
reviewing a drive, which is now what replay is for. The launcher no longer
passes it. The flag survives on `run.py` for judging visuals against an idling
capture, still labelled as a preview on screen and still refused in live mode.
