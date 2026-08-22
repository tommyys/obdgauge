"""Compile logs/*.csv into the binary drive library the firmware replays.

Why binary: the board has no CSV parser and no reason to grow one. Why 12-byte
records rather than a packed 9: Xtensa has no unaligned 32-bit load, so keeping
t_ms and value on 4-byte boundaries avoids a memcpy per field. The whole
library is under 0.5 MB either way, so the padding costs nothing that matters.

Layout (little endian):
  header  32 B : "MX5D", version, channel_count, drive_count, record_count
  channels     : channel_count x 16 B name, NUL padded
  drives       : drive_count x 32 B  -> name[20], first_record, count, duration_ms
  records      : record_count x 12 B -> u32 t_ms, u16 chan, u16 pad, f32 value

Usage: .venv/bin/python tools/build_drive_asset.py [out.bin]
"""
import csv
import glob
import os
import struct
import sys

# 32 bytes: 4+2+2+2+2+4 = 16 of fields, then 16 of padding. The firmware
# assumes a 32-byte header, so the padding is load-bearing.
HDR = struct.Struct('<4sHHHHI16s')
CHAN = struct.Struct('<16s')
DRIVE = struct.Struct('<20sIII')
REC = struct.Struct('<IHHf')


def main(out_path):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sorted(glob.glob(os.path.join(root, 'logs', '*.csv')))
    if not files:
        sys.exit('no captures in logs/')

    channels = []
    chan_id = {}
    drives = []
    records = []

    for path in files:
        first = len(records)
        t0 = None
        last_t = 0.0
        with open(path) as fh:
            for row in csv.DictReader(fh):
                try:
                    value = float(row['value'])
                except (TypeError, ValueError):
                    continue          # non-numeric, exactly as the Python skips
                key = row['key']
                if key not in chan_id:
                    if len(key) >= 16:
                        sys.exit('channel name too long for the table: %r' % key)
                    chan_id[key] = len(channels)
                    channels.append(key)
                t = float(row['t'])
                if t0 is None:
                    t0 = t
                last_t = t
                records.append((int(round((t - t0) * 1000.0)), chan_id[key], value))
        name = os.path.basename(path)[:19]
        drives.append((name, first, len(records) - first,
                       int(round(((last_t - (t0 or 0.0))) * 1000.0))))

    blob = bytearray()
    blob += HDR.pack(b'MX5D', 1, len(channels), len(drives), 0, len(records), b'')
    for c in channels:
        blob += CHAN.pack(c.encode())
    for name, first, count, dur in drives:
        blob += DRIVE.pack(name.encode(), first, count, dur)
    for t_ms, chan, value in records:
        blob += REC.pack(t_ms, chan, 0, value)

    with open(out_path, 'wb') as fh:
        fh.write(blob)

    print('%s: %d drives, %d channels, %d records, %.2f MB'
          % (os.path.basename(out_path), len(drives), len(channels),
             len(records), len(blob) / 1024 / 1024))
    for name, first, count, dur in drives:
        print('  %-24s %6d samples  %5.1f s' % (name, count, dur / 1000.0))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build-assets/drives.bin')
