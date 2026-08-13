"""The library of past drives the gauge can replay.

Two kinds of file are replayable and both belong in the list:

  logs/*.csv      drives this gauge recorded itself, usually with a .json
                  summary beside them holding the distance and score we
                  already worked out when the session closed
  captures/*.brc  Car Scanner recordings, which predate the gauge

Summarising a .brc means parsing it, which is far too slow to do on every
request, so results are cached against the file's size and mtime. On the board
this module becomes the SD-card index — same shape, cheaper source.
"""
import json
import os
import time

from . import brc, sources

# path -> (size, mtime, entry). Invalidated when the file changes on disk.
_cache = {}


def _fmt_when(ts):
    return time.strftime('%d %b %H:%M', time.localtime(ts))


# Both naming schemes carry the moment the drive happened:
#   Car Scanner : '2026-08-11 21-43-36.brc'
#   our own     : 'drive-20260812-212705.csv'
_NAME_FORMATS = ('%Y-%m-%d %H-%M-%S', '%Y%m%d-%H%M%S')


def _time_from_name(name):
    """The drive's own timestamp, parsed out of its filename, or None.

    Preferred over mtime, which records when the file was last copied or
    touched — every capture in this repo shares an mtime, which would make the
    picker's most useful column identical on every row.
    """
    stem = os.path.splitext(os.path.basename(name))[0]
    for prefix in ('drive-', 'replay-'):
        if stem.startswith(prefix):
            stem = stem[len(prefix):]
            break
    for fmt in _NAME_FORMATS:
        try:
            return time.mktime(time.strptime(stem, fmt))
        except ValueError:
            continue
    return None


def _rough_distance_km(rows, speed_key=13):
    """Trapezoid-integrate speed to a distance, ignoring long gaps.

    Same rule as `metrics.Trip`: a gap over 5 s is a pause in the recording,
    not the car covering ground, so it contributes nothing.
    """
    dist = 0.0
    last_t = last_v = None
    for t, sid, v in rows:
        if sid != speed_key:
            continue
        if last_t is not None:
            dt = t - last_t
            if 0 < dt <= 5.0:
                dist += (v + last_v) / 2.0 * dt / 3600.0
        last_t, last_v = t, v
    return dist


def _summarise_brc(path):
    header, names, rows, _skipped = brc.parse(path)
    chans = sorted(set(sources.REPLAY_MAP[i] for (_t, i, _v) in rows
                       if i in sources.REPLAY_MAP))
    span = (rows[-1][0] - rows[0][0]) if rows else 0.0
    moving = sum(1 for (_t, i, v) in rows if i == 13 and v > 1)
    return {
        'kind': 'capture',
        'duration_s': span,
        'dist_km': _rough_distance_km(rows),
        # Car Scanner recordings arrive in bursts with long gaps, because iOS
        # suspends the backgrounded app — a 54-minute file can hold 7 minutes
        # of samples. Distance integrated from that is meaningless, so it is
        # computed but flagged, and the picker shows sample counts instead.
        'dist_reliable': False,
        'samples': len(rows),
        'moving_samples': moving,
        'channels': chans,
        'score': None,
        'profile': header.get('carprofile'),
    }


def _summarise_csv(path):
    """Summarise one of our own recordings, preferring its .json sidecar.

    The sidecar was written at the end of the session from the real metrics, so
    it is both cheaper and more accurate than re-deriving anything here.
    """
    meta = {}
    side = path.replace('.csv', '.json')
    if os.path.exists(side):
        try:
            with open(side) as fh:
                meta = json.load(fh) or {}
        except Exception:
            meta = {}
    derived = meta.get('derived') or {}
    score = meta.get('score') or {}
    chans = sorted((meta.get('channels') or {}).keys())
    out = {
        'kind': 'drive',
        'duration_s': derived.get('elapsed_s') or 0.0,
        'dist_km': derived.get('dist_km'),
        'dist_reliable': True,      # our own logging has no sampling gaps
        'samples': meta.get('rows'),
        'moving_samples': None,
        'channels': chans,
        'score': score.get('total'),
        'profile': None,
    }
    if not meta:
        # No sidecar, so the drive was cut short before the summary could be
        # written — laptop shut, ignition off, power pulled. The CSV itself
        # survives (it is flushed every second), so recover what we can from it
        # in one pass: how many samples, and how long it ran.
        out['partial'] = True
        try:
            rows, last = 0, None
            with open(path) as fh:
                next(fh, None)                    # header
                for line in fh:
                    if line.strip():
                        rows += 1
                        last = line
            out['samples'] = rows
            if last:
                # long format is  iso,t,key,value  — t is seconds from the start
                out['duration_s'] = float(last.split(',')[1])
        except Exception:
            pass
    return out


