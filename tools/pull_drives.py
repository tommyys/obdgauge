"""Pull recorded drives off the gauge and write them into logs/.

The gauge records every reading of every drive to flash (SPEC.md s15). This
takes them off over the USB console and writes the same four-column CSV the
desk replay and tools/build_drive_asset.py already read -- so a drive nobody
watched can be replayed, and can be compiled straight back into the replay
library it will be played from.

The console is shared with `idf.py monitor`; they cannot both be open. If the
port is busy this says so rather than hanging.

Usage: .venv/bin/python tools/pull_drives.py [--port /dev/cu.usbmodemXXXX]
                                             [--list] [--force]

The pure parts below (chan_name, parse_stats, parse_drives, reassemble,
write_csv) take plain data in and plain data out -- no serial port -- so they
are importable and testable on their own. See tests/test_pull_drives.py.
"""
import argparse
import base64
import binascii
import datetime as dt
import glob
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing. Run: .venv/bin/pip install pyserial")

REC = struct.Struct('<IHHf')          # identical to build_drive_asset.py
TABLE_VERSION = 1

# The PID table from firmware/components/gauge_core/poll.cpp, in PID order,
# which is what a channel id indexes. If poll.cpp's table gains an entry in
# the middle, kChanTableVersion in logbuf.h must change and so must this --
# which is what the version check below is for.
PID_KEYS = [
    'fuel_status', 'load', 'coolant', 'stft1', 'ltft1', 'stft2', 'ltft2',
    'fuel_press', 'map', 'rpm', 'speed', 'timing', 'intake', 'maf',
    'throttle', 'o2_b1s1', 'o2_b1s2', 'run_time', 'dist_mil', 'rail_press',
    'rail_gauge', 'egr_cmd', 'egr_err', 'evap_purge', 'fuel_level', 'warmups',
    'dist_clear', 'evap_press', 'baro', 'cat_b1s1', 'cat_b2s1', 'cat_b1s2',
    'cat_b2s2', 'ctrl_volt', 'abs_load', 'equiv_ratio', 'rel_thr', 'ambient',
    'thr_b', 'thr_c', 'pedal_d', 'pedal_e', 'pedal_f', 'thr_actuator',
    'time_mil', 'time_clear', 'ethanol', 'rail_abs', 'pedal', 'hybrid_soc',
    'oil', 'inject_timing', 'fuel_rate', 'torque_demand', 'act_torque',
    'ref_torque',
]
EXTRAS_BASE = 0x0200
EXTRA_KEYS = ['volts', 'imu_ax', 'imu_ay', 'imu_az', 'imu_gz']
CHAN_DRIVE_START = 0xFFFF
CHAN_DRIVE_END = 0xFFFE


def chan_name(chan):
    if chan >= EXTRAS_BASE:
        i = chan - EXTRAS_BASE
        return EXTRA_KEYS[i] if i < len(EXTRA_KEYS) else None
    return PID_KEYS[chan] if chan < len(PID_KEYS) else None


def find_port():
    for pattern in ('/dev/cu.usbmodem*', '/dev/tty.usbmodem*'):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    sys.exit('no board found. Plug it in, or pass --port.')


