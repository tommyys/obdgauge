# Drive recording on the gauge — design

**Date:** 2026-08-27
**Status:** approved, not yet implemented

## The problem

The gauge's real life is on the car's own USB socket with no laptop attached.
In that life it currently keeps nothing: readings arrive over BLE, get drawn,
and are gone. The only persistent record is `flight_log` — a few dozen lines of
event text in NVS, which says whether the link came up, not what the car did.

Every drive log in `logs/` was captured by the Python simulator with a Mac in
the passenger seat. That is not how the gauge is driven.

So: record every reading of every drive to flash, and pull them off over USB
into the same `logs/*.csv` the desk replay already eats.

## Decisions taken (Tommy, 2026-08-27)

- **Record everything the car answers** — all ~50 supported PIDs, not the ~14
  the gauge draws.
- **Plus the IMU**, which is also what unblocks B3: which of X/Y is
  longitudinal vs lateral needs a moving car, and one logged drive answers it.
- **When flash fills, drop the oldest drive.**
- **Borrow wall-clock time from the Mac** — the board has no RTC.
- **Retrieval is one command run by hand** (`tools/pull_drives.py`), not an
  automatic on-plug-in import.

## 1. Storage

A new partition in the 9.94 MB at the tail of flash that nothing uses:

```
logs, data, spiffs, 0x1610000, 0x9F0000
```

Nothing existing moves. The 13.4 MB boot splash in `assets` does not have to be
reflashed — only the partition table changes.

Subtype `spiffs` is a label of convenience, matching what `drives` already
does: there is no filesystem here, and access is raw `esp_partition_read` /
`_write` / `_erase_range`.

### Why not a filesystem

SPIFFS on a 10 MB partition costs RAM for its page index, needs a mount that
can fail, and gives no control over which data is dropped when full. The whole
requirement here is "append, and when full lose the oldest" — which is a ring,
and a ring on raw flash is a few hundred lines that can be host-tested.

## 2. The ring

NOR flash can be written a word at a time but only erased in 4 KB sectors, so
the partition is 2,544 sectors used as a loop.

Each sector opens with a 16-byte header:

```
magic   4 B  "MX5L"
seq     4 B  u32, +1 per sector written, ever
drive   4 B  u32 drive id
flags   2 B  bit 0: this sector opens a drive
pad     2 B
```

The remaining 4,080 bytes hold up to 340 records in **the same 12-byte layout
`build_drive_asset.py` writes** (`u32 t_ms, u16 chan, u16 pad, f32 value`), so a
pulled drive needs no new parser anywhere — firmware, host tests, or desk app.

Records are appended straight into the live sector at increasing offsets, in
batches, which NOR flash permits as long as the target bytes are still erased.
`esp_partition_write` wants 4-byte alignment; 12-byte records give that for
free.

**Wipe-ahead is the whole trick.** When a sector fills, the writer erases the
*next* sector before writing to it. The next sector holds the oldest data in the
partition, so "drop the oldest" needs no bookkeeping, no compaction and no
free-space accounting — it falls out of the loop advancing.

Rate is about 1.1 MB/hour of driving (§3), so each sector is erased roughly
once per 9 hours of driving. The part is rated for 100,000 erases per sector.

### Finding the head on boot

Read all 2,544 headers (one 16-byte read each, ~2 s worst case, done in the
writer task, not the UI loop). Highest valid `seq` is the newest sector. Within
it, the append point is the first 12-byte slot that is still all-`0xFF`.

A power cut can therefore only lose the records still in the current batch —
by design, batches flush at least every 2 seconds.

A sector whose magic is wrong is treated as erased. A sector whose records
trail off into `0xFF` mid-way is the normal case, not corruption.

### Channel names

The 12-byte record stores a channel *id*, not a name. The firmware's channel
table is `gauge_core/poll.cpp`, which is compiled in and ordered — so ids are
assigned as `poll.cpp`'s table order, plus a fixed block for the IMU and
markers. The pull tool has the same table, and the header carries a table
version so a mismatch is an error rather than silently mislabelled data.

Reserved ids at the top of the range: `0xFFFF` drive-start marker (value =
epoch seconds when known, else 0), `0xFFFE` drive-end marker.

## 3. What is recorded

**All supported PIDs.** `build_poll_cycle(supported, log_all=true)` already
does exactly the right thing: it puts `kPollFast` (rpm, speed, throttle,
run_time) in front of every other PID in the cycle. So the needle keeps its
current sub-second refresh and the other ~40 channels come round every few
seconds. Recording everything costs nothing on screen — the change is one bool
in `live_link.cpp`.

Total sample rate is set by the adapter's round-trip, not the channel count:
~12 readings/second measured across four real logs.

**IMU at 5 Hz** — `ax`, `ay`, `az` and yaw rate `gz`. Enough to resolve the
axes and to catch real harshness: a hard brake or a corner is a ~1 s event, so
it lands 5 samples in. It will not see a pothole, which is not what it is for.

