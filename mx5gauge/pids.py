"""OBD-II Mode 01 PID definitions and decode formulas.

Pure Python, no dependencies — this is the module that gets unit-tested on the
host and later ported almost line-for-line to C++ for the ESP32 firmware.

Every decoder takes the response *data bytes* (everything after the `41 <pid>`
header) and returns engineering units, or None if the payload is too short.
"""

# --- individual decoders ----------------------------------------------------

def dec_rpm(d):
    # ((A*256)+B)/4  -> rpm
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 4.0


def dec_temp(d):
    # A - 40 -> deg C   (coolant, intake air, oil, ...)
    if len(d) < 1:
        return None
    return d[0] - 40


def dec_speed(d):
    # A -> km/h
    if len(d) < 1:
        return None
    return float(d[0])


def dec_percent(d):
    # A * 100/255 -> %
    if len(d) < 1:
        return None
    return d[0] * 100.0 / 255.0


def dec_fuel_trim(d):
    # (A - 128) * 100/128 -> %
    if len(d) < 1:
        return None
    return (d[0] - 128) * 100.0 / 128.0


def dec_maf(d):
    # ((A*256)+B)/100 -> g/s
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 100.0


def dec_timing(d):
    # (A/2) - 64 -> deg before TDC
    if len(d) < 1:
        return None
    return (d[0] / 2.0) - 64


def dec_pressure_kpa(d):
    if len(d) < 1:
        return None
    return float(d[0])


def dec_fuel_rate(d):
    # ((A*256)+B)/20 -> L/h
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 20.0


def dec_control_voltage(d):
    # ((A*256)+B)/1000 -> V
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 1000.0


def dec_torque_pct(d):
    # A - 125 -> %
    if len(d) < 1:
        return None
    return d[0] - 125


def dec_ref_torque(d):
    # (A*256)+B -> Nm
    if len(d) < 2:
        return None
    return float((d[0] * 256) + d[1])


def dec_u8(d):
    if len(d) < 1:
        return None
    return float(d[0])


def dec_u16(d):
    if len(d) < 2:
        return None
    return float((d[0] * 256) + d[1])


def dec_fuel_pressure(d):
    # 3*A -> kPa
    if len(d) < 1:
        return None
    return d[0] * 3.0


def dec_rail_pressure(d):
    # 0.079 * ((A*256)+B) -> kPa
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) * 0.079


def dec_rail_gauge(d):
    # 10 * ((A*256)+B) -> kPa
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) * 10.0


def dec_o2_voltage(d):
    # A/200 -> V
    if len(d) < 1:
        return None
    return d[0] / 200.0


def dec_equiv_ratio(d):
    # ((A*256)+B)/32768 -> ratio (lambda)
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 32768.0


def dec_evap_pressure(d):
    # ((A*256)+B)/4 - 8192 -> Pa
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 4.0 - 8192.0


def dec_inject_timing(d):
    # ((A*256)+B)/128 - 210 -> deg
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 128.0 - 210.0


def dec_egr_error(d):
    # (A-128)*100/128 -> %
    if len(d) < 1:
        return None
    return (d[0] - 128) * 100.0 / 128.0


def dec_catalyst_temp(d):
    # ((A*256)+B)/10 - 40 -> deg C
    if len(d) < 2:
        return None
    return ((d[0] * 256) + d[1]) / 10.0 - 40.0


def dec_torque_demand(d):
    # A - 125 -> %
    if len(d) < 1:
        return None
    return d[0] - 125