class Gauge:
    def __init__(self, port):
        try:
            # 30 s, not 5. LIST scans all 2,544 sector headers once per
            # drive held, and GET scans the ring for its own drive before it
            # prints BEGIN -- with 20-30 drives on the board that is seconds
            # of silence, and a 5 s timeout turned it into a bogus "the board
            # stopped answering" in the middle of a pull.
            self.ser = serial.Serial(port, 115200, timeout=30)
        except serial.SerialException as e:
            sys.exit('cannot open %s: %s\n'
                     '(is `idf.py monitor` still open? they share the port)'
                     % (port, e))
        # The board prints its own log lines; drain whatever is in flight.
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, cmd):
        self.ser.write((cmd + '\n').encode())
        self.ser.flush()

    def lines(self):
        """Yields reply lines until OK/ERR, skipping the board's own logging."""
        while True:
            raw = self.ser.readline()
            if not raw:
                sys.exit('the board stopped answering')
            line = raw.decode('utf-8', 'replace').strip()
            if not line:
                continue
            # Board log lines are prefixed (I/W/E or "flight:"/"live:"); the
            # protocol's are not. Anything unrecognised is logging, not data.
            if line.startswith(('OK', 'ERR')):
                yield line
                return
            yield line

    def command(self, cmd):
        self.send(cmd)
        out = list(self.lines())
        if out[-1].startswith('ERR'):
            sys.exit('%s -> %s' % (cmd, out[-1]))
        return out[:-1], out[-1]


def parse_stats(body):
    """Pure: pick the `STATS k=v ...` line out of a STATS reply's body."""
    for line in body:
        if line.startswith('STATS '):
            return dict(kv.split('=', 1) for kv in line.split()[1:])
    sys.exit('no STATS in the reply')


def stats(g):
    body, _ = g.command('STATS')
    return parse_stats(body)


def parse_drives(body):
    """Pure: pick the `DRIVE k=v ...` lines out of a LIST reply's body."""
    out = []
    for line in body:
        if not line.startswith('DRIVE '):
            continue
        d = dict(kv.split('=', 1) for kv in line.split()[1:])
        out.append({'id': int(d['id']), 'epoch': int(d['epoch']),
                    'records': int(d['records']), 'ms': int(d['ms']),
                    'complete': d['complete'] == '1',
                    # The channel table the DRIVE was recorded under, stamped
                    # in its sector headers. Not the same thing as the table
                    # the firmware is running now -- that is what STATS says.
                    'table': int(d.get('table', TABLE_VERSION))})
    return out


def parse_truncated(terminator):
    """Pure: did LIST have more drives than it could show?

    `OK N drives` used to be the whole answer, which read as complete even
    when the board was holding drives it had no room to report. The board now
    ends LIST with `OK N drives truncated=0|1`.
    """
    for kv in terminator.split():
        if kv.startswith('truncated='):
            return kv.split('=', 1)[1] == '1'
    return False


# How many LIST pages one run will walk. 64 drives a page, so this is 6,400
# drives -- far past what the ring can hold. It exists only so a board that
# somehow kept saying "truncated" could not spin this loop forever.
MAX_LIST_PAGES = 100


def page_drives(fetch_page, max_pages=MAX_LIST_PAGES):
    """Pure: walk LIST's pages into one newest-first list of every drive held.

    `fetch_page(before_id)` returns `(drives, truncated)` -- LIST's reply for
    the newest page (`before_id` None) or for the page of drives older than
    `before_id`. Returns `(drives, incomplete)`; `incomplete` is true only if
    the walk stopped with the board still saying there is more, which now
    means the page cap, not the window.

    This exists because LIST only ever reports the newest 64 drives, and the
    caller skips drives already in logs/: without paging, "pull these, then
    run again" hands back the same 64 for ever and the older drives are never
    reachable at all.
    """
    held, seen, before = [], set(), None
    for _ in range(max_pages):
        page, truncated = fetch_page(before)
        # Ids are the ring's own, and a page is defined as strictly older than
        # the one before it -- but a drive that rolled off between two LISTs
        # must not be able to turn this into a loop or a duplicate pull.
        fresh = [d for d in page if d['id'] not in seen]
        held.extend(fresh)
        seen.update(d['id'] for d in fresh)
        if not truncated or not fresh:
            # `not fresh` while the board still says there is more means the
            # walk cannot advance: report that honestly rather than as done.
            return held, bool(truncated)
        before = fresh[-1]['id']
    return held, True


def drives(g, before=None):
    body, term = g.command('LIST' if before is None else 'LIST BEFORE %d' % before)
    return parse_drives(body), parse_truncated(term)


