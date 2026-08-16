> **SUPERSEDED — do not execute.** Written before ignition detection existed.
> It adds a class named `Ignition` to `state.py` and creates
> `tests/test_ignition.py`; both now exist and mean something else
> (`mx5gauge/ignition.py`, shipped 2026-08-16). It also infers ignition from
> rpm plus link state, which the `volts`/`run_time` detector replaced, and it
> builds a shutdown animation that the power decision of 2026-08-16 rules out.
> Current design: `docs/superpowers/specs/2026-08-16-boot-splash-design.md`.

# Ignition Animations (B1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a boot splash when the car wakes and a shutdown animation when it sleeps, driven by a pure, host-tested ignition state machine.

**Architecture:** A pure `Ignition` state machine in `mx5gauge/state.py` is fed rpm and a `link_up` flag on every sample, using the drive's *logical* clock so it behaves identically live and on a sped-up replay. `Gauge.snapshot()` exposes `ignition = {phase, progress}`. A full-screen round canvas overlay in `index.html` draws the placeholder animation from those two numbers and hides the carousel unless the phase is `RUNNING`. No timers anywhere but the machine — the UI is a pure function of the snapshot.

**Tech Stack:** Python 3 stdlib only; vanilla JS + `<canvas>` in one HTML file. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-08-12-ignition-animations-design.md`

## Global Constraints

- **Simulator only.** No firmware, no LVGL, no deep-sleep entry. The state machine is written so it ports, and nothing more.
- **Placeholder art, clearly labelled as such.** The real asset is user-supplied later.
- **Python 3 stdlib only.** No new packages; `requirements.txt` is unchanged.
- **Tests are plain scripts,** matching the existing five: a `check`/`near` helper, printed lines, `sys.exit(1)` on failure. No pytest — it is not installed.
- **Tunables, exact defaults:** `IGN_OFF_SECONDS = 3.0`, `STARTUP_MS = 2500`, `SHUTDOWN_MS = 1500`.
- **Phase names, exact strings:** `'ASLEEP'`, `'STARTING'`, `'RUNNING'`, `'STOPPING'`.
- **Logical time, never wall-clock.** The machine is fed the same `t` that `Gauge.sample()` receives — the capture's own timeline during replay. A replay at 8x must not shut down 8x sooner.
- **Gaps over 5.0 s reset the zero-hold** rather than counting toward it, matching the existing `metrics.Trip` convention (`metrics.py:44`).
- **Shutdown has one rule for live and replay alike:** rpm held at 0 for `IGN_OFF_SECONDS`, or the live link dropping. Reaching the end of a replay does **not** shut down, and neither does parking on the drive card — that is a review state, not an ignition event. (This supersedes the spec's "replay hit end-of-file" row, decided 2026-08-16.)

---

### Task 1: The ignition state machine

**Files:**
- Modify: `mx5gauge/state.py` (add `Ignition` above `class Gauge`)
- Test: `tests/test_ignition.py` (create)

**Interfaces:**
- Consumes: nothing — pure, no imports beyond the stdlib.
- Produces:
  - `state.IGN_OFF_SECONDS = 3.0`, `state.STARTUP_MS = 2500`, `state.SHUTDOWN_MS = 1500`
  - `state.Ignition()` with:
    - `.phase` → one of `'ASLEEP' | 'STARTING' | 'RUNNING' | 'STOPPING'`, starts `'ASLEEP'`
    - `.update(t, rpm, link_up)` → `None`. `t` is logical seconds (float), `rpm` is float or `None`, `link_up` is bool.
    - `.progress` → float 0.0–1.0 through the current animation; `0.0` in `ASLEEP`, `1.0` in `RUNNING`.
    - `.reset()` → back to `ASLEEP` with all timers cleared.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ignition.py`:

