"""Reader for Car Scanner `.brc` recordings.

Format (little-endian), reverse-engineered from captures:

  header : pstr "CARSCANNERRECORD", int32 version, pstr profile-id,
           pstr car name, pstr car profile, pstr adapter, 12 unknown bytes
  then a stream of two record kinds:
    NAMED  (first time a sensor appears)
           int32 id, pstr full name, pstr short name, int32 A, uint32 aux, <core>
    UPDATE <core>
    core = double ts(sec), int32 id, double value, uint32 aux

Sensor ids match OBD PIDs for standard channels (5=coolant, 12=rpm, 13=speed).
"""
import struct

MAGIC = 'CARSCANNERRECORD'


def _pstr(b, p):
    n = b[p]
    return b[p + 1:p + 1 + n].decode('latin-1'), p + 1 + n


def _i32(b, p):
    return struct.unpack_from('<i', b, p)[0], p + 4


def _u32(b, p):
    return struct.unpack_from('<I', b, p)[0], p + 4


def _f64(b, p):
    return struct.unpack_from('<d', b, p)[0], p + 8


def _looks_named(b, p):
    if p + 5 > len(b):
        return False
    sid = struct.unpack_from('<i', b, p)[0]
    if not (0 <= sid <= 5000):
        return False
    ln = b[p + 4]
    if not (1 <= ln <= 45) or p + 5 + ln > len(b):
        return False
    try:
        name = b[p + 5:p + 5 + ln].decode('ascii')
    except UnicodeDecodeError:
        return False
    return name.isprintable()


def _core(b, p):
    ts, p = _f64(b, p)
    sid, p = _i32(b, p)
    val, p = _f64(b, p)
    aux, p = _u32(b, p)
    return ts, sid, val, aux, p


def parse(path):
    """Return (header dict, {id: (full, short)}, [(ts, id, value)], skipped)."""
    with open(path, 'rb') as fh:
        b = fh.read()
    p = 0
    magic, p = _pstr(b, p)
    ver, p = _i32(b, p)
    profile, p = _pstr(b, p)
    car, p = _pstr(b, p)
    carprofile, p = _pstr(b, p)
    adapter, p = _pstr(b, p)
    p += 12
    header = dict(magic=magic, version=ver, profile=profile, car=car,
                  carprofile=carprofile, adapter=adapter)

    names = {}
    rows = []
    n = len(b)
    skipped = 0
    while p + 24 <= n:
        try:
            if _looks_named(b, p):
                sid, q = _i32(b, p)
                full, q = _pstr(b, q)
                short, q = _pstr(b, q)
                _a, q = _i32(b, q)
                _aux, q = _u32(b, q)
                ts, sid2, val, _aux2, q = _core(b, q)
                if sid2 != sid:
                    raise ValueError('named core id mismatch')
                names[sid] = (full, short)
                rows.append((ts, sid, val))
                p = q
            else:
                ts, sid, val, _aux, q = _core(b, p)
                if not (0.0 <= ts <= 1e6):
                    raise ValueError('ts out of range')
                if sid not in names and not (0 <= sid <= 5000):
                    raise ValueError('id out of range')
                if val != val or abs(val) > 1e9:      # NaN / absurd
                    raise ValueError('value out of range')
                rows.append((ts, sid, val))
                p = q
        except Exception:
            skipped += 1
            p += 1
    rows.sort(key=lambda r: r[0])
    return header, names, rows, skipped
