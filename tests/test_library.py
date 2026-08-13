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

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all library tests passed')