```python
"""Unit tests for the ignition state machine.

The machine is fed the drive's *logical* clock, so every test here drives it
with explicit timestamps rather than sleeping. A replay at 8x speed and a car
idling in front of you must produce identical transitions.
   Run: .venv/bin/python tests/test_ignition.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import state  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def near(name, got, want, tol=1e-6):
    ok = got is not None and abs(got - want) <= tol
    if not ok:
        FAILED.append('%s: got %r want ~%r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def run(ign, steps):
    """Feed (t, rpm, link_up) triples in order."""
    for t, rpm, link in steps:
        ign.update(t, rpm, link)
    return ign.phase


SPLASH = state.STARTUP_MS / 1000.0
FADE = state.SHUTDOWN_MS / 1000.0

# 1. Cold start
ign = state.Ignition()
check('starts asleep', ign.phase, 'ASLEEP')
near('asleep shows no animation', ign.progress, 0.0)
ign.update(0.0, 0.0, True)
check('link up but engine not turning stays asleep', ign.phase, 'ASLEEP')
ign.update(1.0, 800.0, True)
check('link up and revs turning starts the splash', ign.phase, 'STARTING')
ign.update(1.0 + SPLASH / 2, 800.0, True)
check('mid-splash still starting', ign.phase, 'STARTING')
near('...and reports half way through', ign.progress, 0.5, tol=0.02)
ign.update(1.0 + SPLASH + 0.01, 800.0, True)
check('splash done hands over to running', ign.phase, 'RUNNING')
near('running reports a finished animation', ign.progress, 1.0)

# 2. Idle blip at a light must not shut the gauge down
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
check('running before the blip', ign.phase, 'RUNNING')
t = SPLASH + 1.0
run(ign, [(t, 0.0, True), (t + state.IGN_OFF_SECONDS - 0.5, 0.0, True)])
check('a brief zero at a light stays running', ign.phase, 'RUNNING')
run(ign, [(t + state.IGN_OFF_SECONDS + 1.0, 900.0, True)])
check('revs returning clears the hold', ign.phase, 'RUNNING')

# 3. Engine off: rpm zero held long enough
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
t = SPLASH + 1.0
run(ign, [(t, 0.0, True), (t + state.IGN_OFF_SECONDS + 0.01, 0.0, True)])
check('rpm zero held long enough stops the gauge', ign.phase, 'STOPPING')
ign.update(t + state.IGN_OFF_SECONDS + FADE + 0.02, 0.0, True)
check('shutdown finishes back at asleep', ign.phase, 'ASLEEP')
near('asleep again shows no animation', ign.progress, 0.0)

# 4. The link dropping stops it immediately, revs or no revs
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
run(ign, [(SPLASH + 1.0, 3000.0, False)])
check('a dropped link stops the gauge even at 3000 rpm', ign.phase, 'STOPPING')

# 5. A gap in the data resets the hold rather than counting toward it
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
t = SPLASH + 1.0
run(ign, [(t, 0.0, True), (t + 60.0, 0.0, True)])
check('a 60s data gap does not count as engine-off time', ign.phase, 'RUNNING')
run(ign, [(t + 60.0 + state.IGN_OFF_SECONDS + 0.01, 0.0, True)])
check('...but the hold then accrues normally', ign.phase, 'STOPPING')

# 6. Missing rpm is not the same as zero rpm
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
run(ign, [(SPLASH + 1.0, None, True),
          (SPLASH + 1.0 + state.IGN_OFF_SECONDS + 0.01, None, True)])
check('a car that stops reporting rpm is not an engine that stopped',
      ign.phase, 'RUNNING')

# 7. Waking again after a shutdown
ign = state.Ignition()
run(ign, [(0.0, 800.0, True), (SPLASH + 0.1, 800.0, True)])
t = SPLASH + 1.0
run(ign, [(t, 0.0, True), (t + state.IGN_OFF_SECONDS + 0.01, 0.0, True),
          (t + state.IGN_OFF_SECONDS + FADE + 0.02, 0.0, True)])
check('asleep after the fade', ign.phase, 'ASLEEP')
run(ign, [(t + 100.0, 900.0, True)])
check('the next start splashes again', ign.phase, 'STARTING')

# 8. reset() puts it back to cold
ign.reset()
check('reset returns it to asleep', ign.phase, 'ASLEEP')
near('reset clears the animation', ign.progress, 0.0)

# 9. Time going backwards (a scrub) must not strand it mid-animation
ign = state.Ignition()
run(ign, [(500.0, 800.0, True)])
check('starting after a seek', ign.phase, 'STARTING')
ign.update(10.0, 800.0, True)          # scrubbed back to earlier in the drive
check('time moving backwards leaves it in a sane phase',
      ign.phase in ('STARTING', 'RUNNING'), True)
near('progress stays within bounds',
     max(0.0, min(1.0, ign.progress)), ign.progress)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all ignition tests passed')
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv/bin/python tests/test_ignition.py`
Expected: FAIL with `AttributeError: module 'mx5gauge.state' has no attribute 'IGN_OFF_SECONDS'`

