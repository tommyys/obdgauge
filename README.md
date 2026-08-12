# MX-5 ND3 OBD Gauge — Mac simulator

A working simulator of the round touch gauge, running on the Mac. It reads the
car live over Bluetooth LE, or replays a Car Scanner `.brc` capture at your
desk, and renders the eight-view carousel exactly as the ESP32 board will.

This is deliberately a **prototype of the firmware**, not a throwaway: the BLE
connection, the ELM327 handshake, the PID decode formulas and the metrics all
port more or less line-for-line to C++ on the board.

> **New here, or picking this up after a break?** Read **[SPEC.md](SPEC.md)**
> first — it covers the goal, the hardware choice, what the car does and doesn't
> expose, the design decisions and the roadmap. This README is the usage guide.

```
   BLE / replay  ->  ELM327  ->  PID decode  ->  VehicleState
                                                     |
                                          metrics (economy / trip / score)
                                                     |
                                           HTTP :8420 -> round gauge UI
```

## One-click launchers

Two files in the project root, both double-clickable, both symlink-aware so a
Desktop alias works:

- **`Gauge — REPLAY (desk).command`** — the one to use at your desk. No car, no
  hardware, no arguments: it picks the capture with the most actual driving,
  frees the port from any previous run, and opens the browser.
- **`Gauge — LIVE (in car).command`** — shows the pre-flight checklist, then
  connects to the vLinker over BLE.

`Ctrl-C` stops either one.

*(First double-click may show "unidentified developer" — right-click → Open once,
then it runs normally after that.)*

## Quick start (Terminal)

Replay one of your existing captures — no car, no hardware:

```bash
cd ~/Development/mx5-obd-gauge
.venv/bin/python run.py --replay
```

Then open <http://127.0.0.1:8420>. It auto-picks the capture containing the most
actual driving, plays it at 4×, and loops.

Options:

```bash
.venv/bin/python run.py --replay "captures/2026-08-11 21-43-36.brc" --speed 10
.venv/bin/python run.py --replay --speed 1        # real time
```

## Any car, not just the MX-5

OBD-II is universal; what each car *reports* is not. The gauge adapts:

- **The car names itself.** Live, it reads the VIN (mode 09 PID 02) and shows
  the make and model year on the display. A VIN cannot give a *model name* —
  that needs a commercial database — so pass it if you want it shown:

  ```bash
  .venv/bin/python run.py --live  --model "MX-5"
  .venv/bin/python run.py --live  --make Honda --model "Civic"   # force both
  ```

  Unknown manufacturer code shows `WMI XXX`; no VIN at all shows `OBD-II`.

- **The dials rescale.** Redline, rpm ceiling and the power-dial top come from
  a per-car profile in `mx5gauge/vehicle.py` (`PROFILES`) — an MX-5 gets
  8000/7000, a Lexus 6500/6000. Add a car by adding a row.

- **Views degrade honestly.** Each view knows which channels it needs. If the
  car doesn't report them, the view dims and says so rather than showing a
  convincing zero. On the sample capture the Power view gates itself, because
  that drive has no torque channels.

- **The backdrop tracks rpm** — near-black at idle, ember mid-range, intense red
  at the redline, and it persists across every view. Scaled to the car's own
  redline, so it reads the same in any car. Tuning lives at the top of the
  `rpm-reactive backdrop` block in `mx5gauge/web/index.html`: `GLOW_START`
  (how early the tint appears), `GLOW_EMBER` / `GLOW_RED`.

  The sample capture only reaches 1919 rpm, so replay barely shows it. To see
  the full ramp at your desk, drive it manually in the browser console:

  ```js
  tachTarget = 6800; tachShown = 6800;   // then watch the screen
  ```

## Live in the car

The adapter stays in the car's OBD port (that's where it gets power) and the
Mac talks to it over Bluetooth — so the laptop needs to be in the car with you.

1. Plug the vLinker into the OBD port.
2. **Make sure the phone isn't connected to it** — BLE adapters accept one
   client at a time, so close Car Scanner first.
3. Engine on, laptop in the passenger seat:

```bash
.venv/bin/python run.py --live -v
```

It scans for the adapter, runs the ELM327 init, discovers which PIDs the car
actually supports, then polls continuously. Unlike the iOS app it will not get
suspended, so it logs the whole drive.

If the scan can't find it, the error lists every visible Bluetooth device — pass
a better match or the exact address:

```bash
.venv/bin/python run.py --live --name vlinker
.venv/bin/python run.py --live --address XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
```

macOS will ask for Bluetooth permission the first time; allow it for your
terminal app or the scan returns nothing.

## The eight views

Swipe, drag, use ← →, or tap a dot.

| # | View | Notes |
|---|------|-------|
| 1 | **Tacho** | RPM needle + 0–8k dial, redline zone, speed / throttle / peak hold |
| 2 | **Engine** | Coolant hero + COLD/WARMING/READY ring, battery, intake air |
| 3 | **Fuel economy** | Instant L/100km, trip average, L/h, fuel used + RM cost |
| 4 | **Driving score** | 0–100 from smoothness + economy + calm, with a coach word |
| 5 | **Trip** | Distance, time, average speed, fuel, cost |
| 6 | **Power** | Estimated crank kW with peak hold — needs torque PIDs |
| 7 | **Thermals** | Coolant, intake air, catalyst, fuel rail |
| 8 | **Electrical** | Voltage + charging status |

