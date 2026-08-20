# Getting the simulator onto the board

**Date:** 2026-08-20
**Status:** approved, ready to plan
**Scope:** Phase 0 (bring-up) through Phase 2 (full carousel)

## The goal, in one line

Everything the Mac simulator does today, running on the board in the car, with
no Mac attached.

The simulator was always the prototype of the firmware — `README.md` says so,
and `SPEC.md` §5 lists the module-by-module mapping. This document is how that
mapping actually gets executed against the hardware that now exists on the desk.

## What changed: the board is not the board in §3

`SPEC.md` §3 commits to a **Waveshare ESP32-S3-Touch-LCD-1.28**. The board
connected on 2026-08-20 is a **Waveshare ESP32-S3-Touch-AMOLED-1.75C**, and it
is the target from here.

Identified by dumping its flash and reading the ESP-IDF app descriptors and
component paths, not by guesswork:

- `components/esp32_s3_touch_amoled_1_75c/` — the board support package
- `esp_lcd_new_panel_co5300()` — CO5300 AMOLED driver over QSPI
- `waveshare__esp_lcd_touch_cst9217` — CST9217 touch
- `Qmi8658Ball.cpp`, "QMI8658 initialized success" — the IMU
- `ES8311` / `ES7210` — audio codec and 4-mic array
- Factory app `esp-brookesia`, OTA slot `xiaozhi v2.1.0`, ESP-IDF `v5.5.2`

| | Spec'd (1.28) | Actual (1.75C) | Consequence |
|---|---|---|---|
| Panel | 240×240 IPS, GC9A01, SPI | round AMOLED, CO5300, QSPI | new driver, ~4× pixels, every layout redrawn |
| Touch | CST816 | CST9217 | different driver, same gesture model |
| IMU | QMI8658 | QMI8658 | **unchanged** — score design survives |
| Flash | 16MB | 32MB | ample |
| PSRAM | 2MB | 8MB | framebuffers are comfortable |
| Audio | none | speaker + mics | unused; out of scope |

The IMU being identical is the important line in that table: the driving-score
design in §6 and the harsh-event work in B3 carry over untouched.

**§3 of `SPEC.md` must be rewritten** as part of this work, along with the BOM
(the board line changes, and the buck converter becomes optional — see Power).

### Panel geometry is not yet confirmed

Resolution lives in compile-time constants, so it was not recoverable from the
flash dump. The 1.75" Waveshare AMOLEDs are 466×466 round. **Phase 0 confirms
this against the real panel and the number is fixed once, in one header.** No
layout work starts before that. Every figure below that depends on resolution is
computed from 466×466 and is provisional until Phase 0 says otherwise.

## Strategy

A fresh ESP-IDF project in this repo, using the vendor board-support component
for the hardware, and porting the simulator's pure logic to C++ that compiles
for **both** the board and the Mac.

Rejected alternatives:

- **Fork the Waveshare demo and strip it.** Fastest to first pixel, but the
  gauge ends up wedged inside a vendor app scaffold and the host-testable split
  is hard to retrofit.
- **MicroPython + LVGL bindings.** Reuses `pids.py`/`metrics.py` nearly verbatim
  and is genuinely tempting, but a 466×466 panel with a continuously redrawn
  backdrop plus BLE is where it falls over, and §5 already commits to C++.

## Architecture

```
firmware/
  main/                  app entry, task wiring
  components/
    gauge_core/          PURE C++ — no ESP-IDF, no LVGL. Host-testable.
      pid.{h,cpp}        <- mx5gauge/pids.py
      metrics.{h,cpp}    <- mx5gauge/metrics.py
      state.{h,cpp}      <- mx5gauge/state.py  (VehicleState + RANGES)
      vehicle.{h,cpp}    <- mx5gauge/vehicle.py (VIN decode, dial profiles)
      elm327.{h,cpp}     <- protocol half of sources.py
      ignition.{h,cpp}   <- mx5gauge/ignition.py
    gauge_platform/      ESP-IDF only — BLE, IMU, power/sleep, NVS
    gauge_ui/            LVGL — screens, carousel, theme
  test/host/             gauge_core tests, plain CMake, run on the Mac
```

### The one rule

`gauge_core` may not include anything from ESP-IDF or LVGL. Plain C++ over plain
structs.

That is what lets it compile on the Mac, which is what lets the existing Python
tests become C++ tests. `SPEC.md` §5 calls the purity of `pids.py` and
`metrics.py` deliberate, "so the maths is proven before it ever runs on the
board" — this rule is that sentence, enforced by the build. A change that wants
to violate it means the design is wrong, not the rule.

### Module map

| Python | Firmware | Layer | Notes |
|---|---|---|---|
| `pids.py` (414) | `gauge_core/pid` | pure | table + decode formulas, direct port |
| `metrics.py` (271) | `gauge_core/metrics` | pure | economy, trip, score |
| `state.py` (374) | `gauge_core/state` | pure | `VehicleState`, range validation |
| `vehicle.py` (257) | `gauge_core/vehicle` | pure | VIN, `PROFILES`, honest view gating |
| `ignition.py` (92) | `gauge_core/ignition` | pure | drives deep-sleep on the board |
| `sources.py` (447) | **split** | both | ELM327 → pure; BLE transport → platform |
| `web/index.html` (1222) | `gauge_ui/` | LVGL | **reimplemented, not ported** |
| `server.py` (176) | — | — | no equivalent; HTTP was scaffolding |
| `recorder.py`, `library.py`, `brc.py` | — | — | SD logging is Phase 3 |

