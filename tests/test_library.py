"""Unit tests for the drive library: filename dates, summary lines, and the
path checking that stops a request selecting a file we never listed.
   Run: .venv/bin/python tests/test_library.py"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from mx5gauge import library  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-52s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


def when(name):
    ts = library._time_from_name(name)
    return None if ts is None else time.strftime('%Y-%m-%d %H:%M:%S',
                                                 time.localtime(ts))


# --- the drive's own timestamp, from its filename -------------------------
# mtime is useless here: every capture in the repo shares one, which would make
# the picker's most useful column identical on every row.
check('Car Scanner name', when('2026-08-11 21-43-36.brc'), '2026-08-11 21:43:36')
check('our own drive name', when('drive-20260812-212705.csv'),
      '2026-08-12 21:27:05')
check('replay- prefix too', when('replay-20260812-212705.csv'),
      '2026-08-12 21:27:05')
check('full path is fine', when('/a/b/2026-08-11 21-20-11.brc'),
      '2026-08-11 21:20:11')
check('unparseable name -> None', when('random-notes.csv'), None)
check('empty-ish name -> None', when('.brc'), None)

# --- summary lines --------------------------------------------------------
# our own drives can quote distance and a score; captures cannot, because iOS
# suspends Car Scanner and leaves gaps that make integrated distance nonsense
drive = {'kind': 'drive', 'duration_s': 1140.0, 'dist_km': 12.43,
         'score': 78.4, 'channels': ['rpm'], 'samples': 900,
         'moving_samples': None}
check('drive quotes km, minutes, score', library._summary_line(drive),
      '12.4 km · 19 min · score 78')

cap = {'kind': 'capture', 'duration_s': 1643.0, 'dist_km': 0.004,
       'score': None, 'channels': ['rpm'] * 18, 'samples': 28682,
       'moving_samples': 0}
check('capture quotes channels and points, flags idle',
      library._summary_line(cap), '18 ch · 28.7k pts · idle · 27 min')

cap_moving = dict(cap, moving_samples=365, samples=900, duration_s=30.0)
check('capture with driving is not flagged idle',
      library._summary_line(cap_moving), '18 ch · 900 pts')

check('unreadable says so',
      library._summary_line({'kind': 'unreadable'}), 'unreadable')

# A drive cut short (laptop shut, ignition off) has no .json sidecar, so no
# distance and no score. It must still describe itself — calling a 4000-sample
# drive "empty" invites deleting a real one.
cut = {'kind': 'drive', 'duration_s': 1020.0, 'dist_km': None, 'score': None,
       'channels': [], 'samples': 4000, 'moving_samples': None,
       'partial': True}
check('interrupted drive quotes what it has',
      library._summary_line(cut), '4.0k pts · 17 min · interrupted')
check('never renders as empty', library._summary_line(cut) == 'empty', False)

# --- resolve(): only ever a file the library already listed ---------------
# The picker POSTs a name back to us. Matching on basename against the real
# listing means a crafted string cannot reach anything else on disk.
real = library.scan(ROOT)
check('library found the sample capture', bool(real), True)
if real:
    name = real[0]['name']
    check('resolve accepts a listed drive',
          library.resolve(ROOT, name)['name'], name)
    check('resolve strips any directory part',
          library.resolve(ROOT, '/etc/../tmp/' + name)['name'], name)

for hostile in ('../mx5gauge/server.py', '../../../../etc/passwd',
                'mx5gauge/state.py', '/etc/passwd', '', None):
    check('resolve rejects %r' % (hostile,), library.resolve(ROOT, hostile), None)

# --- scan ordering --------------------------------------------------------
stamps = [e['recorded_at'] for e in real]
check('newest first', stamps == sorted(stamps, reverse=True), True)
check('every entry has a summary', all(e.get('summary') for e in real), True)
check('every entry names a real file',
      all(os.path.isfile(e['path']) for e in real), True)

# --- recorder: a failed session must leave nothing behind ------------------
# The first BLE connect in the car often needs a retry (the phone may still
# hold the link). If each attempt left a header-only CSV, the Drives view would
# fill with "empty" rows.
import shutil, tempfile
from mx5gauge import recorder  # noqa: E402

tmp = tempfile.mkdtemp()
logs = os.path.join(tmp, 'logs')
rec = recorder.Recorder(logs, prefix='drive', enabled=True)
check('path is known up front (printable at startup)', bool(rec.path), True)
check('but no file until there is data', os.path.exists(rec.path), False)
check('close() reports nothing recorded', rec.close(summary={}), None)
check('no logs dir created either', os.path.isdir(logs), False)
check('library lists nothing', library.scan(tmp), [])

rec = recorder.Recorder(logs, prefix='drive', enabled=True)
rec.write('rpm', 812.0, 0.0)
check('file appears on the first reading', os.path.exists(rec.path), True)
rec.write('_car', {'label': 'X'}, 0.1)          # metadata is not a reading
saved = rec.close(summary={'derived': {'dist_km': 1.5, 'elapsed_s': 120.0},
                           'score': {'total': 70.0}})
check('close() returns the path once there is data', saved == rec.path, True)
with open(saved) as fh:
    body = fh.read()
check('metadata kept out of the log', '_car' in body, False)
check('the reading is in the log', ',rpm,812.0' in body, True)
shutil.rmtree(tmp)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all library tests passed')
