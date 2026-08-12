"""Unit tests for the pure decode layer. Run: .venv/bin/python -m pytest -q
   (or plain: .venv/bin/python tests/test_pids.py)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import pids  # noqa: E402

FAILED = []


def check(name, got, want):
    ok = got == want
    if not ok:
        FAILED.append('%s: got %r want %r' % (name, got, want))
    print('%-42s %s  (%r)' % (name, 'ok ' if ok else 'FAIL', got))


# --- decode formulas --------------------------------------------------------
check('rpm 1A F8 -> 1726', pids.dec_rpm([0x1A, 0xF8]), 1726.0)
check('rpm 0C 60 -> 792',  pids.dec_rpm([0x0C, 0x60]), 792.0)
check('rpm 00 00 -> 0',    pids.dec_rpm([0x00, 0x00]), 0.0)
check('coolant 0x5A -> 50C', pids.dec_temp([0x5A]), 50)
check('coolant 0x28 -> 0C',  pids.dec_temp([0x28]), 0)
check('coolant 0x00 -> -40C', pids.dec_temp([0x00]), -40)
check('speed 0x64 -> 100',  pids.dec_speed([0x64]), 100.0)
check('throttle 0xFF -> 100%', pids.dec_percent([0xFF]), 100.0)
check('throttle 0x00 -> 0%',   pids.dec_percent([0x00]), 0.0)
check('fuel trim 0x80 -> 0%',  pids.dec_fuel_trim([0x80]), 0.0)
check('maf 01 F4 -> 5.0 g/s',  pids.dec_maf([0x01, 0xF4]), 5.0)
check('timing 0x80 -> 0 deg',  pids.dec_timing([0x80]), 0.0)
check('fuel rate 00 64 -> 5 L/h', pids.dec_fuel_rate([0x00, 0x64]), 5.0)
check('ctrl volt 37 6C -> 14.188V', pids.dec_control_voltage([0x37, 0x6C]), 14.188)
check('ref torque 00 FA -> 250Nm', pids.dec_ref_torque([0x00, 0xFA]), 250.0)
check('short payload -> None', pids.dec_rpm([0x1A]), None)

# --- response parsing -------------------------------------------------------
check('parse 41 0C 1A F8', pids.parse_mode01('41 0C 1A F8', 0x0C), [0x1A, 0xF8])
check('parse no spaces',   pids.parse_mode01('410C1AF8', 0x0C), [0x1A, 0xF8])
check('parse with prompt', pids.parse_mode01('41 0C 1A F8 \r>', 0x0C), [0x1A, 0xF8])
check('parse wrong pid -> None', pids.parse_mode01('41 0D 20', 0x0C), None)
check('parse NO DATA -> None', pids.parse_mode01('NO DATA', 0x0C), None)
check('parse with header noise',
      pids.parse_mode01('7E8 03 41 0C 1A F8', 0x0C), [0x1A, 0xF8])

# --- end-to-end decode ------------------------------------------------------
check('decode rpm', pids.decode(0x0C, pids.parse_mode01('41 0C 1A F8', 0x0C)),
      ('rpm', 1726.0))
check('decode coolant', pids.decode(0x05, pids.parse_mode01('41 05 5A', 0x05)),
      ('coolant', 50))

# --- supported-PID bitmask --------------------------------------------------
# 0xBE1FA813 is the classic example mask for PIDs 01-20
sup = pids.parse_supported([0xBE, 0x1F, 0xA8, 0x13], 0x00)
check('bitmask contains 0x0C (rpm)', 0x0C in sup, True)
check('bitmask contains 0x0D (speed)', 0x0D in sup, True)
check('bitmask excludes 0x02', 0x02 in sup, False)
check('MSB of A means base+1', 0x01 in pids.parse_supported([0x80, 0, 0, 0], 0x00), True)
check('LSB of D means base+32', 0x20 in pids.parse_supported([0, 0, 0, 0x01], 0x00), True)
check('base 0x20 offsets', 0x21 in pids.parse_supported([0x80, 0, 0, 0], 0x20), True)

# --- voltage ----------------------------------------------------------------
check('ATRV 13.8V', pids.parse_voltage('13.8V'), 13.8)
check('ATRV 14.4V\\r>', pids.parse_voltage('14.4V\r>'), 14.4)
check('ATRV junk -> None', pids.parse_voltage('ELM327'), None)

# --- poll cycle -------------------------------------------------------------
cyc = pids.build_poll_cycle({0x0C, 0x0D, 0x05})
check('poll cycle includes rpm', 0x0C in cyc, True)
check('poll cycle drops unsupported oil', 0x5C in cyc, False)
check('poll cycle empty when nothing supported', pids.build_poll_cycle(set()), [])

# log_all sweeps every supported PID; display-only keeps the short list
wide = pids.build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, log_all=True)
check('log_all includes fuel level 0x2F', 0x2F in wide, True)
check('log_all includes ambient 0x46', 0x46 in wide, True)
narrow = pids.build_poll_cycle({0x0C, 0x0D, 0x05, 0x2F, 0x46}, log_all=False)
check('display-only skips fuel level', 0x2F in narrow, False)
check('display-only keeps coolant', 0x05 in narrow, True)
check('rpm interleaved between slow pids', wide.count(0x0C) > 1, True)

# newly added decoders
check('catalyst 0F A0 -> 360.0C', pids.dec_catalyst_temp([0x0F, 0xA0]), 360.0)
check('catalyst 00 00 -> -40.0C', pids.dec_catalyst_temp([0x00, 0x00]), -40.0)
check('lambda 80 00 -> 1.0', pids.dec_equiv_ratio([0x80, 0x00]), 1.0)
check('O2 voltage 0x64 -> 0.5V', pids.dec_o2_voltage([0x64]), 0.5)
check('fuel pressure 0x64 -> 300kPa', pids.dec_fuel_pressure([0x64]), 300.0)
check('runtime 00 3C -> 60s', pids.dec_u16([0x00, 0x3C]), 60.0)

print()
if FAILED:
    print('%d FAILURES:' % len(FAILED))
    for f in FAILED:
        print('  -', f)
    sys.exit(1)
print('all decode tests passed')
