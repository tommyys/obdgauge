"""Unit tests for vehicle identity (VIN decode) and mode-09 parsing.
   Run: .venv/bin/python tests/test_vehicle.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import pids, vehicle  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-46s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


# --- mode 09 / VIN reassembly ----------------------------------------------

# A real multi-frame VIN reply: five lines, each 49 02 <seq> then payload.
MULTI = ('49 02 01 00 00 00 4A\r'
         '49 02 02 4D 30 4E 44\r'
         '49 02 03 41 31 52 30\r'
         '49 02 04 31 32 33 34\r'
         '49 02 05 35 36 37\r')
check('multiframe VIN reassembles',
      vehicle.clean_vin(''.join(chr(c) for c in pids.parse_mode09(MULTI, 0x02))),
      'JM0NDA1R01234567')

# ISO-TP style: one blob, leading length byte and padding nulls
SINGLE = '014 \r49 02 01 4A 4D 30 4E 44 41 31 52 30 31 32 33 34 35 36 37 38'
check('single-blob VIN reassembles',
      vehicle.clean_vin(''.join(chr(c) for c in pids.parse_mode09(SINGLE, 0x02))),
      'JM0NDA1R012345678')
check('mode09 returns None when header absent',
      pids.parse_mode09('41 0C 1A F8', 0x02), None)

# --- VIN field decode ------------------------------------------------------
check('clean_vin strips junk', vehicle.clean_vin(' jm0nd-a1r0 1234567 '),
      'JM0NDA1R01234567')
check('valid_vin needs 17 chars', vehicle.valid_vin('JM0NDA1R012345678'), True)
check('valid_vin rejects short', vehicle.valid_vin('JM0ND'), False)

check('make from WMI JM0 -> Mazda', vehicle.make_of('JM0NDA1R012345678'), 'Mazda')
check('make from WMI JHM -> Honda', vehicle.make_of('JHMFC1E30JH000001'), 'Honda')
check('make from WMI JTJ -> Lexus', vehicle.make_of('JTJDARDZ102000001'), 'Lexus')
check('make unknown WMI -> None', vehicle.make_of('ZZZNDA1R012345678'), None)

# position 10 is the model year. R = 2024 in the current cycle.
check('year code R -> 2024', vehicle.model_year('JM0NDA1R0R2345678'), 2024)
check('year code L -> 2020', vehicle.model_year('JM0NDA1R0L2345678'), 2020)
check('year code T -> 2026', vehicle.model_year('JM0NDA1R0T2345678'), 2026)
# a letter 30 years ahead must resolve to the past, not the future
check('year code W -> 1998 not 2028', vehicle.model_year('JM0NDA1R0W2345678'), 1998)
check('year code Y -> 2000 not 2030', vehicle.model_year('JM0NDA1R0Y2345678'), 2000)
check('year digit 5 -> 2005 not 2035', vehicle.model_year('JM0NDA1R052345678'), 2005)
check('year invalid code -> None', vehicle.model_year('JM0NDA1R0I2345678'), None)

# --- identify --------------------------------------------------------------
mx5 = vehicle.identify(vin='JM0NDA1R0R2345678')
check('identify make', mx5['make'], 'Mazda')
check('identify year', mx5['year'], 2024)
check('identify label', mx5['label'], 'MAZDA')
check('identify known', mx5['known'], True)
check('identify uses make profile', mx5['rpm_red'], 6200)

named = vehicle.identify(vin='JM0NDA1R0R2345678', model='MX-5')
check('explicit model in label', named['label'], 'MAZDA MX-5')
check('model picks specific profile', named['rpm_red'], 7000)
check('model picks specific redline', named['rpm_max'], 8000)

unknown = vehicle.identify(vin='ZZZNDA1R0R2345678')
check('unknown WMI shows the code', unknown['label'], 'WMI ZZZ')
check('unknown WMI not "known"', unknown['known'], False)
check('unknown WMI gets default profile',
      unknown['rpm_max'], vehicle.DEFAULT_PROFILE['rpm_max'])

none = vehicle.identify()
check('no VIN at all -> OBD-II', none['label'], 'OBD-II')
check('no VIN -> vin None', none['vin'], None)

# explicit values always beat the VIN's own
override = vehicle.identify(vin='JM0NDA1R0R2345678', make='Honda', year=2019)
check('explicit make wins', override['make'], 'Honda')
check('explicit year wins', override['year'], 2019)

# --- capture headers -------------------------------------------------------
cap = vehicle.from_capture_header({'carprofile': 'Mazda OBD-II / EOBD',
                                   'car': 'My car'})
check('capture header make', cap['make'], 'Mazda')
check('capture ignores "My car" label', cap['model'], None)
check('capture has no year', cap['year'], None)
check('capture source tagged', cap['source'], 'capture')

# --- supported PIDs -> channel keys ----------------------------------------
keys = pids.keys_for({0x0C, 0x0D, 0x05, 0x5C, 0x63})
check('keys_for maps rpm', 'rpm' in keys, True)
check('keys_for maps oil', 'oil' in keys, True)
check('keys_for skips undecodable pid', pids.keys_for({0x99}), set())

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all vehicle tests passed')
