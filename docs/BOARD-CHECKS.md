# Board checks for drive recording

This is the whole hardware half of drive recording, in one list, in the order
it has to be done. It is written to be run by someone who is not a firmware
engineer, some of it while sitting in a car.

**Nothing in here has ever run.** The board was not connected while this
feature was written. Every line of the C++ that touches flash is proven by
tests on a Mac; none of it has met the real part. That is what this document
is for.

## Two rules before anything else

**1. `idf.py monitor` and the Python tool cannot both hold the port.**

The gauge's console is USB-Serial-JTAG. It is one single serial port, and it
carries both the boot log you read in `idf.py monitor` and the `STATS` /
`LIST` / `GET` conversation `tools/pull_drives.py` has with the board. Only
one program can hold a serial port at a time.

So: **before running any `pull_drives.py` command, close the monitor window**
(Ctrl-`]` quits `idf.py monitor`). And before opening a monitor, make sure no
pull is running. If you forget, you will see

```
cannot open /dev/cu.usbmodemXXXX: [Errno 16] Resource busy
(is `idf.py monitor` still open? they share the port)
```

That message is not a fault. It is this exact mistake.

**2. If the console goes silent and stays silent: power-cycle the board.**

Unplug USB for a few seconds and plug it back in. A USB re-enumeration, a
sleeping Mac or a jiggled cable can drop the link. A power-cycle re-runs boot
and starts the console reader fresh. **It does not erase recorded drives** —
`ERASE CONFIRM` is the only thing that does. If the console is still silent
after a power-cycle and ten seconds' wait, that is a real bug: stop and report
it.

---

## Pre-flight (no board involved)

Do this first, from a normal terminal. The board does not need to be plugged
in — `verify_port.sh` never touches it. It only asks whether the C++ core
still agrees with the Python simulator.

```bash
tools/verify_port.sh
```

Expected output:

```
== 1. unit suites ==
   suites passing: 18
   no failures
== 2. cross-validation against the simulator ==
   logs/drive-20260813-204128.csv: 16896 samples, 36 channels, 29 derived fields, 0 divergences
   ... one line per csv in logs/ ...

PORT VERIFIED
```

(Then a long informational block about what is and is not covered. That block
always prints and is not a pass/fail signal.)

What to check:

- `suites passing:` is 18, and the next line reads `   no failures`.
- Every `.csv` line under section 2 ends in `0 divergences`.
- The last non-blank line before the informational block is exactly
  `PORT VERIFIED`.

**Read the text, not the exit code.** The failure signal is the literal line
`PORT NOT VERIFIED - see above`, or any non-zero `divergences` count, or a
`FAILURES` line under section 1. The exit code is not a reliable summary: a
unit-test failure does exit non-zero, but section 2's cross-validation always
exits 0 whatever it finds. If any of the above is wrong, copy the whole output
and flag it rather than interpreting it.

If `logs/` has no `.csv` files at all (fresh clone), section 2 prints
`WARNING: no captures in logs/ - nothing was cross-checked` and skips to
`PORT VERIFIED`. That warning on its own is not a failure.

---

## Part A — at the bench

Board plugged into the Mac, engine nowhere near.

### A1. Check the flash backup exists

Standing rule, and this step changes the partition table:

```bash
cd backups && shasum -a 256 -c esp32s3-full-32MB.bin.sha256 && cd ..
```

Must say `OK`. If it does not, stop — do not flash.

### A2. Flash and boot

```bash
source tools/idf_env.sh
cd firmware
idf.py -p /dev/cu.usbmodem* flash monitor
```

This is a full flash (app plus the changed partition table), not an OTA. It
does **not** erase the 16 MB `assets` partition or the `drives` partition —
only the new `logs` partition at the tail of flash is new.

Watch the first few seconds of boot for:

- **Good:** a line like
  `drivelog: 2544 sectors (0 used), 0 drive starts, 0 records, clock floor 0`
- **Good:** shortly after, `drive log up: 0 drive starts`
- **Bad:** `no 'logs' partition` or `logs partition will not mount` — the
  partition table did not take. Stop. Run
  `idf.py -p /dev/cu.usbmodem* erase-flash` and repeat A2 from the top.
- **Bad:** the word `NO_MEM` anywhere — a draw ran out of memory. Stop and
  report the exact line.

(On a board that has recorded before, the numbers will not be zero. That is
fine. Zeros are what a never-used tail of flash looks like.)

### A3. Watch the frame rate for 30 seconds

A line like `ui: NN fps, view ...` prints **every 2 seconds** (not once a
second — two of the three source runbooks got this wrong).

There is no fixed range to check against. `main.cpp` says the live views run
at **10-22 fps** depending on what is on screen. So: **compare against what
this gauge did before this change**, on the same view, not against a number
written down here. If you do not have a before-figure, note what you see now
and move on.

- **Bad:** a clear drop against the before-figure, or the display visibly
  stuttering or freezing. That would mean flash work is reaching the drawing
  code, which the whole design exists to prevent. Note the fps you saw and
  stop.

### A4. Start the 10 MB tail from a known state

