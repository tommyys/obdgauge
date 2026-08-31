# The driving companion — design

**Date:** 2026-08-29
**Status:** approved design, **parked 2026-08-31**. No implementation plan yet.
Tommy is settling the driving logic (B3, the score definition) first, and this
gets revisited once that is firm. The design itself does not depend on B3 —
parking it is a sequencing choice, not a blocker.
**Related:** `SPEC.md` §6 (the views), §11 (rpm backdrop), §14 (boot splash budget),
B3 (driving score, still undecided — the companion does **not** depend on it)

---

## 1. What it is

A ninth view in the carousel: a cartoon MX-5 ND3 that shows how the car *feels*,
using data the other eight views show as numbers.

The reference is the **Dasai Mochi 3** — an animated dashboard companion that
reacts to driving through a gyroscope. This design keeps the idea and changes the
input: Mochi only feels the road, and this gauge is already reading 34–50 live
channels from the engine. Coolant at 52 °C makes the car shiver. 6800 rpm makes
it grin.

**Decisions taken during brainstorming, so they are not re-litigated:**

| Question | Decision |
|---|---|
| Where it lives | **A ninth view** in the existing carousel, swipeable like any other |
| Its job | **The car's voice** — OBD data expressed as feeling, not a generic mood toy |
| What it looks like | **The MX-5 itself** — headlights are eyes, grille is the mouth |
| Sound | **Chirps and beeps**, no speech |
| IMU motion | **Phase 2.** Phase 1 is OBD-only and needs no car to test |
| Rendering | **Static body image plus face sprites** (approach B of three) |
| First art | **Generated placeholder**, redrawn by Tommy later |

**Non-goals.** No speech. No IMU reactions in phase 1. No dependency on the
driving score. No change to the eight existing views.

---

## 2. The mood vocabulary

Nine moods. Exactly one is active. Thresholds come from measured values on the
real car (2026-08-27, engine warm: idle ~770 rpm, coolant 89 °C, intake 62–66 °C,
throttle 13 %, load 27–36 %, 13.3–13.4 V, fuel rate 0.9–1.2 L/h).

| Priority | Mood | Trigger | Expression |
|---|---|---|---|
| 1 | **Overheating** | coolant ≥ 105 °C | eyes squeezed shut, steam, shake |
| 2 | **Sick** | volts < 12.0 with engine running, or coolant ≥ 100 °C | dim headlights, droop, worried mouth |
| 3 | **Cold** | coolant < 60 °C | shivering, half-shut eyes, breath fog |
| 4 | **Warming** | coolant 60–80 °C | waking, one eye open |
| 5 | **Straining** | load > 75 % or throttle > 80 % | eyes forward, gritted grille |
| 6 | **Thrilled** | rpm > 5500 | eyes wide, grin, headlight flash |
| 7 | **Dozing** | rpm below 1000 and throttle at 0 for more than 30 s | eyes closed, slow bob, zzz |
| 8 | **Content** | coolant 80–100 °C, volts normal, none of the above | relaxed blink, small smile |
| 9 | **Blind** | no live link, or replay ended | `x x` eyes, no reactions |

**Selection rules**

1. **Priority beats recency.** The lowest-numbered mood whose trigger holds wins.
   Overheating always wins.
2. **Hysteresis.** A candidate mood must hold for **2 s** before it replaces the
   current one. This stops flicker at a threshold edge.
3. **Missing channel means missing mood.** If the drive carries no coolant
   channel, Cold, Warming, Overheating and Sick's coolant half can never be
   chosen. This follows the same honesty rule as `gauge::view_available`.
4. **Dozing outranks Content deliberately.** At a warm idle both triggers hold.
   Content is the fallback, so it must sit below every mood that describes a
   more specific situation.
5. **Intensity.** Every mood carries a `0.0–1.0` intensity, mapped linearly across
   the trigger's range (Thrilled: 5500 rpm = 0.0, redline = 1.0). Intensity drives
   pixel offsets and glow, never new art.