- [ ] **Step 3: Write the implementation**

In `mx5gauge/state.py`, add after the `plausible()` function and before `class Gauge`:

```python
# Ignition animation timings. Seconds for the hold (it is measured against the
# drive's own clock); milliseconds for the animations (they are wall-clock
# things the UI draws).
IGN_OFF_SECONDS = 3.0
STARTUP_MS = 2500
SHUTDOWN_MS = 1500

# A gap longer than this is a pause in the recording or a stalled link, not
# time the engine spent switched off. Same rule and same number as metrics.Trip.
IGN_MAX_GAP = 5.0


class Ignition(object):
    """Tracks whether the car is waking, running, sleeping, or asleep.

    Pure and host-testable: it is told the time rather than reading a clock, so
    the same transitions happen live, on a replay at 8x, and in a test. The
    board will drive its LVGL splash and its deep-sleep backstop from exactly
    this machine.

    Ignition is not a signal any car publishes, so it is inferred:
      wake  = the link is up and the engine is actually turning
      sleep = the revs sit at zero long enough to mean "switched off",
              or the link goes away underneath us
    """

    def __init__(self):
        self.phase = 'ASLEEP'
        self._since = None      # logical time the current phase began
        self._zero_at = None    # logical time rpm first read zero
        self._last_t = None     # previous sample, to measure gaps

    def reset(self):
        self.__init__()

    @property
    def progress(self):
        """0 -> 1 through the current animation. Flat in the steady phases."""
        if self.phase == 'RUNNING':
            return 1.0
        if self.phase == 'ASLEEP' or self._since is None:
            return 0.0
        span = (STARTUP_MS if self.phase == 'STARTING' else SHUTDOWN_MS) / 1000.0
        if span <= 0:
            return 1.0
        return max(0.0, min(1.0, (self._elapsed) / span))

    def update(self, t, rpm, link_up):
        """Feed one sample. `t` is logical seconds, `rpm` may be None."""
        t = float(t)
        prev, self._last_t = self._last_t, t
        # A backwards jump is a scrub, not elapsed time. Rebase every timer on
        # the new clock so the machine keeps running rather than stranding
        # itself part-way through an animation that can never finish.
        if prev is not None and t < prev:
            shift = prev - t
            if self._since is not None:
                self._since -= shift
            if self._zero_at is not None:
                self._zero_at -= shift
        self._elapsed = 0.0 if self._since is None else max(0.0, t - self._since)

        gap = None if prev is None else (t - prev)
        turning = rpm is not None and rpm > 0

        if self.phase == 'ASLEEP':
            if link_up and turning:
                self._enter('STARTING', t)
        elif self.phase == 'STARTING':
            if not link_up:
                self._enter('STOPPING', t)
            elif t - self._since >= STARTUP_MS / 1000.0:
                self._enter('RUNNING', t)
        elif self.phase == 'RUNNING':
            if not link_up:
                self._enter('STOPPING', t)
            else:
                self._track_zero(t, rpm, gap)
        elif self.phase == 'STOPPING':
            if t - self._since >= SHUTDOWN_MS / 1000.0:
                self._enter('ASLEEP', t)
        self._elapsed = 0.0 if self._since is None else max(0.0, t - self._since)

    def _track_zero(self, t, rpm, gap):
        """Accrue engine-off time, honestly.

        `rpm is None` means the car stopped answering, which is a different
        thing from an engine that stopped turning — a car mid-reconnect must
        not be treated as switched off. Only a real zero counts.
        """
        if rpm is None:
            return
        if gap is not None and gap > IGN_MAX_GAP:
            # nothing was observed across that gap, so nothing is known about
            # it; start the hold again from here
            self._zero_at = t if rpm <= 0 else None
            return
        if rpm > 0:
            self._zero_at = None
        elif self._zero_at is None:
            self._zero_at = t
        elif t - self._zero_at >= IGN_OFF_SECONDS:
            self._enter('STOPPING', t)

    def _enter(self, phase, t):
        self.phase = phase
        self._since = t
        self._elapsed = 0.0
        if phase != 'RUNNING':
            self._zero_at = None
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `.venv/bin/python tests/test_ignition.py`
Expected: PASS, ending `all ignition tests passed`

- [ ] **Step 5: Run the whole suite — nothing else may move**

Run: `for t in tests/test_*.py; do .venv/bin/python "$t" >/dev/null 2>&1 && echo "PASS $t" || echo "FAIL $t"; done`
Expected: all six PASS.

- [ ] **Step 6: Commit**

```bash
git add mx5gauge/state.py tests/test_ignition.py
git commit -m "Infer ignition state from the revs and the link"
```

---

### Task 2: Wire the machine into the gauge and the snapshot

**Files:**
- Modify: `mx5gauge/state.py` (`Gauge.__init__`, `Gauge.reset`, `Gauge.sample`, `Gauge.snapshot`)
- Modify: `mx5gauge/sources.py` (`ReplaySource.run`, `LiveSource.run` — report link state)
- Test: `tests/test_ignition.py` (append a Gauge-level section)

**Interfaces:**
- Consumes: `state.Ignition` from Task 1.
- Produces:
  - `Gauge.ignition` → an `Ignition` instance, updated on every non-meta sample.
  - `Gauge.link_up` → bool, default `False`; set by sources via the `_link` meta key.
  - Snapshot gains `'ignition': {'phase': str, 'progress': float}`.
  - Sources emit `on_sample('_link', True)` when producing data and `on_sample('_link', False)` when the link goes away.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_ignition.py`, immediately **before** the final `print()` / `if FAILED:` block:

```python
# --- driven through the Gauge, the way the app does it -------------------
g = state.Gauge()
check('a fresh gauge is asleep', g.ignition.phase, 'ASLEEP')
check('snapshot carries the phase', g.snapshot()['ignition']['phase'], 'ASLEEP')

g.sample('_link', True)
g.sample('rpm', 850.0, 0.0)
check('a running engine wakes the gauge', g.ignition.phase, 'STARTING')
g.sample('rpm', 850.0, SPLASH + 0.1)
check('and reaches running after the splash', g.ignition.phase, 'RUNNING')
snap = g.snapshot()
check('snapshot phase follows', snap['ignition']['phase'], 'RUNNING')
near('snapshot progress follows', snap['ignition']['progress'], 1.0)

g.sample('rpm', 0.0, SPLASH + 1.0)
g.sample('rpm', 0.0, SPLASH + 1.0 + state.IGN_OFF_SECONDS + 0.01)
check('switching off stops it', g.ignition.phase, 'STOPPING')

# metadata must not drive the machine — only real readings do
g2 = state.Gauge()
g2.sample('_link', True)
g2.sample('_car', {'label': 'MX-5'})
check('metadata alone does not wake it', g2.ignition.phase, 'ASLEEP')

# loading another drive starts from cold
g.reset()
check('reset clears the ignition too', g.ignition.phase, 'ASLEEP')

# a channel that is not rpm still advances the clock, carrying last-known rpm
g3 = state.Gauge()
g3.sample('_link', True)
g3.sample('rpm', 800.0, 0.0)
g3.sample('speed', 40.0, SPLASH + 0.2)
check('any channel can carry the machine forward', g3.ignition.phase, 'RUNNING')
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv/bin/python tests/test_ignition.py`
Expected: FAIL with `AttributeError: 'Gauge' object has no attribute 'ignition'`

- [ ] **Step 3: Implement the Gauge wiring**

In `mx5gauge/state.py`, in `Gauge.__init__`, after `self.score = metrics.DrivingScore()`:

```python
        self.ignition = Ignition()
        # whether the source is currently producing data: the BLE link for a
        # live drive, the playback loop for a replay
        self.link_up = False
```

In `Gauge.reset`, after `self.score = metrics.DrivingScore()`:

```python
            self.ignition.reset()
```

In `Gauge.sample`, replace the metadata early-return block:

```python
            if meta:
                self.values[key] = value
                return
```

with:

```python
            if meta:
                self.values[key] = value
                if key == '_link':
                    self.link_up = bool(value)
                    # a link that has just gone away must be acted on now, not
                    # at the next reading — there may never be another one
                    if not self.link_up and self._t0 is not None:
                        self.ignition.update(t, self.values.get('rpm'), False)
                return
```