The `logs` partition has never been used, so whatever is in it is whatever the
factory left there. Type into the monitor:

```
ERASE CONFIRM
```

- Expect: `OK erased`.
- Then `LIST` → `OK 0 drives truncated=0`.
- Then `STATS` → `used=0`, `starts=0`, `records=0`.

This is destructive and that is the point: it is the only step in this
document that is, and doing it now — before there is anything worth keeping —
is why it is safe here and nowhere later.

### A5. Read STATS and write the numbers down

```
STATS
```

Expect one line then `OK`:

```
STATS sectors=2544 used=0 bytes=0 starts=0 records=0 dropped=0 writefail=0 epoch=0 floor=0 table=1
```

Write down `dropped=` and `writefail=`. **These two numbers are the only
evidence of data loss this system produces**, and you will be asked for them
again after every drive and after every `GET`:

- `dropped=` — readings the recorder's queue could not take, thrown away.
- `writefail=` — flash writes or erases that failed, leaving a hole in a
  drive. Any non-zero value here is serious; the first failure is also in the
  flight log.

Both should be `0` here, and should still be `0` at the end of the day. A
reply of exactly `ERR no recorder` is not a Task-8 bug — it means the `logs`
partition never mounted, i.e. A2 failed.

### A6. The two error cases

```
ERASE
```
- Expect exactly: `ERR say 'ERASE CONFIRM'`
- Then `LIST` again: unchanged. This command rejects, it does not erase.

```
GET 999
```
- Expect exactly: `ERR no drive 999`

### A7. Set the clock — before driving

**Do not skip this and do not do it later.** If the board has no clock when a
drive opens, that drive is stamped "unknown" forever and comes off as
`drive-unknown-N.csv` with no timestamp. Setting the clock afterwards does not
retroactively fix it.

Close the monitor first (rule 1), then:

```bash
.venv/bin/python tools/pull_drives.py --list
```

That sets the clock as a side effect and prints what the board holds. **Note
the wall-clock time, to the second, at the moment you run it** — from the
Mac's own clock. Write it down. You will compare a filename against it later.

Then reopen the monitor and check:

```
STATS
```
- `epoch=` is now a large number close to the current Unix time, not `0`.
- `floor=` will also be non-zero from here on.

---

## Part B — in the car

### B0. Turn the Mac's Bluetooth off

System Settings → Bluetooth → off. The OBD adapter pairs with one device at a
time. If the Mac still holds it, the board will just report that it cannot
find the adapter and nothing else in Part B will happen.

### B1. First drive

Start the engine and watch the console.

- **Good:** a line like `live: 52 PIDs` — around 50, confirming the recorder
  asks the car for everything it supports, not just the ~14 the screen shows.
  **Bad:** a number in the teens or single digits.
- **Good:** `drive 1 opened, clock known`. If it says `clock UNKNOWN`, A7 did
  not take — the drive will still record, it just will not have a timestamp.
- Drive normally for **about 5 minutes**, then switch the engine off.
- Wait a full 25 seconds without touching anything. Do not restart the car.
- **Good:** within those 25 seconds, `drive 1 closed, NNNN records`. Five
  minutes of driving should be roughly ten thousand records. Under 100 and the
  drive will not be offered at all — write down the exact number and flag it.
- **Bad:** no `drive 1 closed` within 30 seconds of key-off. This is the
  single most important failure in this document: it means the silence timer
  is not firing, drives never end, and everything merges into drive 1.
- **Bad:** it closes immediately, within a couple of seconds of key-off. The
  timer is firing too early and one drive will be split into many.
- Keep half an eye on the `ui: NN fps` line throughout. It should not dip when
  a drive opens, closes, or flushes.

### B2. Second drive — and check that it is a second drive

Restart the car and drive for **another 5 minutes**, then key off and wait 25
seconds again.

- **Good:** `drive 2 opened`, and later `drive 2 closed, NNNN records`.
- **Bad:** it still says `drive 1`. The first drive never closed.

This pair of drives is the check that matters most in this document, and B4
below is where it actually gets confirmed.

### B3. The power-cut test

Start the car again and let it record for a minute or two. Then, **mid-drive,
unplug the gauge's USB power for 2 seconds and plug it back in.**

After it reboots:

- **Good:** the boot line shows non-zero `used=` and `records=` — the earlier
  drives are still there.
- Then `LIST` (or `pull_drives.py --list`, remembering rule 1) must still show
  the interrupted drive, with roughly the records it had accumulated, and
  `complete=0`. An unfinished drive is expected here; its records are still
  good. What must not happen is the drive vanishing.

### B4. Read the loss counters again

`STATS`, and compare `dropped=` and `writefail=` against what you wrote down
in A5.

- `dropped=` may have risen a little; note by how much.
- `writefail=` **must still be 0.** Anything else means a drive on this board
  has a hole in it.

---

## Part C — back at the desk

Close the monitor first. Rule 1.

### C1. List what the board is holding

```bash
.venv/bin/python tools/pull_drives.py --list
```

Expect a `board: ...` line, a `clock set to ...` line, then one line per
drive, newest first.

**This is where the two drives get checked as two:**

