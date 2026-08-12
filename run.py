#!/usr/bin/env python3
"""MX-5 gauge simulator.

  Replay a capture at your desk (no car needed):
      .venv/bin/python run.py --replay "captures/2026-08-11 21-43-36.brc"

  Live from the car (laptop in the passenger seat, engine on):
      .venv/bin/python run.py --live

Then open http://127.0.0.1:8420
"""
import argparse
import asyncio
import glob
import json
import os
import sys
import webbrowser

from mx5gauge import recorder, server, sources, state

HERE = os.path.dirname(os.path.abspath(__file__))


LOG_DIR = os.path.join(HERE, 'logs')


def list_sessions():
    """Recorded drives, newest first, with whatever the summary json holds."""
    out = []
    for csv_path in sorted(glob.glob(os.path.join(LOG_DIR, '*.csv')), reverse=True):
        meta = {}
        jp = csv_path.replace('.csv', '.json')
        if os.path.exists(jp):
            try:
                with open(jp) as fh:
                    meta = json.load(fh)
            except Exception:
                meta = {}
        out.append((csv_path, meta))
    return out


def print_sessions():
    rows = list_sessions()
    if not rows:
        print('\n  No recorded sessions yet. Drive with --live and they land in logs/\n')
        return
    print('\n  Recorded sessions (newest first)\n')
    print('  %-34s %8s %8s %7s %6s' % ('file', 'dist', 'moving', 'score', 'rows'))
    print('  ' + '-' * 68)
    for path, meta in rows:
        d = meta.get('derived') or {}
        s = meta.get('score') or {}
        dist = '%.2f km' % d['dist_km'] if d.get('dist_km') is not None else '-'
        mov = '%.0f min' % ((d.get('moving_s') or 0) / 60.0) if d else '-'
        sc = '%.0f' % s['total'] if s.get('total') is not None else '-'
        print('  %-34s %8s %8s %7s %6s' % (
            os.path.basename(path), dist, mov, sc, meta.get('rows', '-')))
    print('\n  Replay one with:  run.py --replay "logs/<file>"')
    print('  Or the latest:    run.py --replay last\n')


def resolve_replay(arg):
    """Turn --replay's argument into a path. Accepts 'last' and 'auto'."""
    if arg == 'last':
        rows = list_sessions()
        if not rows:
            return None
        return rows[0][0]
    if arg in (None, 'auto'):
        return pick_default_capture()
    return arg


def pick_default_capture():
    """Pick the most interesting capture — the one with the most actual driving.

    Size is a poor proxy: the biggest file here is a 27-minute idle. Ranking by
    how many moving samples a capture holds surfaces a real drive instead.
    """
    from mx5gauge import brc
    files = sorted(glob.glob(os.path.join(HERE, 'captures', '*.brc')))
    if not files:
        return None
    best, best_score = None, -1
    for f in files:
        try:
            _h, _n, rows, _s = brc.parse(f)
        except Exception:
            continue
        moving = sum(1 for (_t, sid, v) in rows if sid == 13 and v > 1)
        if moving > best_score:
            best, best_score = f, moving
    return best or max(files, key=os.path.getsize)


