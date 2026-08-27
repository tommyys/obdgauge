# Project spec — MX-5 ND3 custom OBD gauge

Context, decisions and roadmap. `README.md` covers *how to run* the simulator;
this file covers *what we're building, why, and what's next* — enough to pick
the project up cold.

---

## 1. Goal

A custom OBD-II gauge for a **Mazda MX-5 ND3 (2024, 6-speed automatic)**. A
1.75" round AMOLED touch display on the dash, driven by an ESP32-S3, showing what the
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

**Waveshare ESP32-S3-Touch-AMOLED-1.75C** — all-in-one, and *not* the board the
first draft of this spec picked. Everything below is measured on the real
hardware rather than read off a product page:

- round **CO5300 AMOLED, 466×466**, over QSPI. This project's vendored BSP
  raises the panel clock to 80 MHz; at that clock a full-screen transfer costs
  19 ms measured
- **CST9217** capacitive touch → the swipeable view carousel
- **QMI8658** 6-axis IMU. Read for the first time on 2026-08-27: gravity
  registers on **Z, through the screen**. Which of X and Y is longitudinal
  is still unknown and needs a moving car in the final mounting orientation
- 32 MB flash, 8 MB octal PSRAM

*Trade-off accepted:* the ESP32-S3 has **no Bluetooth Classic**, so it talks to
the adapter over **BLE**. Unchanged from the original reasoning and still worth
recording — and now confirmed in the car: the board scans, connects to the
vLinker, and runs the ELM327 handshake with no Mac involved.

> If you ever swap to a modular build, note the **classic ESP32-WROOM-32** has
> Bluetooth Classic (simpler SPP transport) but needs a separate LCD, touch
> controller and IMU.

**The memory constraint that governs this board.** Internal DMA-capable RAM is
the scarce resource, not PSRAM and not flash. Any SPI transfer to the panel
whose source is not DMA-capable makes the driver allocate a *contiguous
internal* buffer the size of the whole transfer and copy into it, per transfer
(`spi_master.c`, `setup_priv_desc`). Drawing from PSRAM in large bands
therefore needs a large contiguous internal block on every single flush, and
once the heap is fragmented that allocation fails permanently — the display
freezes mid-frame and does not recover. **Nothing in the draw path may
allocate.** The LVGL draw buffers live in internal DMA RAM in 16-row bands and
the carousel slide blits through one 32-row buffer reserved at first use. That
leaves ~22 KB of DMA-capable RAM spare, which the `ui:` log line reports every
two seconds precisely so the next thing to want internal memory can be checked
against it.

### Bill of materials (Malaysia, typical street prices)

| Item | Model | RM |
|---|---|---|
| OBD adapter | **Vgate vLinker MC+** (owned) | 130–180 |
| Board | **Waveshare ESP32-S3-Touch-AMOLED-1.75C** | 200–260 |
| Power | DC-DC buck, 8–40 V → 5 V 3 A — *optional, see Power* | 8–15 |
| OBD extension | male→female / Y-splitter cable | 15–30 |
| 12 V tap | add-a-circuit fuse tap + inline fuse — *optional, see Power* | 5–10 |
| Wiring | JST connectors, heat-shrink | 10–20 |
| *(later)* GPS | u-blox NEO-M8N | 18–35 |
| *(later)* SD | microSD breakout | 3–6 |

**Avoid RM20 ELM327 clones** — flaky firmware, dropped links, inconsistent PIDs.

### Power (in-car install)

**Measured in the car on 2026-08-27: this car's USB socket is CONSTANT, not
ignition-switched.** The board was run from it with nothing else attached and
stayed lit with the key out. That single fact decides most of this section.

**Plugging into the car's USB socket is a legitimate permanent install.** The
buck converter and the fuse tap in the BOM above are therefore **optional** —
they buy a tidier cable run, not function. Power-on to live engine data takes
**32 seconds** on that socket with no laptop involved: boot 0.06 s, display
5.4 s, adapter found 13.1 s, 50 PIDs 28.0 s, first reading 32.0 s.

