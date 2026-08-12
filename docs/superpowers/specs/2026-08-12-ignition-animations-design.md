# B1 — Ignition on/off animations (design)

Backlog item **B1** from `SPEC.md` §7.5. Scope for this pass: build the
detection + playback machinery in the **Mac simulator** with a **placeholder**
startup/shutdown animation. The final animation asset is user-supplied later;
the firmware port is deferred (but the design is chosen so it ports cleanly).

## Goal

When the car wakes, the gauge plays a short **boot splash**, then reveals the
view carousel. When the car sleeps, it plays a shutdown animation and goes dark.
This mirrors how a real instrument cluster powers on/off and gives an obvious
visual confirmation that the gauge saw the ignition change.

## Ignition state machine

A pure, host-testable machine — the same states drive LVGL + deep-sleep on
hardware later.

```
ASLEEP ──(link up & rpm>0)──▶ STARTING ──(splash done)──▶ RUNNING
   ▲                                                          │
   └──────(shutdown done)── STOPPING ◀──(rpm=0 held / link down)┘
```

| From | Condition | To |
|---|---|---|
| `ASLEEP`   | live: BLE link up **and** rpm > 0 · replay: playback started | `STARTING` |
| `STARTING` | `STARTUP_MS` elapsed | `RUNNING` |
| `RUNNING`  | rpm = 0 held ≥ `IGN_OFF_SECONDS`, **or** link dropped, **or** replay hit end-of-file | `STOPPING` |
| `STOPPING` | `SHUTDOWN_MS` elapsed | `ASLEEP` |

Notes:
- **rpm = 0 held**, not instantaneous — a single dropped/zero sample at idle
  must not trigger shutdown. Uses the same "held for N seconds" idea already
  used elsewhere; gaps > 5 s reset the hold rather than count toward it.
- **Replay** deliberately triggers startup on play and shutdown at EOF so the
  animation is always visible on the desk without a car.
- On hardware, `STOPPING → ASLEEP` is exactly where the deep-sleep backstop
  (roadmap phase 1) gets armed. Not implemented in the simulator.

## Where it lives

| Piece | File | Detail |
|---|---|---|
| State machine | `mx5gauge/state.py` | New `Ignition` helper updated each tick from `rpm` + a `link_up` flag; exposed in the snapshot as `ignition = {phase, progress}` where `progress` is 0→1 through the current animation. |
| Link signal | `mx5gauge/sources.py` | Sources report `link_up` (BLE connected / replay running) so the machine can detect wake and link-drop. |
| Overlay UI | `mx5gauge/web/index.html` | A full-screen round `<canvas>` overlay, drawn from `ignition.phase`/`progress`. **Gating boot-splash**: the carousel is hidden while `phase ∈ {ASLEEP, STARTING, STOPPING}` and shown only in `RUNNING`. |

## Placeholder visuals (240×240 round)

Clearly labelled as placeholder art so the real asset drops in later.

- **Startup** (`STARTUP_MS ≈ 2500`): a radial sweep fills the ring one lap while
  an "MX-5" wordmark fades in, then hands off to the carousel.
- **Shutdown** (`SHUTDOWN_MS ≈ 1500`): the ring contracts/fades to black.

## Tunables

- `IGN_OFF_SECONDS` — rpm-zero hold before shutdown (default **3**).
- `STARTUP_MS` — startup splash duration (default **2500**).
- `SHUTDOWN_MS` — shutdown duration (default **1500**).

## Testing (host)

Pure state-machine tests in `tests/`:

1. Cold start: `ASLEEP` + link up + rpm 800 → `STARTING`, then `RUNNING` after `STARTUP_MS`.
2. Idle blip: brief rpm 0 (< hold) at a light → stays `RUNNING`.
3. Shutdown: rpm 0 held ≥ `IGN_OFF_SECONDS` → `STOPPING` → `ASLEEP`.
4. Link drop while running → `STOPPING`.
5. Data gap (> 5 s) resets the zero-hold rather than counting toward shutdown.

## Out of scope (this pass)

- The final/real animation asset.
- Firmware (LVGL) implementation and actual deep-sleep entry.
- Any change to the driving score, views, or other backlog items.
