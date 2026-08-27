"""Unit tests for tools/pull_drives.py's pure parts: channel names, the
LIST/STATS reply parsing, the base64+crc32 reassembly a GET reply goes
through, and the CSV writer. None of this opens a serial port -- these are
exactly the parts that do not need a board.
   Run: .venv/bin/python tests/test_pull_drives.py"""
import base64
import binascii
import csv
import os
import shutil
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from pull_drives import (  # noqa: E402
    REC, CHAN_DRIVE_START, CHAN_DRIVE_END, TABLE_VERSION,
    chan_name, page_drives, parse_stats, parse_drives, parse_truncated,
    reassemble, write_csv,
)

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def check_exits(name, fn, *args):
    """Expect fn(*args) to sys.exit rather than return -- that's how this
    tool refuses to trust a mismatch."""
    try:
        fn(*args)
        ok = False
        got = 'no exit'
    except SystemExit as e:
        ok = True
        got = str(e)
    if not ok:
        FAILED.append('%s: expected SystemExit, got %r' % (name, got))
    print('%-58s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


# --- chan_name --------------------------------------------------------------
check('chan_name PID 0', chan_name(0), 'fuel_status')
check('chan_name PID 55 (last)', chan_name(55), 'ref_torque')
check('chan_name extras base', chan_name(0x0200), 'volts')
check('chan_name extras last (imu_gz)', chan_name(0x0204), 'imu_gz')
check('chan_name out-of-range PID -> None', chan_name(56), None)
check('chan_name out-of-range extra -> None', chan_name(0x0205), None)
check('chan_name drive-start marker -> None', chan_name(CHAN_DRIVE_START), None)


# --- parse_stats -------------------------------------------------------------
stats_body = [
    'STATS sectors=2544 used=812 bytes=3325952 starts=3 records=12345 dropped=0 '
    'writefail=0 epoch=1756300000 floor=1756200000 table=1',
]
check('parse_stats', parse_stats(stats_body),
      {'sectors': '2544', 'used': '812', 'bytes': '3325952', 'starts': '3',
       'records': '12345', 'dropped': '0', 'writefail': '0',
       'epoch': '1756300000', 'floor': '1756200000', 'table': '1'})
check('parse_stats: the write-failure count is visible',
      parse_stats(['STATS sectors=1 used=1 bytes=4096 starts=1 records=9 '
                   'dropped=2 writefail=7 epoch=0 floor=0 table=1'])['writefail'],
      '7')
check_exits('parse_stats: no STATS line -> exits', parse_stats, ['I board booted'])


# --- parse_drives ------------------------------------------------------------
drive_body = [
    'I some board log line landed here',    # a log line interleaved
    'DRIVE id=1 epoch=1756300000 records=500 ms=42000 complete=1 table=1',
    'DRIVE id=2 epoch=0 records=150 ms=9000 complete=0 table=1',
]
check('parse_drives count', len(parse_drives(drive_body)), 2)
check('parse_drives first entry', parse_drives(drive_body)[0],
      {'id': 1, 'epoch': 1756300000, 'records': 500, 'ms': 42000,
       'complete': True, 'table': 1})
check('parse_drives incomplete/unknown-clock entry', parse_drives(drive_body)[1],
      {'id': 2, 'epoch': 0, 'records': 150, 'ms': 9000, 'complete': False,
       'table': 1})
check('parse_drives ignores non-DRIVE noise', parse_drives(['nothing here']), [])
check('parse_drives: a reply with no table= assumes this tool version',
      parse_drives(['DRIVE id=9 epoch=0 records=200 ms=5 complete=1'])[0]['table'],
      TABLE_VERSION)
check('parse_drives: a drive from another channel table is flagged',
      parse_drives(['DRIVE id=3 epoch=0 records=900 ms=1 complete=1 table=2'])[0]['table'],
      2)


# --- parse_truncated ----------------------------------------------------------
# "OK 32 drives" used to be indistinguishable from "32 drives and more I
# cannot show you". The board now says which it is.
check('parse_truncated: complete answer', parse_truncated('OK 4 drives truncated=0'), False)
check('parse_truncated: partial answer', parse_truncated('OK 64 drives truncated=1'), True)


# --- page_drives: walking LIST past its own window --------------------------
# LIST only ever reports the newest 64 drives, and the pull skips drives
# already in logs/ -- so "pull these, then run again" handed back the same 64
# for ever and the older ones were never reachable. The tool pages instead.
PAGES = {
    None: ([{'id': 9}, {'id': 8}], True),
    8: ([{'id': 7}, {'id': 6}], True),
    6: ([{'id': 5}], False),
}
check('page_drives: walks every page, newest first',
      page_drives(lambda b: PAGES[b])[0],
      [{'id': 9}, {'id': 8}, {'id': 7}, {'id': 6}, {'id': 5}])
check('page_drives: and reports the walk as complete',
      page_drives(lambda b: PAGES[b])[1], False)
check('page_drives: one complete page needs no second call',
      page_drives(lambda b: ([{'id': 2}, {'id': 1}], False)),
      ([{'id': 2}, {'id': 1}], False))
# A board that keeps saying "truncated" must not spin this for ever, and must
# not be reported as a complete answer either.
check('page_drives: the page cap stops the walk',
      page_drives(lambda b: ([{'id': 1}], True), max_pages=3)[1], True)
check('page_drives: a page with nothing new stops the walk',
      page_drives(lambda b: ([{'id': 4}, {'id': 3}], True), max_pages=9),
      ([{'id': 4}, {'id': 3}], True))


# --- reassemble: the base64 + crc32 GET reassembly --------------------------
def b64_lines(records, per_line=3):
    """Mirror serial_cmd.cpp's b64_line: group records into base64 lines,
    `per_line` records (36 bytes) per line -- no padding, so decoding line
    by line is exact, same as the firmware relies on."""
    blob = b''.join(REC.pack(*r) for r in records)
    out = []
    step = per_line * REC.size
    for off in range(0, len(blob), step):
        out.append(base64.b64encode(blob[off:off + step]).decode())
    return out


def get_stream(records, drive_id=7, corrupt_crc=False, corrupt_count=False,
               with_log_noise=False):
    blob = b''.join(REC.pack(*r) for r in records)
    crc = binascii.crc32(blob) & 0xFFFFFFFF
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    n = len(records) - 1 if corrupt_count else len(records)
    lines = ['BEGIN %d %d' % (drive_id, n)]
    if with_log_noise:
        lines.append('I a board log line mid-stream, not base64')
    lines += b64_lines(records)
    lines.append('END crc32=%08x' % crc)
    lines.append('OK')
    return lines


sample_records = [
    (0, CHAN_DRIVE_START, 0, 0.0),          # marker, value is a punned epoch
    (0, 9, 0, 850.0),                       # rpm
    (250, 0x0200, 0, 13.8),                 # volts (extra)
    (500, CHAN_DRIVE_END, 0, 0.0),          # marker
]

got = reassemble(get_stream(sample_records), 7)
check('reassemble: record count', len(got), len(sample_records))
check('reassemble: decodes rpm record', got[1], (0, 9, 0, 850.0))
check('reassemble: decodes extras record', got[2],
      (250, 0x0200, 0) + struct.unpack('<f', struct.pack('<f', 13.8)))

got_noisy = reassemble(get_stream(sample_records, with_log_noise=True), 7)
check('reassemble: tolerates a board log line mid-stream',
      len(got_noisy), len(sample_records))

check_exits('reassemble: crc32 mismatch -> exits',
            reassemble, get_stream(sample_records, corrupt_crc=True), 7)
check_exits('reassemble: record count mismatch -> exits',
            reassemble, get_stream(sample_records, corrupt_count=True), 7)


# --- the crc32 convention -----------------------------------------------------
# The board ends a GET with a checksum and reassemble() above verifies it with
# binascii.crc32. Every test above generates BOTH sides of that from Python,
# so none of them could catch the two conventions disagreeing -- and if they
# did disagree, every pull of every drive would fail identically. So pin the
# Python side to the CRC-32 standard check value, published outside this
# project: CRC-32("123456789") == 0xCBF43926. firmware/test/host/test_crc32.cpp
# pins the C side to the same literal, independently. The board's
# gauge::crc32() is byte-for-byte esp_rom_crc32_le(), which is inverted-in,
# inverted-out over the reflected 0xEDB88320 table -- exactly zlib's crc32 --
# so seed 0 on both sides and no '~' wrapper on either.
check('crc32 of "123456789" is the standard check value',
      binascii.crc32(b'123456789') & 0xFFFFFFFF, 0xCBF43926)
check('crc32 of the pangram',
      binascii.crc32(b'The quick brown fox jumps over the lazy dog') & 0xFFFFFFFF,
      0x414FA339)
check('crc32 chains across a split buffer the way the board streams it',
      binascii.crc32(b'56789', binascii.crc32(b'1234')) & 0xFFFFFFFF,
      0xCBF43926)


# --- write_csv ----------------------------------------------------------------
tmp = tempfile.mkdtemp(prefix='mx5-pull-drives-')
try:
    os.makedirs(os.path.join(tmp, 'logs'))

    # A drive with a known epoch: markers skipped, iso filled in, unknown
    # channel counted and skipped rather than written.
    epoch = 1756300000     # 2025-08-27 12:26:40 UTC-ish, exact value doesn't matter
    records = [
        (0, CHAN_DRIVE_START, 0, 0.0),
        (0, 9, 0, 850.0),                 # rpm
        (1500, 0x0200, 0, 13.8),          # volts
        (3000, 999, 0, 1.0),              # unknown channel id
        (4000, CHAN_DRIVE_END, 0, 0.0),
    ]
    drive = {'id': 1, 'epoch': epoch, 'records': len(records), 'ms': 4000,
             'complete': True}
    path, skipped = write_csv(tmp, drive, records)
    check('write_csv: known-epoch filename',
          os.path.basename(path),
          'drive-%s.csv' % __import__('datetime').datetime
              .fromtimestamp(epoch).strftime('%Y%m%d-%H%M%S'))
    check('write_csv: one unknown channel skipped', skipped, 1)

    with open(path, newline='') as fh:
        rows = list(csv.DictReader(fh))
    check('write_csv: markers not written, unknown chan not written', len(rows), 2)
    check('write_csv: header', list(rows[0].keys()), ['iso', 't', 'key', 'value'])
    check('write_csv: rpm row key', rows[0]['key'], 'rpm')
    check('write_csv: rpm row t', rows[0]['t'], '0.000')
    check('write_csv: rpm row value', float(rows[0]['value']), 850.0)
    check('write_csv: iso is non-empty when epoch known', rows[0]['iso'] != '', True)
    check('write_csv: volts row key', rows[1]['key'], 'volts')
    check('write_csv: volts row t reflects t_ms/1000', rows[1]['t'], '1.500')

    # A drive with no clock: drive-unknown-N naming, empty iso column.
    unknown_records = [(0, 9, 0, 900.0)]
    unknown_drive = {'id': 42, 'epoch': 0, 'records': 1, 'ms': 0, 'complete': True}
    upath, uskipped = write_csv(tmp, unknown_drive, unknown_records)
    check('write_csv: unknown-epoch filename', os.path.basename(upath),
          'drive-unknown-42.csv')
    with open(upath, newline='') as fh:
        urows = list(csv.DictReader(fh))
    check('write_csv: unknown-epoch iso column is empty', urows[0]['iso'], '')

    # The NVS clock floor is the only thing that places a drive-unknown in
    # time. It goes in as a comment on the SECOND line, so csv.DictReader
    # still sees the real header -- every reader of these files uses one.
    fpath, _ = write_csv(tmp, unknown_drive, unknown_records, 1756200000)
    with open(fpath) as fh:
        lines = fh.read().splitlines()
    check('write_csv: header is still the first line', lines[0], 'iso,t,key,value')
    check('write_csv: clock floor written as a comment on line 2',
          lines[1], '# clock floor: recorded after %s (board had no clock)'
          % __import__('datetime').datetime.fromtimestamp(1756200000)
              .isoformat(' ', 'seconds'))
    with open(fpath, newline='') as fh:
        frows = list(csv.DictReader(fh))
    check('write_csv: the comment does not become the header',
          list(frows[0].keys()), ['iso', 't', 'key', 'value'])
    # Exactly how mx5gauge.recorder.load_csv and tools/build_drive_asset.py
    # skip it: the numeric conversion fails and the row is dropped.
    usable = []
    for row in frows:
        try:
            usable.append((float(row['t']), row['key'], float(row['value'])))
        except (TypeError, ValueError, KeyError):
            continue
    check('write_csv: every real reader skips the comment row', len(usable), 1)

    # A known-epoch drive gets no comment -- its filename already says when.
    kpath, _ = write_csv(tmp, drive, records, 1756200000)
    with open(kpath) as fh:
        klines = fh.read().splitlines()
    check('write_csv: no floor comment when the clock was known',
          any(l.startswith('#') for l in klines), False)

    # A fresh clone has no logs/ at all. This used to traceback AFTER the
    # whole drive had been streamed off the board.
    fresh = os.path.join(tmp, 'fresh-clone')
    os.makedirs(fresh)
    npath, _ = write_csv(fresh, unknown_drive, unknown_records)
    check('write_csv: creates logs/ when it is missing', os.path.isfile(npath), True)
finally:
    shutil.rmtree(tmp, ignore_errors=True)


print()
if FAILED:
    print('%d FAILURE(S):' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('ALL PASSED')