### Splitting `sources.py`

It currently mixes the ELM327 conversation (`ATZ`, `ATE0`, protocol negotiation,
response framing) with `bleak` transport. The protocol half is pure logic worth
testing on the host; the transport half is NimBLE on the board and `bleak` on the
Mac.

So it becomes an interface — `ITransport { write(); read(); }` — with two
implementations. `tests/test_sources.py` (199 lines) largely survives as host
tests against a fake transport.

### Runtime shape

```
NimBLE  ->  ITransport  ->  Elm327  ->  PidDecoder  ->  VehicleState
                                                            |
                                      QMI8658 IMU ---> Metrics (score, economy, trip)
                                                            |
                                                  LVGL carousel (9 views)
```

Two FreeRTOS tasks:

- **OBD task** — BLE, polling, decode, metrics
- **UI task** — LVGL tick and render

`VehicleState` is the handoff: written by one, read by the other under a mutex,
so a slow or dropped BLE response can never stall a redraw. This mirrors the
simulator, where the poll loop and the HTTP UI are already decoupled — and it
preserves the §4 promise that views degrade honestly, since a stale or missing
channel is visible in the state rather than papered over.

## Display and UI

### The stack

CO5300 over QSPI via the vendor BSP, LVGL on top. LVGL version follows whatever
the BSP pins; the board's existing firmware ships LVGL 9, so that is the working
assumption.

### Framebuffer budget

At 466×466 in RGB565: **434,312 bytes** per full frame (~424KB). Double-buffered
is ~848KB against 8MB PSRAM — not a memory problem.