- There are **two rows** (three, if the power-cut drive of B3 is separate).
- They have **different ids**.
- Each has a plausible `records` count and duration — roughly 5 minutes each,
  not one drive of ten minutes and no second row.
- If there is one row holding everything, drives are not closing. Stop.

Failure signals:

- `cannot open /dev/cu...` — the monitor is still open. Rule 1.
- `no board found. Plug it in, or pass --port.` — nothing matched
  `/dev/cu.usbmodem*`. Check the cable and the USB mode.
- `board channel table is vN, this tool speaks v1` — the firmware's channel
  table changed. Do not pull anything until `PID_KEYS` in `pull_drives.py` has
  been brought back in line with `poll.cpp`.
- A drive marked `(channel table v2 -- CANNOT BE PULLED)` — that drive was
  recorded by different firmware and this tool would mislabel every channel in
  it. It is refused on purpose.
- `...and more the board could not list in one reply` — there are more drives
  than one `LIST` can report. Pull these, then run again.
- `WARNING: N flash writes FAILED` — the `writefail` counter from A5, surfaced
  by the tool. Report it.

### C2. Pull them

```bash
.venv/bin/python tools/pull_drives.py
```

Expect, per drive, `pulling drive N (... records)...` then
`wrote logs/drive-....csv (N rows)`.

**Watch the monitor-free console output for a crash while `GET` is streaming.**
If you see `Guru Meditation`, `stack overflow in task serialcmd`, or the board
reboots mid-stream — stop. Do not re-run. Report it. That is the 12 KB console
stack being too small on real hardware, and re-running just does it again.

If it says `crc32 mismatch (got ..., board said ...)` or
`got N records, board said M`, the tool has refused to write a half-pulled
drive. Re-run the pull once.

- If the re-run succeeds, it was the USB link. Fine.
- **If the same mismatch repeats on the re-run AND happens on a different
  drive too, it is not the cable.** It is the CRC convention between the
  firmware and Python disagreeing, and every pull of every drive will fail
  identically. The fix is in `emit()` in `firmware/main/serial_cmd.cpp` and in
  `gauge::crc32` — not in the cable, not in the port. Report it that way.

Then run `STATS` once more (reopen the monitor) and check `dropped=` and
`writefail=` a final time. A `GET` holds the recorder's lock for the whole
stream, so if the engine was running during a pull, `dropped=` may have risen.

### C3. Check the timestamp is actually right

Look at the filename of the first drive you pulled:

```bash
ls -1 logs/drive-*.csv | tail -3
```

**Compare it, to the second, against the wall-clock time you wrote down in
A7,** plus however long it was before you started driving. It must line up.

This is not a formality. If the epoch is ever handled as a float instead of
raw bits, it comes back rounded to the nearest 128 seconds — a timestamp that
is wrong by up to two minutes and looks completely plausible. Every other step
in this document passes with that bug present. This is the only one that
catches it.

If any file is named `drive-unknown-N.csv`, open it: the second line is a
`# clock floor:` comment giving the time the drive is known to be *after*.
That comment is the only thing placing it in time.

### C4. Look inside

```bash
head -5 logs/drive-<the-new-one>.csv
```

Expect the `iso,t,key,value` header, then rows with a real ISO timestamp,
increasing `t`, and channel names including `imu_ax`, `imu_ay`, `imu_az` and
`imu_gz` — no log before this feature has had those.

**Also check the two files do not overlap in time.** Two drives means two
non-overlapping time ranges; if drive 2's rows start before drive 1's end,
they were never really two drives.

### C5. Replay it

```bash
.venv/bin/python run.py --replay logs/drive-<the-new-one>.csv
```

Expect real rpm, speed and coolant playing back at the desk. **This is the
acceptance test for the whole feature** — a drive nobody watched, watchable
afterwards.

### C6. Compile it back into the on-board replay library

```bash
.venv/bin/python tools/build_drive_asset.py build-assets/drives.bin
```

The new drive should appear in the printed listing with its own record count.

### C7. Answer B3 — which IMU axis is which

With a real moving-car log in hand, look at `imu_ax` against `imu_ay` during a
hard braking event and during a corner, and note which axis moves under which.
Write the answer into `SPEC.md` §7.5 B3, replacing "the on-board IMU is
unused". This is the open design question the whole IMU recording exists to
settle, and it cannot be answered from a parked car.

---

## What these checks still do not cover

**Ring wrap has never run on real flash, and cannot be reached in one
session.** The `logs` partition holds about 7.5 hours of driving before the
oldest drive starts being dropped. Wrap is proven by host tests against a fake
flash — including a drive that fills the entire ring, a power cut mid-write,
and a power cut mid-erase — but the real part has never been round the loop
once. The first time it happens will be in a car, weeks from now, unattended.
If a drive ever comes back short at its beginning, or `LIST` starts skipping
the oldest drive, that is the wrap and it wants looking at rather than
shrugging at.

Also untested on hardware: the console reader under a real USB drop, the
behaviour of `GET` on a drive of more than a few thousand records, and
`writefail` ever being non-zero (nothing has yet made a real flash write
fail).