def reassemble(lines, drive_id):
    """Pure: turn the line stream after a `GET <id>` into decoded records.

    `lines` is any iterable of protocol lines (BEGIN, base64 body lines,
    board log lines interleaved, END, then OK/ERR) -- exactly what
    Gauge.lines() yields, or a canned list in a test. Verifies the crc32 and
    the record count the board reported in BEGIN/END and refuses (exits) on
    a mismatch, rather than writing a half-pulled drive.
    """
    blob = bytearray()
    expect = None
    crc = None
    for line in lines:
        if line.startswith('BEGIN '):
            expect = int(line.split()[2])
        elif line.startswith('END '):
            crc = int(line.split('=', 1)[1], 16)
        elif line.startswith(('OK', 'ERR')):
            if line.startswith('ERR'):
                sys.exit('GET %d -> %s' % (drive_id, line))
        elif expect is not None and crc is None:
            try:
                blob += base64.b64decode(line, validate=True)
            except binascii.Error:
                continue          # a board log line landed mid-stream
    got = binascii.crc32(bytes(blob)) & 0xFFFFFFFF
    if crc is not None and got != crc:
        sys.exit('drive %d: crc32 mismatch (got %08x, board said %08x)'
                 % (drive_id, got, crc))
    if expect is not None and len(blob) // REC.size != expect:
        sys.exit('drive %d: got %d records, board said %d'
                 % (drive_id, len(blob) // REC.size, expect))
    return [REC.unpack_from(blob, o) for o in range(0, len(blob), REC.size)]


def fetch(g, drive_id):
    g.send('GET %d' % drive_id)
    return reassemble(g.lines(), drive_id)


def write_csv(root, drive, records, clock_floor=0):
    """Pure: write one drive's records as the iso,t,key,value CSV.

    The drive-start/end markers (chan 0xFFFF/0xFFFE) are skipped entirely --
    the start marker's `value` carries the epoch as the raw bits of a
    uint32_t (logbuf.h's kChanDriveStart comment), not a float, and there is
    nothing else worth reading out of either marker. The epoch used for the
    iso column instead comes from `drive['epoch']`, which the board already
    decoded correctly (its own bits, not a float reinterpretation) when it
    built the LIST reply -- so this file never has to unpack that field.
    """
    epoch = drive['epoch']
    if epoch:
        start = dt.datetime.fromtimestamp(epoch)
        name = 'drive-%s.csv' % start.strftime('%Y%m%d-%H%M%S')
    else:
        # No clock when this was recorded. An honest name beats a wrong one.
        start = None
        name = 'drive-unknown-%d.csv' % drive['id']
    logs = os.path.join(root, 'logs')
    # A fresh clone has no logs/ directory. Without this the pull streamed the
    # whole drive off the board and only then tracebacked on open().
    os.makedirs(logs, exist_ok=True)
    path = os.path.join(logs, name)
    skipped = 0
    with open(path, 'w', newline='') as fh:
        fh.write('iso,t,key,value\n')
        if start is None and clock_floor:
            # The board had no clock when this was recorded, so the drive has
            # no timestamp -- but the clock floor persisted in NVS says it
            # happened AFTER this moment, and that is the only thing that
            # places a drive-unknown-N in time at all (design s5).
            #
            # Deliberately the SECOND line, not the first. Every reader of
            # these files (mx5gauge.recorder.load_csv,
            # tools/build_drive_asset.py, tools/dump_python_states.py) uses
            # csv.DictReader: a comment above the header would be taken FOR
            # the header, while here it is one row whose numeric conversions
            # fail and which all three already skip.
            fh.write('# clock floor: recorded after %s (board had no clock)\n'
                     % dt.datetime.fromtimestamp(clock_floor).isoformat(' ', 'seconds'))
        for t_ms, chan, _pad, value in records:
            if chan in (CHAN_DRIVE_START, CHAN_DRIVE_END):
                continue
            key = chan_name(chan)
            if key is None:
                skipped += 1
                continue
            t = t_ms / 1000.0
            if start:
                iso = (start + dt.timedelta(seconds=t)).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]
            else:
                iso = ''
            fh.write('%s,%.3f,%s,%g\n' % (iso, t, key, value))
    return path, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--list', action='store_true', help='show what is held, pull nothing')
    ap.add_argument('--force', action='store_true', help='re-pull drives already in logs/')
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    g = Gauge(args.port or find_port())

    s = stats(g)
    if int(s.get('table', -1)) != TABLE_VERSION:
        sys.exit('board channel table is v%s, this tool speaks v%d -- update '
                 'PID_KEYS from poll.cpp before trusting anything it says'
                 % (s.get('table'), TABLE_VERSION))
    # `starts` is drive-open markers, an upper bound -- the board cannot cheaply
    # say how many drives are still whole and offerable, and LIST below is the
    # authority. Saying "N drives held" here was overstating it.
    print('board: %s of %s sectors used (%s bytes), %s drive starts, %s records'
          % (s.get('used', '?'), s['sectors'], s.get('bytes', '?'),
             s.get('starts', s.get('drives', '?')), s['records']))
    dropped = int(s.get('dropped', 0))
    if dropped:
        print('  WARNING: %d samples were dropped -- the queue was full and '
              'those readings are gone' % dropped)
    write_fail = int(s.get('writefail', 0))
    if write_fail:
        print('  WARNING: %d flash writes FAILED -- some drive on this board '
              'has a hole in it. See the flight log for the first one.'
              % write_fail)
    clock_floor = int(s.get('floor', 0))

    now = int(time.time())
    g.command('TIME %d' % now)
    print('clock set to %s' % dt.datetime.fromtimestamp(now).isoformat(' ', 'seconds'))

    held, truncated = page_drives(lambda before: drives(g, before))
    if not held:
        print('no drives held.')
        return
    for d in held:
        when = (dt.datetime.fromtimestamp(d['epoch']).isoformat(' ', 'seconds')
                if d['epoch'] else 'clock unknown')
        print('  drive %-4d %-20s %7d records  %5.1f min%s%s'
              % (d['id'], when, d['records'], d['ms'] / 60000.0,
                 '' if d['complete'] else '  (unfinished)',
                 '' if d['table'] == TABLE_VERSION
                 else '  (channel table v%d -- CANNOT BE PULLED)' % d['table']))
    if truncated:
        print('  ...and MORE the board is still holding that this run could not'
              ' reach after %d pages of LIST. Report this -- it should not'
              ' happen; %d drives is far more than the ring can hold.'
              % (MAX_LIST_PAGES, len(held)))
    if args.list:
        return

    existing = set(os.path.basename(p) for p in glob.glob(os.path.join(root, 'logs', '*.csv')))
    for d in held:
        guess = ('drive-%s.csv' % dt.datetime.fromtimestamp(d['epoch']).strftime('%Y%m%d-%H%M%S')
                 if d['epoch'] else 'drive-unknown-%d.csv' % d['id'])
        if guess in existing and not args.force:
            print('drive %d already in logs/ as %s -- skipping' % (d['id'], guess))
            continue
        if d['table'] != TABLE_VERSION:
            print('drive %d was recorded under channel table v%d, this tool '
                  'speaks v%d -- skipping rather than mislabelling every '
                  'channel in it' % (d['id'], d['table'], TABLE_VERSION))
            continue
        print('pulling drive %d (%d records)...' % (d['id'], d['records']))
        records = fetch(g, d['id'])
        path, skipped = write_csv(root, d, records, clock_floor)
        print('  wrote %s (%d rows%s)'
              % (os.path.relpath(path, root), len(records),
                 ', %d unknown channels skipped' % skipped if skipped else ''))


if __name__ == '__main__':
    main()