In `Gauge.sample`, after the existing `self.score.update(...)` call:

```python
            self.ignition.update(t, v.get('rpm'), self.link_up)
```

In `Gauge.snapshot`, add to the returned dict after `'replay': self._replay_position(),`:

```python
                'ignition': {'phase': self.ignition.phase,
                             'progress': self.ignition.progress},
```

- [ ] **Step 4: Implement the source link reporting**

In `mx5gauge/sources.py`, in `ReplaySource.run`, replace:

```python
        on_sample('_car', self.car)
        on_sample('_supported_keys', self.supported_keys)
```

with:

```python
        on_sample('_car', self.car)
        on_sample('_supported_keys', self.supported_keys)
        # a replay's "link" is simply whether it is playing; parking on the
        # drive card is a review state and deliberately leaves it up, so
        # opening a drive does not read as switching the car off
        on_sample('_link', True)
```

In `mx5gauge/sources.py`, in `LiveSource.run`, after the existing `on_sample('_car', self.car)`:

```python
                    on_sample('_link', True)
```

and in the same method, immediately after the `async with BleakClient(...)` block ends — that is, just before `self.reconnects += 1`:

```python
                on_sample('_link', False)
```

- [ ] **Step 5: Run the tests**

Run: `.venv/bin/python tests/test_ignition.py`
Expected: PASS

Run: `for t in tests/test_*.py; do .venv/bin/python "$t" >/dev/null 2>&1 && echo "PASS $t" || echo "FAIL $t"; done`
Expected: all six PASS.

- [ ] **Step 6: Verify against a real replay**

```bash
lsof -ti tcp:8420 | xargs kill 2>/dev/null; sleep 2
.venv/bin/python run.py --replay last --speed 8 --model "MX-5" --no-browser &
sleep 4
curl -s localhost:8420/data | .venv/bin/python -c "import json,sys;d=json.load(sys.stdin);print(d['ignition'])"
```
Expected: `{'phase': 'RUNNING', 'progress': 1.0}` — the splash has already finished by then. Kill the server afterwards.

- [ ] **Step 7: Commit**

```bash
git add mx5gauge/state.py mx5gauge/sources.py tests/test_ignition.py
git commit -m "Feed the ignition machine from the gauge and report link state"
```

---

### Task 3: The overlay — placeholder splash and shutdown

**Files:**
- Modify: `mx5gauge/web/index.html` (markup after `<div class="track">…</div>`, CSS near `.glow`, JS near `applyReplayPosition`)

**Interfaces:**
- Consumes: `snapshot.ignition = {phase, progress}` from Task 2.
- Produces: `applyIgnition(ign)`, called from `tick()`; a `<canvas id="ignfx">` overlay; `.screen.booting` gating the carousel.

- [ ] **Step 1: Add the markup**

In `mx5gauge/web/index.html`, immediately after the closing `</div>` of `<div class="track" id="track">` and before the closing `</div>` of `.screen`:

```html
    <!-- PLACEHOLDER ignition animation. Real asset drops in here later; the
         machinery around it (phase, progress, carousel gating) is the point. -->
    <canvas class="ignfx" id="ignfx" width="480" height="480" hidden></canvas>
```

- [ ] **Step 2: Add the CSS**

In the `<style>` block, immediately after the `.glow{...}` rule:

```css
  /* Ignition overlay. Above the views and the glow, below the bezel's inner
     ring, and never interactive — it is a curtain, not a control. */
  .ignfx{position:absolute;inset:0;width:100%;height:100%;z-index:4;
    pointer-events:none;border-radius:50%;}
  .ignfx[hidden]{display:none;}
  /* the carousel is not merely covered, it is withheld: nothing of the
     instruments shows until the gauge has finished waking */
  .screen.booting .track,
  .screen.booting .carbar{opacity:0;transition:opacity 0.18s linear;}
```

- [ ] **Step 3: Add the drawing code**

In the `<script>` block, immediately **before** `/* ---- render from /data ---- */`:

```javascript
/* ---- ignition: waking and sleeping ------------------------------------
   PLACEHOLDER ART. A radial sweep fills the ring while a wordmark fades in,
   then hands over to the carousel; shutdown contracts the ring to nothing.
   Everything is a pure function of (phase, progress) so replacing the visual
   later touches this function and nothing else. */
const ignCanvas=$('ignfx');
const ignCtx=ignCanvas.getContext('2d');
let ignPhase='';

function drawIgnition(phase, p){
  const W=ignCanvas.width, C=W/2;
  ignCtx.clearRect(0,0,W,W);
  if(phase==='RUNNING'||phase==='ASLEEP') return;
  const starting = phase==='STARTING';
  // shutdown runs the same ring backwards, so the two read as one mechanism
  const f = starting ? p : 1-p;
  ignCtx.fillStyle='#0a0b0e';
  ignCtx.fillRect(0,0,W,W);
  const r=C*0.62;
  ignCtx.lineWidth=W*0.026;
  ignCtx.lineCap='round';
  ignCtx.strokeStyle='rgba(255,255,255,0.10)';
  ignCtx.beginPath(); ignCtx.arc(C,C,r,0,Math.PI*2); ignCtx.stroke();
  ignCtx.strokeStyle='#e1000a';
  ignCtx.beginPath();
  ignCtx.arc(C,C,r,-Math.PI/2,-Math.PI/2+Math.PI*2*Math.max(0,Math.min(1,f)));
  ignCtx.stroke();
  ignCtx.globalAlpha=Math.max(0,Math.min(1,f*1.6));
  ignCtx.fillStyle='#dbe8ff';
  ignCtx.textAlign='center';
  ignCtx.textBaseline='middle';
  ignCtx.font='600 '+(W*0.085)+'px ui-monospace,SFMono-Regular,Menlo,monospace';
  ignCtx.fillText(($('c_make').textContent||'OBD-II').toUpperCase(),C,C-W*0.02);
  ignCtx.globalAlpha=Math.max(0,Math.min(1,(f-0.35)/0.65));
  ignCtx.font='500 '+(W*0.030)+'px ui-monospace,SFMono-Regular,Menlo,monospace';
  ignCtx.fillStyle='#6b7280';
  ignCtx.fillText('PLACEHOLDER ANIMATION',C,C+W*0.075);
  ignCtx.globalAlpha=1;
}

function applyIgnition(ign){
  if(!ign) return;
  const busy = ign.phase!=='RUNNING' && ign.phase!=='ASLEEP';
  // ASLEEP is a black screen with nothing on it: the gauge is off, and
  // showing the instruments frozen behind a curtain would be a lie
  const dark = ign.phase==='ASLEEP';
  ignCanvas.hidden = !(busy||dark);
  screenEl.classList.toggle('booting', busy||dark);
  if(dark){
    const W=ignCanvas.width;
    ignCtx.clearRect(0,0,W,W);
    ignCtx.fillStyle='#0a0b0e'; ignCtx.fillRect(0,0,W,W);
  } else if(busy){
    drawIgnition(ign.phase, ign.progress);
  }
  if(ign.phase!==ignPhase){ ignPhase=ign.phase; }
}
```

- [ ] **Step 4: Call it from the poll**

In `tick()`, on the line immediately after `applyReplayPosition(d.replay);`:

```javascript
  applyIgnition(d.ignition);
```

- [ ] **Step 5: Verify the page still parses**

```bash
D=$(mktemp -d)
.venv/bin/python - <<'PY' > $D/ui.js
import re
h=open('mx5gauge/web/index.html').read()
print('\n'.join(re.findall(r'<script>(.*?)</script>', h, re.S)))
PY
node --check $D/ui.js && echo "JS parses OK"
```
Expected: `JS parses OK`

- [ ] **Step 6: Watch a real boot**

```bash
lsof -ti tcp:8420 | xargs kill 2>/dev/null; sleep 2
.venv/bin/python run.py --replay last --speed 8 --model "MX-5" &
```
Expected in the browser: a black screen with the red ring sweeping and `MAZDA MX-5` fading in, for ~2.5 s, then the carousel appears. Kill the server afterwards.

- [ ] **Step 7: Commit**

```bash
git add mx5gauge/web/index.html
git commit -m "Play a placeholder splash while the gauge wakes"
```

---

### Task 4: Document it and close B1

**Files:**
- Modify: `SPEC.md` (§7.5 table row for B1; new §14)
- Modify: `README.md` (behaviour list)

**Interfaces:**
- Consumes: everything above. Produces no code.

- [ ] **Step 1: Flip the B1 row in the backlog table**