Views degrade honestly: anything the car isn't reporting shows `--` or `n/a`
rather than a plausible-looking zero. **Oil temperature is not available on the
ND3** — confirmed against four captures including the Mazda extended-PID
profile — so the thermals view says so explicitly.

## Every drive is recorded — in full

Live sessions poll **every PID the car reports**, not just the ones on screen,
and write them to CSV as they go. Nothing is held only in memory, so a crash or
a yanked cable costs at most one second of data.

```
logs/drive-20260812-212114.csv     one row per reading
logs/drive-20260812-212114.json    trip totals, score, channel counts
```

56 standard Mode 01 PIDs are decoded — temperatures, fuel trims, O2 sensors,
catalyst temps, rail pressures, torque, pedal positions, evap system and more.
The gauge's own channels (rpm / speed / throttle) are interleaved between every
other reading, so the needle stays responsive while the long tail is swept.

Readings are range-checked per channel before being stored: a resync glitch can
otherwise produce values like `6e-310` volts and poison both the log and the
metrics. Rejected samples are counted, not silently kept.

Pass `--no-record` to skip logging. Replays are not recorded (that would just
duplicate a file you already have).

## Replaying past drives

List what you have:

```bash
.venv/bin/python run.py --sessions
```

```
  file                                   dist   moving   score   rows
  --------------------------------------------------------------------
  drive-20260812-212705.csv           8.42 km   18 min      86  38104
```

Replay the most recent drive, or a specific one:

```bash
.venv/bin/python run.py --replay last
.venv/bin/python run.py --replay "logs/drive-20260812-212705.csv" --speed 10
```

Recorded CSVs and Car Scanner `.brc` captures are both accepted. Replay uses the
file's own timeline, so the driving score comes out the same at any speed.

## Layout

```
mx5gauge/
  pids.py      PID table + decode formulas + response parsing   (pure, tested)
  metrics.py   fuel economy, trip accumulators, driving score   (pure, tested)
  brc.py       Car Scanner .brc reader
  sources.py   ReplaySource (.brc) and LiveSource (BLE + ELM327)
  state.py     shared VehicleState + derived snapshot for the UI
  server.py    stdlib HTTP server
  web/         the round gauge UI
run.py         CLI entry point
tests/         host tests for the decode + metric maths
captures/      your .brc recordings
```

## Tests

```bash
.venv/bin/python tests/test_pids.py
.venv/bin/python tests/test_vehicle.py
```

`test_pids` covers the decode formulas against known byte inputs (e.g. RPM
`1A F8` → 1726), response parsing including multi-ECU replies, the
supported-PID bitmask, and the poll-cycle builder.

`test_vehicle` covers VIN handling: multi-frame mode-09 reassembly, WMI →
make, and the model-year cycle (including that `W` must read as 1998, not the
still-future 2028).

## Troubleshooting: the adapter keeps disconnecting

In rough order of likelihood:

1. **The vLinker is paired in System Settings → Bluetooth.** This is the big
   one. If macOS is managing the link it will connect, drop and re-grab the
   device underneath us. Click the ⓘ next to it and **Forget This Device**.
   A BLE adapter used this way should *not* appear as a paired Mac device.
2. **The phone still holds it.** BLE is one client at a time. Force-quit Car
   Scanner (swipe it away — backgrounding is not enough).
3. **Bluetooth permission.** If the window closes instantly with no output, or
   the process dies with signal 6/134, macOS denied Bluetooth access. Enable
   Terminal under **System Settings → Privacy & Security → Bluetooth**.
4. **The adapter went to sleep.** The vLinker sleeps when the car is off or
   after a long idle. Ignition on, then start the launcher.
5. **Wrong device picked.** See exactly what the Mac can see:

```bash
.venv/bin/python run.py --scan
```

   Then pin it: `run.py --live -v --address <address-from-the-scan>`

Run with `-v` and the terminal prints every command, the characteristic pair it
chose, and the reason for each drop. The gauge header also shows a live
reconnect count, so you can tell "never connected" apart from "connects then
drops".

## Notes / gotchas

- **Replay uses the capture's own timeline**, not wall-clock. Without that, a
  10× replay makes every speed change look 10× harsher and the driving score
  reports phantom harsh-braking events.
- **One BLE client at a time.** If the phone holds the adapter, the Mac can't
  connect, and vice versa.
- **Fuel price** for the cost readouts is `FUEL_PRICE_RM` in `metrics.py`.
- **Score weights** (smoothness / economy / calm) are constants at the top of
  `metrics.py` — tune them against a replay until the numbers feel right.

## Porting to the board

When the Waveshare ESP32-S3-Touch-LCD-1.28 arrives:

- `pids.py` → `src/obd/pid.cpp` (same formulas, same tests)
- `metrics.py` → `src/metrics/` (same accumulators)
- `sources.LiveSource` → `src/obd/ble_transport.*` + `elm327.*` (same command
  sequence, same discovery)
- `web/index.html` → LVGL screens, one per view (same layout and thresholds)