The rate is set by storage, not by the sensor. The IMU is four channels against
the car's twelve readings a second, so at 10 Hz it would be 40 records/s and
outweigh the car three to one — 10 MB would hold 4.5 hours. At 5 Hz the total
is ~28 records/s, ~1.1 MB/hour, and the partition holds **about 9 hours of
driving**. If the axes turn out to need more resolution once B3 is decided,
this is one constant.

*Rejected:* packing all four axes into one 12-byte record (`u32 t_ms` + four
`int16`) would buy 10 Hz at the same cost, but it breaks the uniform record
layout that lets the desk app, `build_drive_asset.py` and the host tests read
this data with no new parser — which is most of why this format was chosen.

**Live data only.** Replay mode records nothing — a recorded replay of a
recording is a trap, and the desk app can already read the source.

## 4. Drive boundaries

A drive ends when no car reading arrives for 20 seconds. This is the key-off
signal already proven in the car: at key-off the vLinker sleeps and the poll
loop reports `adapter stopped answering` within ~15 s. The next reading opens a
new drive with a new id.

Engine restart while the link is still up is caught by `run_time` (PID 0x1F)
resetting, which `gauge_core/ignition` already detects and which the poll cycle
already requests for that purpose.

A drive that produced fewer than 100 records is dropped rather than kept: that
is a key touched and released, not a drive.

## 5. Time

There is no RTC. `esp_timer` counts microseconds since boot and nothing else.

`tools/pull_drives.py` sends `TIME <epoch>` on connect. The firmware stores
`epoch_at_boot = epoch - uptime` and stamps each drive-start marker with
`epoch_at_boot + uptime`. Because the car's USB socket is constant power (proven
2026-08-27), the board rarely reboots, so one clock-set covers many drives.

Every 5 minutes the current wall clock is written to NVS. On a boot with no Mac
present that value is a floor — the drive happened *after* it — so such drives
are named `drive-unknown-<id>.csv` and carry a comment giving the floor, rather
than a timestamp that looks authoritative and is wrong.

## 6. Keeping it away from the display

Erasing a sector takes tens of milliseconds. On the UI loop that is visibly
dropped frames, and the panel's no-allocation rule
(`SPEC.md` §6) exists because that path is already fragile.

So `drive_log` owns a FreeRTOS task at low priority, pinned to core 0 beside
the BLE task, and it is the only thing that touches the `logs` partition or the
IMU. The UI loop's contribution is one non-blocking `xQueueSend` per sample,
dropping on a full queue exactly as `live_link` already drops readings the UI is
too busy to take.

The IMU moves out of the UI loop and into this task at the same time; today it
is read once per second only to print it.

## 7. Retrieval

The console is USB-Serial-JTAG — the same port as `idf.py monitor`, so the two
cannot be open at once. The pull tool says so when the port is busy.

A line-based command reader on stdin:

| Command | Reply |
|---|---|
| `TIME <epoch>` | `OK` |
| `STATS` | sectors used, drives held, bytes, table version |
| `LIST` | one line per drive: id, epoch (or `?`), record count, duration |
| `GET <id>` | base64 records, 64 lines per chunk, `END <crc32>` |
| `ERASE` | wipes the partition; asks for confirmation |

`tools/pull_drives.py` sets the clock, runs `LIST`, skips ids already present in
`logs/`, `GET`s the rest, and writes `logs/drive-YYYYmmdd-HHMMSS.csv` in the
existing four-column `iso,t,key,value` shape. Which means
`tools/build_drive_asset.py` can then compile a real unattended drive back into
the replay library — the loop closes.

## 8. Files

| File | Role |
|---|---|
| `firmware/components/gauge_core/logbuf.{h,cpp}` | ring index and framing, pure, no flash — host-tested |
| `firmware/main/drive_log.{h,cpp}` | the task, the partition, the IMU, drive boundaries |
| `firmware/main/serial_cmd.{h,cpp}` | the stdin command reader |
| `tools/pull_drives.py` | host side |
| `firmware/partitions.csv` | the `logs` line |
| `firmware/main/live_link.cpp` | `log_all=true` |
| `firmware/main/main.cpp` | tap the sample drain; hand the IMU over |

`logbuf` goes in `gauge_core`, not `main`, for the reason the rest of core is
there: it is the part with the bugs worth catching on a Mac in a second rather
than in a car in an hour.

## 9. Testing

Host tests in `firmware/test/host`, against a fake flash (a byte array with
sector-erase semantics and a write that refuses to un-set a bit, like the real
part):

- append, fill a sector, roll to the next
- wrap the whole partition; oldest drive is the one lost
- power cut mid-batch — reopen, head is found, records before the cut survive
- power cut mid-erase — a half-erased sector is not mistaken for data
- a drive spanning the wrap point
- `seq` wrap-around at u32
- drive shorter than 100 records is dropped
- a channel table version mismatch is refused, not guessed

Then, on the board and in the car: log a real unattended drive, pull it, and
play it back in the desk app. That is the acceptance test — the point of the
whole thing is a drive nobody watched being watchable afterwards.

## Out of scope

- Automatic import on plug-in. Asked for and declined: one command by hand.
- Pulling drives over BLE or Wi-Fi.
- Any change to the eight views. Nothing on screen changes.
- Sleep-on-key-off. Still the other open battery item, and independent of this.