def _entry(path):
    st = os.stat(path)
    key = (st.st_size, st.st_mtime)
    hit = _cache.get(path)
    if hit and hit[0] == key[0] and hit[1] == key[1]:
        return hit[2]
    try:
        if path.lower().endswith('.brc'):
            info = _summarise_brc(path)
        else:
            info = _summarise_csv(path)
    except Exception as exc:
        # a corrupt file must not take the whole list down with it
        info = {'kind': 'unreadable', 'error': str(exc), 'duration_s': 0.0,
                'dist_km': None, 'dist_reliable': False, 'samples': 0,
                'moving_samples': None, 'channels': [], 'score': None,
                'profile': None}
    info['path'] = path
    info['name'] = os.path.basename(path)
    when = _time_from_name(path)
    info['recorded_at'] = when if when is not None else st.st_mtime
    info['when'] = _fmt_when(info['recorded_at'])
    info['mtime'] = st.st_mtime
    info['size'] = st.st_size
    info['summary'] = _summary_line(info)
    _cache[path] = (key[0], key[1], info)
    return info


def _summary_line(e):
    """One line describing the drive, honest about what the file can support.

    Our own drives can quote distance and a score. Captures cannot (see
    `dist_reliable`), so they quote what is actually solid: how many channels
    the car was reporting, and whether the file contains any driving at all.
    """
    if e['kind'] == 'unreadable':
        return 'unreadable'
    mins = (e.get('duration_s') or 0) / 60.0
    bits = []
    if e['kind'] == 'drive' and e.get('dist_km') is not None:
        bits.append('%.1f km' % e['dist_km'])
        if mins >= 1:
            bits.append('%.0f min' % mins)
        if e.get('score') is not None:
            bits.append('score %.0f' % e['score'])
    elif e['kind'] == 'drive':
        # cut short, so there is no distance or score to quote — say what the
        # file does hold rather than calling a 4000-sample drive "empty"
        n = e.get('samples') or 0
        bits.append(('%.1fk pts' % (n / 1000.0)) if n >= 1000 else '%d pts' % n)
        if mins >= 1:
            bits.append('%.0f min' % mins)
        bits.append('interrupted')
    else:
        bits.append('%d ch' % len(e.get('channels') or ()))
        n = e.get('samples') or 0
        # kept terse: the row is ~30 characters wide on a 240px round screen,
        # and anything longer wraps and pushes the list out of the circle
        bits.append(('%.1fk pts' % (n / 1000.0)) if n >= 1000
                    else '%d pts' % n)
        if not e.get('moving_samples'):
            bits.append('idle')
        if mins >= 1:
            bits.append('%.0f min' % mins)
    return ' · '.join(bits) or 'empty'


def scan(root):
    """Every replayable drive under `root`, newest first."""
    found = []
    for sub, pattern in (('logs', '.csv'), ('captures', '.brc')):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if name.lower().endswith(pattern) and not name.startswith('.'):
                found.append(_entry(os.path.join(d, name)))
    found.sort(key=lambda e: e['recorded_at'], reverse=True)
    return found


def resolve(root, wanted):
    """Match a requested path against the library.

    Compares basenames rather than trusting the incoming string, so a request
    can only ever select a file the library already listed — it cannot be
    talked into opening something elsewhere on disk.
    """
    if not wanted:
        return None
    target = os.path.basename(wanted)
    for entry in scan(root):
        if entry['name'] == target:
            return entry
    return None