**The cost of constant power is that the gauge never stops.** Key out, car
locked, and it sits there with the panel lit and BLE scanning until the battery
gives up. On a car that sits between weekends that is a flat battery, and it is
the most important open item on the firmware path. **Until the firmware sleeps,
unplug the board when leaving the car.**

**The shutdown trigger already exists and is proven.** At key-off the vLinker
stops answering and the poll loop reports it within about 15 seconds (12
consecutive empty replies), observed at a real key-off. That is the signal to
blank the panel and sleep — and, because there *is* power after key-off, it is
also the moment a shutdown animation could play. See §14, where the reasoning
that ruled one out no longer holds.

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

The simulator and the firmware are the same design twice, meeting at one seam.
Everything above the transport is pure logic, host-tested, and shared in shape
between the two; only the bytes on the wire differ.

```
  Mac:   bleak ---.                                    .--> HTTP :8420 -> browser
                   \                                  /
                    +-> ELM327 -> PID decode -> VehicleState
                   /                    |             \
  Board: NimBLE --'          metrics (economy /         '--> LVGL -> AMOLED
                             trip / score)
```

| Module | Role | Ported to |
|---|---|---|
| `mx5gauge/pids.py` | PID table, decode formulas, response parsing | `gauge_core/pid.cpp`, `poll.cpp` |
| `mx5gauge/metrics.py` | economy, trip accumulators, driving score | `gauge_core/metrics.cpp` |
| `mx5gauge/sources.py` | BLE + ELM327 client; `.brc`/CSV replay | `gauge_platform/ble_transport.cpp` (NimBLE), `gauge_core/elm327.cpp`, `main/live_link.cpp` |
| `mx5gauge/state.py` | shared state, range validation, derived snapshot | `gauge_core/state.cpp` |
| `mx5gauge/vehicle.py` | VIN decode, per-car dial profiles | `gauge_core/vehicle.cpp` |
| `mx5gauge/ignition.py` | engine stop/start from the sample stream | `gauge_core/ignition.cpp` |
| `mx5gauge/library.py` | the replayable-drive library | `gauge_core/replay.cpp` + `main/drive_source.c` (mmap'd `drives` partition) |
| `mx5gauge/web/index.html` | the eight views | `gauge_ui/ui.cpp`, `slide.cpp` |
| `mx5gauge/recorder.py` | CSV logging + JSON summary | **not ported** — on-board drive logging is still phase 3 |

**The transport seam.** `gauge_core/transport.h` is the whole of it: `write`,
`read` up to the ELM327's `>` prompt, and a delay. `bleak` fills it on the Mac,
NimBLE on the board, and a fake fills it in the host tests — so the handshake
proven by `test_elm327.cpp` is byte-for-byte the one that runs in the car.
There is no second implementation to keep honest.

**Everything in `gauge_core` builds and runs on the host.** `firmware/test/host`
covers state, PID decode, poll order, metrics, carousel wrap, view gating,
ignition and replay. `tools/verify_port.sh` goes further: it replays real logs
through both the C++ and the Python cores, diffs every channel, then mutates
the C++ to prove the harness would have noticed. Run it after touching anything
in `mx5gauge/` that the port mirrors.

**What is board-specific, and is therefore the only code a host test cannot
reach:** `gauge_platform/ble_transport.cpp`, `main/drive_source.c`,
`main/imu.c`, `main/live_link.cpp`, `main/flight_log.cpp` and the LVGL views.
`flight_log` exists for exactly that reason — see §3's note that the gauge's
real life has no console attached.

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

**Eight of the nine are on the board.** Views 1–8 are built and running on the
AMOLED (plus the not-available screens that replace any view whose channels the
car does not supply — `gauge::view_available`, host-tested). **View 9, Drives,
is simulator-only**: browsing a library of past drives needs drives to have been
recorded on the board first, and on-board drive logging is still phase 3. The
firmware's carousel is eight wide, and `gauge_ui::view_count()` is the authority
on that.

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
- **Board bought and brought up** — the AMOLED-1.75C, not the board this spec
  originally picked (section 3)
- **`gauge_core` ported to C++** and host-tested, cross-validated against the
  Python core by `tools/verify_port.sh`
- **All eight views on the board**, plus the not-available screens
- **Boot splash on the panel** (§14)
- **Phase 0 complete — the board talks to the car.** Proven 2026-08-27 with the
  engine running: scan → connect → ELM327 handshake → 50 supported PIDs →
  views switched off replay and onto the vehicle, no Mac in the loop. Idle
  rpm 770, coolant 89 °C, 13.4 V, ~1 L/h — decoded values sane, not merely a
  reply received. Survives an ignition cycle unattended and reconnects itself.
- **Runs standalone on the car's USB socket**, 32 s from power-on to live data

**Next**
1. **Sleep when the car shuts down.** The top item, and it is a battery
   problem, not a feature: the USB socket is constant (section 3, Power), so
   the gauge currently runs until the battery is flat. The trigger is already
   proven — the adapter stops answering within ~15 s of key-off. Blank the
   panel, sleep, wake when it answers again. The shutdown animation that B1
   dropped becomes possible in the same change.
2. **Capture a proper drive** — 15–20 min with variety (town, open road, a few
   pulls), now recordable from the board itself rather than the Mac.
3. **B3 — decide what "spirited" means** (§7.5). Not a build task. Nothing in
   the driving score should be tuned before it is settled, or the constants get
   tuned twice.
4. **Mount the board**, then identify the IMU axes in that orientation. Gravity
   is known to read on Z; longitudinal versus lateral is not, and cannot be
   until the board is fixed in place.
5. **Phase 3 — extras**: on-board drive logging, Wi-Fi sync, GPS, shift LEDs,
   3D-printed enclosure.

**Open questions**
- Driving-score weights are untuned guesses — needs a real drive log *and* B3.
- Whether to fit a physical oil-temp sender.
- Mounting position and enclosure design.
- ~~Exact ignition-switched 12 V tap point~~ — **moot**: the USB socket is
  constant and is a legitimate install (section 3, Power).
- The car will not answer mode 09, so the VIN is permanently empty and
  identification falls back to the configured make/model. Not a bug.

---

## 7.5 Backlog — new feature requests (opened 2026-08-12)

Five items raised this session. We work them **one at a time**; each lands here
first so nothing gets lost, then graduates into section 5/6/7 once built.

| # | Item | Status |
|---|---|---|
| B1 | Boot splash on ignition on (shutdown dropped — see below) | **shipped (simulator) — §14** |
| B2 | In-UI view to browse and pick a past drive to replay | **shipped (simulator) — §12** |
| B3 | Define the driving score: what is "spirited", what is "harsh"? | **open question — see below** |
| B4 | Does OBD expose convertible-roof up/down? | **answered: NO — see below** |
| B5 | Make it a *universal* gauge; show car make/model at the top | **shipped in the simulator — §10** |

### B1 — Boot splash

Shipped as a boot splash only. The shutdown half was dropped on 2026-08-16:
the gauge is fed from an **ignition-switched** supply (§3, Power), so key-off
cuts power instantly and there is no frame left to draw a shutdown in. See §14.

Ignition itself is no longer inferred from rpm — `mx5gauge/ignition.py` reads
it from `volts` and `run_time` (§15).

### B2 — Replay picker view

Today past drives are listed only via the CLI (`run.py --sessions`). This adds
an **on-screen** way to scroll a list of recorded drives (date, duration,
distance, score) and pick one to replay — the touch-carousel equivalent of the
sessions list. Simulator-first; on hardware it reads the SD-card log index.

### B3 — Driving-score definition (open — needs a decision)

**Current logic** (`metrics.py`, all host-tested) blends three 0–100 sub-scores:

- **smooth (0.40):** average throttle jerk (%/s); 0 → 100, 12 %/s → 0.
- **econ (0.30):** time in the 1200–2600 rpm band + average instant
  consumption. The display quotes **km/L**; the score keeps working in L/100km,
  where its 5→100 / 15→0 band is tuned. `metrics.km_per_l` is the only place
  the two units meet.
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
   <http://127.0.0.1:8420> playing the newest drive in `logs/`.
4. `.venv/bin/python run.py --sessions` — lists any drives recorded locally
   (they're gitignored, so a fresh clone shows none).

Drives and captures both stay local by design — they're personal telemetry, so
a fresh clone has nothing to replay until you record a drive with `--live` or
drop a `.brc` into `captures/`. A sample capture used to ship for that reason;
it was borrowed telemetry from before the gauge could record its own, and it
has been cleared out now that there are real drives to use instead.

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
a capture without torque channels: exactly one view (Power) gates, because it holds
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

**The six stat cells read two ways, chosen by where the handle sits** (added
2026-08-20). Parked at the far end — where the card opens — the drive is over,
so they answer what it came to: `PEAK RPM`, `PEAK SPEED`, `DISTANCE`, then
`PEAK COOLANT`, `PEAK INTAKE`, `PEAK CAT`. Scrub back and they revert to the
live readings at that moment. Peaks are deliberately not shown mid-scrub: there
they would describe only the part replayed so far, which reads as a finished
summary of a drive that has not finished.

The peaks cost no extra bookkeeping, and that is a consequence of the seek
primitive above. Seeking replays the log from the start, so the gauge's running
maxima at the end already *are* the whole drive's peaks — the card reads them
straight out of `derived`. `Gauge.peaks` is a dict keyed by summary field
rather than an attribute per channel, because catalyst temperature arrives on
any of five bank/sensor channels (`state.CATALYST_KEYS`) and the drive's peak
is the hottest of whichever the car reported. The marks sit *behind* the
plausibility gate: a maximum is the one statistic a single garbage frame can
pin for an entire drive. `peak_rpm` still answers `0.0` before any rev, because
the tacho and score footer draw it unconditionally; the four new fields answer
`None`, which the card shows as `—` rather than lying with a bold `0`.

Because `run.py:drive_summary()` writes `snapshot()['derived']` wholesale, the
peaks land in each drive's `.json` sidecar with no change to the recorder. They
are **not** in the C++ core (§ firmware): the port still holds only `peak_rpm`
and `peak_kw`, so `verify_port.sh` does not cross-validate them.

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

---

## 14. The boot splash (B1, shipped in the simulator)

The gauge holds a splash for `BOOT_MS` (2.5 s) when it wakes, then reveals the
carousel. The instruments are **withheld rather than covered**: nothing shows
until the splash finishes, which is how a cluster behaves and which also spares
you the first second of half-populated channels.

`state.Boot` is a two-phase tracker — `WAKING` then `RUNNING` — fed the drive's
**logical** clock, the same one the metrics use. A capture replayed at 8x
therefore splashes for 2.5 s *of the drive*, not of your afternoon, and live
and replay need no special cases between them. Scrubbing backwards rebases the
start rather than stranding the splash waiting out an interval that has already
gone by.

There is deliberately no `ASLEEP` phase. A car that is off is a board with no
power, not a board in a state, and modelling something the hardware cannot be
in would be fiction.

### Why there is no shutdown animation — *superseded 2026-08-27*

> **The premise below was measured and is false.** It assumed §3 fed the board
> from an ignition-switched source, making key-off a hard power cut with no
> frame left to draw in. The board was then run from the car's own USB socket
> and **stayed lit with the key out**: the socket is constant. There is power
> after key-off, and there is a reliable trigger for it (the adapter stops
> answering within ~15 s). A shutdown animation is therefore possible, and it
> arrives with the sleep behaviour the constant supply now makes necessary —
> see §3's Power note and §7's first Next item. The original reasoning is kept
> below because the consequence it forced, in the following subsection, is real
> and still load-bearing.

`SPEC.md` §3 feeds the buck converter from an ignition-switched source. Key-off
is a hard power cut, so a shutdown animation could never be seen. This was
weighed against moving to constant 12 V with a deep-sleep backstop, and against
a hold-up capacitor to outlive the cut; switched power won on simplicity and
on parked-car draw.

### The consequence that had to be fixed

`Recorder.close()` writes the `.json` sidecar holding distance, economy and
score — and a power cut never reaches it. Without a sidecar `library.py` can
only count rows, so **every drive in the car would have shown as
`interrupted`** with no distance, exactly as the 13 Aug log does.

So the recorder now rewrites the sidecar every `SIDECAR_SECONDS` (10) as the
drive goes, via a `summary_fn` the caller supplies. `close()` still writes the
final one, so a clean shutdown is unchanged and a cut costs at most ten seconds
of summary. The periodic write happens outside the recorder's lock, because
`summary_fn` reaches into `Gauge.snapshot()` and takes the gauge's — the two
must never be acquired in both orders.

### The animation

`mx5gauge/web/boot.mp4` — the MX-5 rising out of darkness, headlights igniting,
then a push-in. User-supplied, 2026-08-16. The source clip was portrait
560x752 and 4.06 s; it is centre-cropped square at y=96 (which also removes a
watermark in the original's top-left), scaled to 480x480, sped up to
`BOOT_MS`, and stripped of its audio track — the board has no speakers and
browsers refuse to autoplay sound anyway.

The phase tracker still owns the timing. The video is only what gets drawn
during `WAKING`, and `drawBoot()` survives as the fallback: if the asset cannot
load or the browser refuses to autoplay it, the ring sweep plays rather than a
black screen.

**This is a simulator asset.** The ESP32-S3 cannot decode H.264 — the firmware
port needs the same animation as extracted frames or an LVGL sequence, which is
phase 2 work.

## 15. Drive recording

Every drive the board ever sees while nobody is watching — a commute with the
phone in a pocket, a wife or a mechanic driving it, an overnight forgotten
`idf.py monitor` window closed — is worth being able to go back and look at.
So the board keeps its own flight recorder, independent of the Mac, the BLE
link and the desk app: as long as the vLinker is answering, every reading
gets written to flash, and a Mac only needs to show up occasionally to take
what's new.

### What's recorded, and where

A dedicated `logs` partition (`firmware/partitions.csv`), sized 0x9F0000
(≈9.9 MB), is carved out of flash as a flat ring of 4096-byte sectors —
`gauge::LogBuf` in `firmware/components/gauge_core/logbuf.h`/`.cpp`. There is
no filesystem in it: no directory table, no wear-levelling layer, nothing
that has to be mounted or can get corrupted independently of the data. A
sector holds a small header plus as many 12-byte records as fit
(`kRecordsPerSector`), and a record is `u32 t_ms, u16 chan, u16 pad, f32
value` — the same 12 bytes `tools/build_drive_asset.py` and the desk replay
already read, so nothing downstream had to change shape to accept flash
instead of a Mac.

While a drive is open, `firmware/main/drive_log.cpp` records every PID the
poll loop reads (§3's decode table, all of it — not a curated subset), about
12 readings a second measured across the four real logs, plus the board's
own IMU sampled at 5 Hz across 4 channels — 20 records a second, the larger
half of the combined rate. That is deliberate: at 10 Hz the IMU alone would
be 40 records/s, pushing the combined rate to 52 rec/s and the retained
history down to about 4.6 hours, for IMU resolution the driving-score maths
in §7.5 B3 doesn't need.

At 32 records/s combined × 12 bytes/record = 384 B/s ≈ 1.38 MB/hour, and the
`logs` partition's 2544 sectors give 10,379,520 usable bytes once each
sector's 16-byte header is subtracted (0x9F0000 × 4080/4096) — so the ring
holds roughly **7.5 hours** of driving before the oldest record has to make
room for the newest — see "wipe-ahead" below.

### Drive boundaries

A drive starts on the first reading after the car goes quiet, and ends after
**20 seconds of silence** (`kSilenceUs` in `drive_log.cpp`) — long enough that
a red light or a stalled queue doesn't split one drive into two, short enough
that key-off (which the vLinker itself notices within about 15 s, measured in
the car) reliably closes it. A drive under 100 records (`kMinDriveRecords` in
`logbuf.h`) is a key bump, not a drive, and is filtered out of what `LIST`
reports — it is never actively erased, because erasing mid-ring would take a
live sector with it; it is just never offered.

The ring is wipe-ahead: `begin_drive()` always advances onto a fresh sector,
erasing it first if that means overwriting the oldest drive still held. So
the partition is self-cleaning — nobody has to remember to clear it, and it
never fills up and stops recording; it just quietly forgets the oldest drive
to make room for the newest, oldest dropped first.

### Time, and what `drive-unknown-N` means

The board has no RTC and no network — it only knows the time of day if a Mac
tells it. `TIME <epoch>` (below) sets an in-RAM clock and also persists it to
NVS every five minutes, so a reboot with no Mac nearby still has a floor to
date a drive's records after. A drive that starts before the clock is ever
set — first power-up after a blank build, or a long stretch with no Mac
around — records with epoch 0. `tools/pull_drives.py` writes that drive as
`drive-unknown-<id>.csv` with its `iso` column left empty, rather than
guessing: a clearly-labelled gap is more useful later than a confidently
wrong timestamp quietly poisoning a replay.

The drive-start marker record (reserved channel id `kChanDriveStart`,
`0xFFFF`) carries that epoch, but not as a normal reading — see the comment
at its definition in `logbuf.h`. A 12-byte record has no field to spare for a
proper timestamp, so the marker's `value` field carries the epoch as the raw
bits of a `uint32_t`, not that number converted to a float (a float only has
24 bits of mantissa; converting a Unix timestamp through one rounds to the
nearest ~128 seconds, silently). Nothing downstream needs to unpack this —
`LIST` already reports each drive's epoch decoded correctly — but any code
that ever reads a marker's value directly must treat those 4 bytes as a
`uint32_t`, not a `float`.

### Retrieval — the console, and the puller

The recorder is read back over the same USB console the board already
exposes, five plain-text commands (`firmware/main/serial_cmd.cpp`):

- `TIME <epoch>` — lends the board the caller's clock.
- `STATS` — sectors in the partition and sectors actually used, the bytes
  those come to, drive-start markers, record count, samples dropped, flash
  writes that failed, the borrowed clock and the NVS clock floor, and a
  channel-table version (so a caller that disagrees about what channel id 9
  means refuses to guess rather than mislabel a column). `starts=` is an
  upper bound on drives, not an answer: it counts drive-open markers, is
  never decremented when the ring drops a drive, and includes drives too
  short to be offered. `LIST` is the authority on what can be pulled.
- `LIST` — one line per drive held, newest first: id, epoch, record count,
  duration, whether it ended cleanly, and the channel-table version it was
  recorded under. Its `OK N drives truncated=0|1` terminator says whether
  there were drives it had no room to report — "N drives" and "N drives and
  more I cannot show you" must not look the same.
- `GET <id>` — streams a drive's records out as base64, three records per
  line, followed by a record count and a crc32 the reader must check before
  trusting anything it received. A drive recorded under a different channel
  table is refused outright rather than streamed out to be mislabelled.
- `ERASE CONFIRM` — wipes the whole ring.

`tools/pull_drives.py` drives all of this: it sets the board's clock from the
Mac's, lists what's held, and pulls anything not already sitting in `logs/`
into `logs/drive-YYYYmmdd-HHMMSS.csv` — the same four-column `iso,t,key,value`
shape every other tool here already reads, so a drive nobody watched can be
replayed with `run.py --replay` exactly like a normal capture, and folded
back into `build-assets/drives.bin` with `tools/build_drive_asset.py`. It
verifies `GET`'s crc32 and record count itself and refuses to write a
half-pulled drive into `logs/` as though it were whole. Run it with
`--list` to see what the board is holding without pulling anything, and
`--force` to re-pull a drive already on disk.

**The console is shared with `idf.py monitor`.** Only one program can have
the USB-CDC port open at a time — leaving a monitor window running is the
most likely reason the puller can't connect, and it says so rather than
hanging.