# --- PID table --------------------------------------------------------------
# The full standard SAE J1979 Mode 01 set we can decode. Everything the car
# reports as supported gets logged, not just what the gauge draws.
# pid -> (key, label, unit, decoder)
PIDS = {
    0x03: ('fuel_status',   'Fuel system status',   '',     dec_u8),
    0x04: ('load',          'Engine load',          '%',    dec_percent),
    0x05: ('coolant',       'Coolant temp',         'C',    dec_temp),
    0x06: ('stft1',         'Short fuel trim B1',   '%',    dec_fuel_trim),
    0x07: ('ltft1',         'Long fuel trim B1',    '%',    dec_fuel_trim),
    0x08: ('stft2',         'Short fuel trim B2',   '%',    dec_fuel_trim),
    0x09: ('ltft2',         'Long fuel trim B2',    '%',    dec_fuel_trim),
    0x0A: ('fuel_press',    'Fuel pressure',        'kPa',  dec_fuel_pressure),
    0x0B: ('map',           'Intake manifold',      'kPa',  dec_pressure_kpa),
    0x0C: ('rpm',           'Engine RPM',           'rpm',  dec_rpm),
    0x0D: ('speed',         'Vehicle speed',        'km/h', dec_speed),
    0x0E: ('timing',        'Timing advance',       'deg',  dec_timing),
    0x0F: ('intake',        'Intake air temp',      'C',    dec_temp),
    0x10: ('maf',           'MAF flow',             'g/s',  dec_maf),
    0x11: ('throttle',      'Throttle position',    '%',    dec_percent),
    0x14: ('o2_b1s1',       'O2 B1S1 voltage',      'V',    dec_o2_voltage),
    0x15: ('o2_b1s2',       'O2 B1S2 voltage',      'V',    dec_o2_voltage),
    0x1F: ('run_time',      'Run time since start', 's',    dec_u16),
    0x21: ('dist_mil',      'Distance with MIL',    'km',   dec_u16),
    0x22: ('rail_press',    'Fuel rail pressure',   'kPa',  dec_rail_pressure),
    0x23: ('rail_gauge',    'Fuel rail gauge',      'kPa',  dec_rail_gauge),
    0x2C: ('egr_cmd',       'Commanded EGR',        '%',    dec_percent),
    0x2D: ('egr_err',       'EGR error',            '%',    dec_egr_error),
    0x2E: ('evap_purge',    'Commanded evap purge', '%',    dec_percent),
    0x2F: ('fuel_level',    'Fuel tank level',      '%',    dec_percent),
    0x30: ('warmups',       'Warm-ups since clear', '',     dec_u8),
    0x31: ('dist_clear',    'Distance since clear', 'km',   dec_u16),
    0x32: ('evap_press',    'Evap vapour pressure', 'Pa',   dec_evap_pressure),
    0x33: ('baro',          'Barometric press.',    'kPa',  dec_pressure_kpa),
    0x3C: ('cat_b1s1',      'Catalyst temp B1S1',   'C',    dec_catalyst_temp),
    0x3D: ('cat_b2s1',      'Catalyst temp B2S1',   'C',    dec_catalyst_temp),
    0x3E: ('cat_b1s2',      'Catalyst temp B1S2',   'C',    dec_catalyst_temp),
    0x3F: ('cat_b2s2',      'Catalyst temp B2S2',   'C',    dec_catalyst_temp),
    0x42: ('ctrl_volt',     'Control module V',     'V',    dec_control_voltage),
    0x43: ('abs_load',      'Absolute load',        '%',    dec_percent),
    0x44: ('equiv_ratio',   'Commanded lambda',     '',     dec_equiv_ratio),
    0x45: ('rel_thr',       'Relative throttle',    '%',    dec_percent),
    0x46: ('ambient',       'Ambient air temp',     'C',    dec_temp),
    0x47: ('thr_b',         'Absolute throttle B',  '%',    dec_percent),
    0x48: ('thr_c',         'Absolute throttle C',  '%',    dec_percent),
    0x49: ('pedal_d',       'Accel pedal D',        '%',    dec_percent),
    0x4A: ('pedal_e',       'Accel pedal E',        '%',    dec_percent),
    0x4B: ('pedal_f',       'Accel pedal F',        '%',    dec_percent),
    0x4C: ('thr_actuator',  'Commanded throttle',   '%',    dec_percent),
    0x4D: ('time_mil',      'Time with MIL on',     'min',  dec_u16),
    0x4E: ('time_clear',    'Time since clear',     'min',  dec_u16),
    0x52: ('ethanol',       'Ethanol fuel',         '%',    dec_percent),
    0x59: ('rail_abs',      'Fuel rail abs press.', 'kPa',  dec_rail_gauge),
    0x5A: ('pedal',         'Accelerator pedal',    '%',    dec_percent),
    0x5B: ('hybrid_soc',    'Hybrid batt. life',    '%',    dec_percent),
    0x5C: ('oil',           'Oil temp',             'C',    dec_temp),  # absent on ND3
    0x5D: ('inject_timing', 'Fuel injection timing', 'deg', dec_inject_timing),
    0x5E: ('fuel_rate',     'Engine fuel rate',     'L/h',  dec_fuel_rate),
    0x61: ('torque_demand', 'Driver demand torque', '%',    dec_torque_demand),
    0x62: ('act_torque',    'Actual torque',        '%',    dec_torque_pct),
    0x63: ('ref_torque',    'Reference torque',     'Nm',   dec_ref_torque),
}

# PIDs the gauge draws — polled between every other reading so the needle stays
# responsive while the long tail is swept for the log.
# rpm, speed, throttle — and run time since start, which is not a display
# channel at all: it is how `ignition` catches an engine that restarted while
# we were disconnected. On the slow sweep it came round every ~28 s, which is
# long enough to lose the start of a drive.
POLL_FAST = [0x0C, 0x0D, 0x11, 0x1F]
# Preferred order for the rest, so the display-relevant ones refresh soonest.
POLL_PRIORITY = [0x05, 0x0F, 0x5E, 0x04, 0x42, 0x63, 0x62, 0x5C, 0x3C, 0x33]