**Bandwidth is the constraint, not capacity.** PSRAM is slower than internal
SRAM, and a full-frame blit per frame at 30fps is ~13MB/s of reads before any
drawing happens. The design therefore uses **partial/dirty-region rendering**
(LVGL's default) with small internal-SRAM draw buffers, rather than full-screen
PSRAM framebuffers, wherever LVGL allows it.

### The §11 rpm backdrop is the main performance risk

`SPEC.md` §11 specifies an rpm-reactive radial backdrop, chosen as `glow=rim`
after side-by-side comparison on the real gauge. On a 240×240 panel a full-screen
gradient per frame is cheap. At 466×466 it is roughly four times the fill, and
rpm changes constantly, so the naive implementation dirties the entire screen
every frame and defeats partial rendering.

Mitigation, in order of preference:

1. **Quantise rpm into buckets** (e.g. 100rpm steps). The backdrop only redraws
   when the bucket changes, not every frame. The visual difference is
   imperceptible at a glance; the saving is large.
2. **Precompute gradient rings** as a small set of image assets, and cross-fade
   or index between them rather than generating pixels at runtime.
3. **Restrict the backdrop to an annulus.** `glow=rim` already keeps the centre
   pure black — §11's whole rationale — so the dirty region can be the outer ring
   only, leaving the readouts undisturbed.

(3) composes with both (1) and (2), and follows directly from the visual decision
already made. **Phase 0 measures this** rather than assuming it; the exit
criterion is a real frame-rate number on the real panel.

### The nine views are a reimplementation

The simulator's UI is 112 `<div>`s, 4 `<svg>`s, one `<canvas>` and CSS
`radial-gradient` — DOM and CSS, not canvas drawing. There is no mechanical
translation to LVGL. The web UI is the **visual reference**; the LVGL screens are
new code.

This is the single largest piece of work in Phase 2, and the honest reason the
plan treats it as nine tasks rather than one.

Carried over unchanged from the existing design:

- The infinite carousel: each view placed at its shortest signed distance from
  the current one, so last-to-first is one slide (§6)
- Honest degradation: `--` or `n/a`, never a plausible zero; a fully unfed view
  says so outright (§4)
- Per-car dial scaling from `vehicle.PROFILES`, and the make/model banner (§10)

**View 9 (Drives) is reduced in Phase 2.** It browses and replays a drive library
that lives on SD, and SD logging is Phase 3. Phase 2 ships the view with the
current session only; the library lands with the card.

### Touch

CST9217 via the vendor driver, feeding LVGL's input device. Gestures needed are
horizontal swipe (carousel) and tap (the drive-card interactions §12 added). The
gesture model is unchanged from the simulator; only the driver differs.

### The boot splash

§14 shipped a boot splash in the simulator, and the memory note records that you
supply the real animation asset. On the board it becomes an LVGL animation or a
sequence of image frames in flash. There is no shutdown animation — §14 already
settled that, since key-off is a hard power cut.

## IMU and the driving score

The QMI8658 finally gets used. In the simulator, "harsh" is a speed-delta proxy —
laggy and coarse, as §4 admits and the backlog note repeats. On the board, real
acceleration is available at high rate.

Two consequences worth stating plainly:

- **The harsh thresholds must be re-tuned against the IMU.** The current
  `HARSH_ACCEL` / `HARSH_BRAKE` values are guesses calibrated against a different
  and worse signal. Porting the numbers unchanged would be a mistake.
- **B3 stays open and stays yours.** Whether "spirited" is a separate positive
  metric, a mode that reweights, or left out is a decision, not an
  implementation task. The firmware ports the score *as it stands* so nothing is
  blocked; when B3 is decided, it changes `gauge_core/metrics` in one place and
  the host tests prove it.

## Power and ignition

The backlog records an open question: whether the car's USB socket is
ignition-switched or constant. It needs a physical test at the car (plug a
charger in, key off, wait a minute) and it is not resolvable from here.

The firmware is designed for both:

- **Switched** — power simply disappears at key-off. `ignition.cpp` detecting
  engine stop is a clean-shutdown nicety, and deep-sleep is a backstop for a
  stalled link, per §3.
- **Constant** — deep-sleep on ignition-off becomes the primary mechanism and
  must actually work, or the board sits awake draining the battery.

Designing for both costs nothing: the same "engine has stopped" signal drives
either path. **The test result changes the BOM, not the code** — if the USB
socket is switched, the buck converter and fuse tap become optional and plugging
into the car's USB socket is a legitimate install.

## Testing

The whole point of the `gauge_core` purity rule is that the maths stays provable
without hardware.

| Layer | How it is tested | Where it runs |
|---|---|---|
| `gauge_core` | unit tests ported from `tests/` | Mac, plain CMake, in CI |
| `elm327` | fake `ITransport`, from `test_sources.py` | Mac |
| `gauge_platform` | manual bring-up checks, Phase 0 | board |
| `gauge_ui` | visual comparison against the simulator | board |

**Cross-validation is the strongest tool available and should be used
deliberately:** the simulator and the firmware consume the same captures. Feed a
`.brc` or CSV through both, and `VehicleState` and the metric outputs must agree.
Any divergence is a port bug, located precisely. This is worth building as a
harness in Phase 1, because it converts "does the C++ match the Python?" from a
judgement call into a diff.

The existing Python suite (~1000 lines) is the specification being ported. Where
a C++ test disagrees with its Python ancestor, the Python is right until proven
otherwise.

## Phases and exit criteria

**Phase 0 — bench bring-up (USB, no soldering).** ESP-IDF toolchain installed,
project skeleton builds, vendor BSP integrated. Confirm panel resolution. Display
a test pattern. Touch events logged. IMU readings logged. BLE connects to the
vLinker and completes the ELM327 handshake. **Measure the backdrop frame rate and
pick a mitigation.**
*Exit:* every subsystem individually proven on the real board, and a real fps
number for the backdrop.

**Phase 1 — firmware MVP.** `gauge_core` ported with host tests passing. The
cross-validation harness agrees with the simulator on a known capture. The
engine-vitals home screen (view 2) renders live from the car. Ignition-off
deep-sleep works.
*Exit:* the gauge shows correct live coolant, battery and intake in the car with
no Mac attached.

**Phase 2 — the carousel.** Touch swiping with the infinite-wrap rule. The
remaining views. Per-car dial scaling and the make/model banner. The boot splash
with your animation asset. View 9 limited to the current session.
*Exit:* all nine views on the board, matching the simulator's behaviour.

## Prerequisites

- **ESP-IDF is not installed on this Mac.** No `idf.py`, no `~/.espressif`, no
  `cmake`, no `ninja`. A ~2–3GB install, and a hard blocker on producing anything
  flashable. Pin the version to v5.5.x, matching what the board's own firmware
  was built with.

## Safety: the board is not blank

It currently runs a working Xiaozhi voice assistant. A full verified backup
exists at `backups/esp32s3-full-32MB.bin` (32MB, `verify-flash` digest matched,
SHA-256 recorded, restore steps in `backups/RESTORE.md`). `backups/` is
gitignored because the image contains the NVS partitions and therefore any
Wi-Fi credentials on that device.

Flashing the gauge will use its own partition table, which overwrites the
existing layout. That is recoverable — `write-flash 0` of the backup restores the
device exactly — but it is the reason the backup was taken first.

Neither flash encryption nor secure boot is enabled, and no eFuses are burned, so
nothing in this plan is one-way. **Do not run `espefuse burn_*` or enable
encryption/secure boot;** those are the only genuinely irreversible actions
available, and none of this work needs them.

## Out of scope

SD logging, Wi-Fi sync, GPS, shift LEDs, enclosure, permanent 12V install — all
Phase 3. The board's speaker and microphone array are unused. The Mac simulator
is not retired; it remains the design lab where views and score weights are
iterated against captures without a car.

## Open questions

1. **Panel resolution** — 466×466 assumed, confirmed in Phase 0.
2. **B3, the driving-score definition** — yours to decide; does not block.
3. **Car USB socket: switched or constant?** — physical test at the car; changes
   the BOM, not the code.
4. **Backdrop mitigation** — which of the three; decided by Phase 0 measurement.