async def main():
    ap = argparse.ArgumentParser(description='MX-5 OBD gauge simulator')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--live', action='store_true',
                      help='connect to the vLinker over BLE (must be in the car)')
    mode.add_argument('--replay', metavar='FILE', nargs='?', const='auto',
                      help='replay a recorded session or .brc capture. '
                           'Use "last" for the most recent drive.')
    mode.add_argument('--sessions', action='store_true',
                      help='list recorded drives and exit')
    ap.add_argument('--speed', type=float, default=4.0,
                    help='replay speed multiplier (default 4x)')
    ap.add_argument('--port', type=int, default=8420)
    ap.add_argument('--name', default='vlinker',
                    help='BLE name fragment to match (default: vlinker)')
    ap.add_argument('--address', default=None, help='BLE address, skips scanning')
    ap.add_argument('--no-browser', action='store_true')
    ap.add_argument('--scan', action='store_true',
                    help='list nearby BLE devices and exit (diagnostics)')
    ap.add_argument('--no-record', action='store_true',
                    help='do NOT write a CSV log of this session')
    ap.add_argument('--make', default=None,
                    help='override the car make shown on the display '
                         '(otherwise read from the VIN)')
    ap.add_argument('--model', default=None,
                    help='car model for the display, e.g. "MX-5". A VIN cannot '
                         'give a model name, so set it here to see it on screen.')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if args.sessions:
        print_sessions()
        return 0

    if args.scan:
        from bleak import BleakScanner
        print('\n  scanning 10s...\n')
        devs = await BleakScanner.discover(timeout=10.0)
        if not devs:
            print('  nothing found. Is Bluetooth on, and has Terminal been')
            print('  granted Bluetooth permission in System Settings > Privacy?')
        for d in sorted(devs, key=lambda x: (x.name or '~')):
            star = '  <-- looks like an OBD adapter' if any(
                k in (d.name or '').lower()
                for k in ('obd', 'elm', 'vlink', 'vgate')) else ''
            print('  %-28s %s%s' % (d.name or '(no name)', d.address, star))
        print()
        return 0

    g = state.Gauge()

    if args.live:
        src = sources.LiveSource(name_hint=args.name, address=args.address,
                                 verbose=args.verbose,
                                 make=args.make, model=args.model)
    else:
        path = resolve_replay(args.replay)
        if not path:
            print('Nothing to replay. Record a drive with --live, or put a '
                  '.brc capture in captures/.')
            return 2
        if not os.path.exists(path):
            print('No such capture: %s' % path)
            return 2
        src = sources.ReplaySource(path, speed=args.speed,
                                   make=args.make, model=args.model)

    g.source_kind = src.kind
    g.status = src.status

    # Record every reading. Live drives are always worth keeping; replays are
    # not (we'd just be copying a file we already have).
    rec = recorder.Recorder(os.path.join(HERE, 'logs'),
                            prefix='drive' if src.kind == 'live' else 'replay',
                            enabled=(not args.no_record) and src.kind == 'live')
    g.recorder = rec

    try:
        httpd = server.serve(g, port=args.port)
    except server.PortInUse as exc:
        print('\n  !! %s\n' % exc)
        return 2
    url = 'http://127.0.0.1:%d' % args.port
    print('\n  OBD gauge simulator')
    print('  mode   : %s' % src.kind)
    car = getattr(src, 'car', None)
    if car:
        bits = car['label']
        if car.get('year'):
            bits += ' · %s' % car['year']
        print('  car    : %s  (redline %d rpm)' % (bits, car['rpm_red']))
    if src.kind == 'replay':
        print('  file   : %s  (%gx speed)' % (os.path.basename(src.path), args.speed))
        print('  channels: %d reported in this capture' % len(src.supported_keys))
    else:
        print('  adapter: scanning for "%s" — engine on, laptop in the car' % args.name)
    if rec.enabled:
        print('  logging: %s' % os.path.relpath(rec.path, HERE))
    print('  open   : %s\n  (ctrl-c to stop)\n' % url)

    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass

    async def pump():
        while True:
            g.status = src.status
            await asyncio.sleep(0.3)

    try:
        await asyncio.gather(src.run(g.sample), pump())
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        httpd.shutdown()
        snap = g.snapshot()
        d, s = snap['derived'], snap['score']
        path = rec.close(summary={'derived': d, 'score': s,
                                  'supported_pids': snap.get('supported')})
        if path:
            print('\n  ── session saved ───────────────────────────')
            print('  %s' % os.path.relpath(path, HERE))
            print('  %d readings · %.2f km · %.0f min moving'
                  % (rec.rows, d['dist_km'], (d['moving_s'] or 0) / 60.0))
            if s['total'] is not None:
                print('  driving score %.0f (%s)' % (s['total'], s['coach']))
            print('  replay it with:  run.py --replay "%s"'
                  % os.path.relpath(path, HERE))
            print()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(asyncio.run(main()) or 0)
    except KeyboardInterrupt:
        print('\nstopped')