6. **Blind is the boot default.** The face starts blind and earns an expression
   only once real data arrives.

---

## 3. Architecture

```
VehicleState ──> gauge_core/mood ──> {mood, intensity} ──> companion view
   (PIDs)          pure function                            (sprites + sound)
```

The view never reads a PID. The mood unit never touches LVGL. A threshold change
touches no UI code; a redraw touches no logic.

### New files

| File | Job |
|---|---|
| `firmware/components/gauge_core/mood.{h,cpp}` | Pure decision function plus the hysteresis latch. No LVGL, no hardware, host-testable. |
| `mx5gauge/mood.py` | The Python mirror. Same thresholds, same priorities. |
| `firmware/components/gauge_ui/companion.{h,cpp}` | The ninth view. Reads mood only. Named `companion`, not `face`, because `gauge_ui/face.{h,cpp}` already exists and means the dial face. |
| `tools/make_companion_art.py` | Draws the placeholder ND3 and every sprite as PNGs, reproducibly. |
| `tools/build_companion_asset.py` | Packs those PNGs into `build-assets/companion.bin` as RGB565 with an index header. |
| `mx5gauge/web/companion/` | The same PNGs, served to the simulator. |

### The interface

```c++
enum class Mood { Blind, Dozing, Content, Thrilled, Straining,
                  Warming, Cold, Sick, Overheating };

struct MoodReading { Mood mood; float intensity; };

// Holds the 2 s latch between calls. One instance per running gauge.
struct MoodLatch { Mood candidate; uint32_t since_ms; Mood held; };

// `supported` is the car's reported channel set, or nullptr before the sweep
// finishes -- the same argument gauge::view_available already takes.
MoodReading mood_decide(const VehicleState& s,
                        const std::set<std::string>* supported,
                        uint32_t now_ms, MoodLatch& latch);
```

### Why it is not a `ViewSpec` row

`gauge_ui/views.h` describes views as a hero number, a dial `Instrument` and rows
or a grid. A sprite character is none of those. The companion is a bespoke view
in the carousel, in the same way the Drives view is, and it registers in
`carousel.h` alongside them.

---

## 4. Rendering

### Layout (466 × 466 round)

```
        .-----------------------.
      /     rpm glow backdrop     \      reuses the existing SPEC.md §11 backdrop
    |        __/\______             |
    |       /  O   O   \            |     headlights = eyes   (sprite)
    |      |   \____/   |           |     grille     = mouth  (sprite)
    |       '--o----o--'            |
    |          CONTENT              |     the mood word, small
      \      89°C · 13.4V          /       two vitals, so it is still a gauge
        '-----------------------'
```

The two vitals are coolant and volts, and they read `--` when the channel is
absent, exactly as the other views do.

### Assets

| Piece | Count | Size each | Flash |
|---|---|---|---|
| ND3 body, eye and mouth areas left blank | 1 | 300×180 | 108 KB |
| Eye pairs — open, half, closed, wide, squeezed, `x` | 6 | 64×48 | 36 KB |
| Mouths — smile, small, flat, gritted, grin, worried | 6 | 120×48 | 69 KB |
| Extras — steam, breath fog, zzz, headlight glow | 4 | 80×80 | 51 KB |
| **Total** | | | **~264 KB** |

### Where it sits in flash

The `assets` partition is 16 MB at `0x410000`. Only the part below flash
`0x1000000` is mmap-able (the ESP32-S3 MMU limit documented in
`firmware/main/drive_source.c`), which is about 12.5 MB. The companion's 264 KB
is 2 % of that, and sits far inside the limit.

The boot splash reads its frames at boot and is finished before the carousel
exists, so the two never contend.

### The redraw budget

Measured reference: a full-screen 466×466 transfer costs **19 ms** at the
vendored 80 MHz panel clock.