def build_poll_cycle(supported, log_all=True):
    """Build the polling rotation.

    log_all=True  -> sweep every supported PID we can decode, so the recorded
                     log captures the whole car, not just the gauge's subset.
    log_all=False -> poll only the display set (snappier, thinner log).

    Fast PIDs are interleaved between every slow one, so rpm/speed stay live
    even while a long sweep is in progress.
    """
    fast = [p for p in POLL_FAST if p in supported]
    if log_all:
        rest = [p for p in POLL_PRIORITY if p in supported and p not in fast]
        rest += [p for p in sorted(supported)
                 if p in PIDS and p not in fast and p not in rest]
    else:
        rest = [p for p in POLL_PRIORITY if p in supported and p not in fast]
    if not fast and not rest:
        return []
    if not rest:
        return fast
    cycle = []
    for pid in rest:
        cycle.extend(fast)
        cycle.append(pid)
    return cycle


# --- response parsing -------------------------------------------------------

def _hex_bytes(text):
    """Collect hex byte values from an ELM327 reply, ignoring spaces/prompt."""
    out = []
    hi = None
    for ch in text:
        if '0' <= ch <= '9':
            v = ord(ch) - 48
        elif 'a' <= ch <= 'f':
            v = ord(ch) - 87
        elif 'A' <= ch <= 'F':
            v = ord(ch) - 55
        else:
            hi = None          # non-hex resets any half byte
            continue
        if hi is None:
            hi = v
        else:
            out.append((hi << 4) | v)
            hi = None
    return out


def parse_mode01(text, pid):
    """Parse '41 0C 1A F8' -> [0x1A, 0xF8] for pid 0x0C, else None.

    Tolerates multi-line/multi-ECU replies by scanning for the `41 <pid>`
    header anywhere in the byte stream.
    """
    b = _hex_bytes(text)
    for i in range(len(b) - 1):
        if b[i] == 0x41 and b[i + 1] == pid:
            return b[i + 2:]
    return None


def parse_mode09(text, pid):
    """Parse a mode-09 (vehicle info) reply, returning its payload bytes.

    Mode 09 answers are multi-frame: the VIN arrives as several `49 02 <n>`
    lines, each carrying a slice of the string. We concatenate every slice in
    arrival order, so the caller sees one continuous payload.

    Frame counters and ISO-TP padding are dropped here rather than by the
    caller: any byte below 0x20 cannot be part of a printable field, so this
    stays correct without needing to know the field's layout.
    """
    b = _hex_bytes(text)
    out = []
    i = 0
    n = len(b)
    seen_header = False
    while i < n - 1:
        if b[i] == 0x49 and b[i + 1] == pid:
            seen_header = True
            i += 2
            continue
        if seen_header and b[i] >= 0x20:
            out.append(b[i])
        i += 1
    if seen_header and i < n and b[i] >= 0x20:
        out.append(b[i])
    return out if seen_header else None


def keys_for(supported):
    """Channel keys the car can supply, from its supported-PID set.

    Turns the raw PID numbers the adapter reports into our own key namespace,
    which is what the views ask about. PIDs we cannot decode are ignored —
    the car offering one does us no good if we can't read it.
    """
    out = set()
    for pid in supported or ():
        entry = PIDS.get(pid)
        if entry:
            out.add(entry[0])
    return out


def decode(pid, data):
    """Decode data bytes for a pid. Returns (key, value) or None."""
    entry = PIDS.get(pid)
    if entry is None or data is None:
        return None
    key, _label, _unit, fn = entry
    val = fn(data)
    if val is None:
        return None
    return key, val


def parse_supported(data, base):
    """Decode a supported-PID bitmask reply (PIDs 0x00/0x20/0x40/0x60).

    Bit 31 (MSB of byte A) = base+1 ... bit 0 (LSB of D) = base+32.
    """
    if data is None or len(data) < 4:
        return set()
    mask = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
    found = set()
    for i in range(32):
        if mask & (1 << (31 - i)):
            found.add(base + 1 + i)
    return found


def parse_voltage(text):
    """Parse an ELM327 ATRV reply, e.g. '13.8V' -> 13.8.

    Requires a 'V' suffix and a plausible range, so device banners like
    'ELM327 v1.5' don't get mistaken for a reading.
    """
    i = 0
    n = len(text)
    while i < n:
        if text[i].isdigit():
            start = i
            while i < n and (text[i].isdigit() or text[i] == '.'):
                i += 1
            num = text[start:i]
            # must be followed by V (optionally after spaces)
            j = i
            while j < n and text[j] == ' ':
                j += 1
            if j < n and text[j] in 'Vv':
                try:
                    val = float(num)
                except ValueError:
                    return None
                if 0.0 < val <= 60.0:
                    return val
            continue
        i += 1
    return None