In `SPEC.md` §7.5, replace the B1 row with:

```markdown
| B1 | Startup + shutdown animation on ignition on/off | **shipped (simulator, placeholder art) — §14** |
```

- [ ] **Step 2: Append §14 to `SPEC.md`**

```markdown
---

## 14. Ignition animations (B1, shipped in the simulator)

The gauge plays a boot splash when the car wakes and a shutdown when it
sleeps, so an ignition change is visibly acknowledged rather than inferred by
the driver from numbers appearing.

### Ignition is inferred, not published

No OBD channel reports the ignition, so `state.Ignition` derives it:

- **wake** — the source is producing data *and* the engine is actually turning.
- **sleep** — the revs sit at zero for `IGN_OFF_SECONDS` (3 s), or the link
  goes away underneath us.

Four phases, `ASLEEP → STARTING → RUNNING → STOPPING → ASLEEP`, with a
`progress` of 0→1 through whichever animation is playing.

### Three things the rule has to get right

- **Held, not instantaneous.** A single zero sample at a light is not an
  ignition event. The zero has to persist.
- **A gap is not engine-off time.** Nothing is known about a stretch where
  nothing was observed, so a gap over 5 s restarts the hold instead of
  counting toward it — same rule and same number as `metrics.Trip`.
- **No reading is not a zero reading.** A car mid-reconnect stops answering;
  that is not an engine that stopped turning, and only a real zero counts.

### One rule for live and replay

The machine is fed the drive's **logical** clock — the capture's own timeline
during replay — so a replay at 8x shuts down after three seconds of the
*drive*, not three seconds of your afternoon. Replay therefore needs no
special case: a log that ends with the engine switched off plays its shutdown
exactly where the real one happened.

Two things deliberately do **not** shut the gauge down: reaching the end of a
replay, and parking on the drive card. Opening a drive to read its summary is
a review state, not an ignition event, and blacking the card out the moment it
opened would be absurd.

### Placeholder art

The visual is a labelled placeholder — a ring sweep with the car's name fading
in, run backwards for shutdown. It is a pure function of `(phase, progress)`,
so dropping the real asset in touches `drawIgnition()` and nothing else. The
carousel is withheld rather than merely covered: nothing of the instruments
shows until the gauge has finished waking.

### What the board inherits

`STOPPING → ASLEEP` is exactly where the deep-sleep backstop gets armed on
hardware. The machine is pure and told the time rather than reading a clock,
so the firmware drives the same transitions from the same code path.
```

- [ ] **Step 3: Add a line to `README.md`**

In the behaviour list in `README.md`, immediately after the `**The backdrop tracks rpm**` bullet and its code block, add:

```markdown
- **It wakes and sleeps like a cluster.** A boot splash plays when the engine
  starts turning and a shutdown when it stops, inferred from the revs and the
  link since no OBD channel reports the ignition. The animation is a labelled
  placeholder for now. Tunables live at the top of `mx5gauge/state.py`:
  `IGN_OFF_SECONDS`, `STARTUP_MS`, `SHUTDOWN_MS`.
```

- [ ] **Step 4: Final check**

Run: `for t in tests/test_*.py; do .venv/bin/python "$t" >/dev/null 2>&1 && echo "PASS $t" || echo "FAIL $t"; done`
Expected: all six PASS.

Run: `grep -n "B1" SPEC.md | head`
Expected: the backlog row reads **shipped**.

- [ ] **Step 5: Commit**

```bash
git add SPEC.md README.md
git commit -m "Write up the ignition machine and close B1"
```

---

## Self-review notes

- **Spec coverage:** state machine (Task 1), the four transitions and both
  tunable sets (Task 1), link signal from sources (Task 2), snapshot exposure
  (Task 2), canvas overlay + carousel gating (Task 3), placeholder visuals for
  both directions (Task 3), all five spec test cases plus four more (Tasks 1–2).
- **Deliberate deviation:** the spec's `RUNNING → STOPPING` row lists "replay
  hit end-of-file" as a trigger. It is dropped — end-of-file and parking on the
  drive card are not ignition events, decided 2026-08-16. Recorded in the
  Global Constraints and in the §14 write-up.
- **Out of scope, unchanged:** the real asset, firmware/LVGL, deep-sleep entry,
  and anything touching the driving score or the other backlog items.
