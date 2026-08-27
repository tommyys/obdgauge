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

## Pulling drives off the board

The ESP32-S3 board records every drive to its own flash while it's out
driving, whether or not the Mac was ever connected (SPEC.md §15). Bring the
board back, plug it into the Mac over USB, and pull whatever it collected:

```bash
.venv/bin/python tools/pull_drives.py           # lists what's held, then pulls it
.venv/bin/python tools/pull_drives.py --list    # just look, pull nothing
.venv/bin/python tools/pull_drives.py --force   # re-pull drives already in logs/
```

It lends the board the Mac's clock, then writes anything new into `logs/` in
the same CSV shape everything else here reads, so it shows up in `run.py
--replay` and `run.py --sessions` immediately. A drive recorded before the
board ever had a clock is written as `drive-unknown-<id>.csv` with no
timestamp, rather than guessing one.

**The USB console is shared with `idf.py monitor`** — close any open monitor
window first, or the puller reports the port busy rather than hanging.

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
.venv/bin/python run.py --replay last --speed 10   # the newest drive
.venv/bin/python run.py --replay "logs/drive-20260813-204128.csv"
.venv/bin/python run.py --replay --speed 1         # real time
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
  convincing zero. A drive with no torque channels gates the Power view, and
  one with no catalyst sensor leaves that figure as a dash.

- **It wakes like a cluster.** A boot splash holds the screen for 2.5 s and the
  instruments are withheld until it finishes — no half-populated dials on the
  way up. The clip is `mx5gauge/web/boot.mp4`; `BOOT_MS` at the top of
  `mx5gauge/state.py` sets how long it holds, and the clip is trimmed to match.
  There is no shutdown animation: the gauge runs on ignition-switched power, so
  key-off is a hard cut with no frame left to draw in.

- **A drive ends when the engine does.** Ignition on/off is read from the
  stream — the PIDs going quiet while battery voltage drops off the alternator,
  and `run_time` resetting on a restart — so switching the car off and driving
  on later leaves two files rather than one. Drives under a minute are
  discarded. Because the summary is rewritten every 10 s, a drive that ends by
  losing power still reports its distance instead of reading `interrupted`.

- **The backdrop tracks rpm** — near-black at idle, ember mid-range, intense red
  at the redline, and it persists across every view. Scaled to the car's own
  redline, so it reads the same in any car. Tuning lives at the top of the
  `rpm-reactive backdrop` block in `mx5gauge/web/index.html`: `GLOW_START`
  (how early the tint appears), `GLOW_EMBER` / `GLOW_RED`.

  Full intensity is reached at `GLOW_FULL` (0.72 of redline, so ~5000 rpm on
  the MX-5) rather than at the redline itself, which is somewhere you rarely
  go on a road. A gentle drive may never light it far; to see the full ramp at
  your desk, drive it manually in the browser console:

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
| 3 | **Fuel economy** | Instant km/L, trip average, L/h, fuel used + RM cost |
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

**On screen:** swipe to the last view, **Drives**. It lists every replayable
drive — your own recordings and the Car Scanner captures — newest first. Tap one
and the gauge loads it there and then; the row you're playing is marked. In live
mode the list still shows, but the rows are inert: loading a drive would drop
the link to the car.

**From the terminal**, the same library:

```bash
.venv/bin/python run.py --sessions
```

```
  when                       kind     summary
  ----------------------------------------------------------------------
  12 Aug 16:50               capture  10 ch · 12.9k pts · 54 min
                             2026-08-12 16-50-58.brc
```

Replay the most recent drive, or a specific one:

```bash
.venv/bin/python run.py --replay last
.venv/bin/python run.py --replay "logs/drive-20260812-212705.csv" --speed 10
```

Your own drives quote distance and a score. Captures quote channels, sample
count and duration instead — distance integrated from a Car Scanner file is
meaningless, because iOS suspends the app and leaves gaps in the recording.

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
captures/      drop Car Scanner .brc files here to replay them
```

## Tests

```bash
.venv/bin/python tests/test_pids.py
.venv/bin/python tests/test_vehicle.py
.venv/bin/python tests/test_library.py
```

`test_pids` covers the decode formulas against known byte inputs (e.g. RPM
`1A F8` → 1726), response parsing including multi-ECU replies, the
supported-PID bitmask, and the poll-cycle builder.

`test_vehicle` covers VIN handling: multi-frame mode-09 reassembly, WMI →
make, and the model-year cycle (including that `W` must read as 1998, not the
still-future 2028).

`test_library` covers the drive library: pulling each drive's date out of its
filename (mtime is useless — every capture shares one), the summary lines, and
that `resolve` only ever returns a file the library already listed, so a
crafted path like `../mx5gauge/server.py` cannot be loaded.

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

## The board

Done, not pending. The firmware lives in `firmware/` and runs on a **Waveshare
ESP32-S3-Touch-AMOLED-1.75C** — not the 1.28" board this README used to
anticipate. As of 2026-08-27 it reads the car on its own: plugged into the car's
USB socket with no laptop attached, it goes from power-on to live engine data in
32 seconds.

| Simulator | Firmware |
|---|---|
| `pids.py` | `firmware/components/gauge_core/pid.cpp`, `poll.cpp` |
| `metrics.py` | `gauge_core/metrics.cpp` |
| `sources.LiveSource` | `gauge_platform/ble_transport.cpp` (NimBLE), `gauge_core/elm327.cpp`, `main/live_link.cpp` |
| `state.py` | `gauge_core/state.cpp` |
| `vehicle.py`, `ignition.py` | `gauge_core/vehicle.cpp`, `ignition.cpp` |
| `web/index.html` | `gauge_ui/ui.cpp` — eight of the nine views |

Building and flashing, from `firmware/`:

```sh
. ../tools/idf_env.sh          # source it; plain export.sh picks the wrong Python
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

**Turn the Mac's Bluetooth off before testing in the car.** macOS auto-reconnects
to the vLinker whenever it can, and a BLE adapter that is already connected stops
advertising — so the board cannot see the very adapter the laptop is holding.
Only one of them gets it at a time.

For runs with no cable attached, the board keeps its own record: events go to
NVS and the previous session is printed back on the next boot with a console
attached (`main/flight_log.cpp`). Unplug, drive, plug in, read.

**Unplug the board when you leave the car.** This car's USB socket is constant,
not ignition-switched, so the gauge does not yet stop on its own — sleep-on-
key-off is the next thing to build (`SPEC.md` §7).