| Operation | Bytes | Cost |
|---|---|---|
| Full screen | 434 KB | 19 ms |
| Body blit (mood change only) | 108 KB | ~4.7 ms |
| Eye pair, per frame | 12 KB | ~0.5 ms |
| Mouth, per frame | 11 KB | ~0.5 ms |

**Consequence: 30 fps is comfortable.** The live data views manage 21–24 fps
because they redraw dials and text every frame; the companion redraws two small
rectangles.

### Rules the renderer must keep

1. **No heap allocation on the draw path.** Body and sprites are mmap'd and
   blitted straight from flash. This is the rule from the panel DMA trap, and
   undoing it kills the display permanently after the first swipe.
2. **The body redraws only on a pose change**, never per frame.
3. **Blinking is a sprite swap** — 120 ms of the closed pair at random intervals
   of 2–6 s. No new art, no logic in the mood unit.
4. **Intensity moves pixels, not sprites.** A stronger grin is the grin sprite
   lifted 2 px with a brighter headlight glow.
5. **Phase 2 motion is a blit offset.** Lean and squat shift the body by a few
   pixels, which is why deferring the IMU costs no art and no redesign.

---

## 5. Sound

- **Path:** ES8311 codec over I2S. The BSP declares `BSP_CAPS_AUDIO_SPEAKER 1`
  and `BSP_CAPS_AUDIO_MIC 1`; the microphone is unused.
- **Format:** mono 16 kHz WAV, under 20 KB each, packed into the same asset blob.
- **The set:** warm-up complete (two rising chirps), Thrilled (short trill),
  Straining (low growl), Sick (worried tone), Overheating (repeating alarm).
- **Rate limit:** one sound per mood *change*, and never more than one per 5 s.
  A mood that re-triggers without an intervening change stays silent.
- **Mute:** a tap anywhere on the companion view. Kept in NVS, so it survives a
  key cycle.
- **The one exception:** Overheating sounds even when muted. Interrupting is the
  entire point of that mood.

---

## 6. Degradation

| Failure | Behaviour |
|---|---|
| No live link yet, or link lost | Blind mood, `x x` eyes. Never invents feeling from stale data. |
| Coolant channel absent | Cold, Warming and Overheating are never selected. |
| Audio init fails | The view runs silent, logs once, and never retries. |
| Asset blob missing or short | The view draws the mood word and the two vitals as text only. The gauge never fails for want of a cartoon. |
| Mood function reaches an impossible state | Falls back to Content. |

---

## 7. Testing

1. **Host tests** (`firmware/test/host`) — a table of `VehicleState` → expected
   mood covering every threshold edge, every priority conflict, and the 2 s
   hysteresis timing. Runs on the Mac with no board attached.
2. **`tools/verify_port.sh`** — already replays real logs through the C++ and
   Python cores and diffs every channel. The mood field joins that diff, so the
   simulator and the board cannot drift apart silently.
3. **Threshold tuning against a real drive** — `logs/drive-unknown-1.csv` holds
   27.1 minutes of actual driving with 39 channels. Replay it and report how long
   each mood held. If one face holds for 25 of the 27 minutes, the thresholds are
   wrong and get tuned **before** any art is drawn.
4. **Simulator first** — the whole view works in the browser before anything is
   flashed. This is how every other view in this project was built.
5. **Bench check** — free heap and largest free block logged before and after 20
   swipes onto the companion view. A shrinking largest-free-block is the panel
   DMA trap returning, and free bytes alone do not reveal it.

---

## 8. Phases

**Phase 1 — OBD moods.** Everything above except motion. Testable end to end
with no car: the simulator replays a log, the board replays from flash.

**Phase 2 — IMU motion.** Lean into corners, squat under braking, bounce over
bumps. Blocked on one piece of knowledge: gravity reads on **Z, through the
screen**, but which of X and Y is longitudinal is unknown until the gauge is
driven in its final mounting orientation. Phase 2 is a blit offset and a
threshold table — no new art, no change to phase 1's structure.

**Explicitly not a phase.** The driving score (B3) stays out. It is an open
decision, and the companion must not be blocked behind it.
