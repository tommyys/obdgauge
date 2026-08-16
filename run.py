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
import os
import sys
import webbrowser

from mx5gauge import library, recorder, server, sources, state

HERE = os.path.dirname(os.path.abspath(__file__))


def print_sessions():
    """List everything replayable — the same library the on-screen picker shows.

    Reads through `library` rather than only globbing logs/, so the terminal and
    the Drives view can never disagree about what exists.
    """
    rows = library.scan(HERE)
    if not rows:
        print('\n  Nothing to replay yet. Drive with --live (drives land in '
              'logs/), or put a .brc capture in captures/\n')
        return
    print('\n  Replayable drives (newest first)\n')
    print('  %-26s %-8s %-34s' % ('when', 'kind', 'summary'))
    print('  ' + '-' * 70)
    for e in rows:
        print('  %-26s %-8s %-34s' % (e['when'], e['kind'], e['summary']))
        print('  %-26s %s' % ('', e['name']))
    print('\n  Replay one with:  run.py --replay "<path>"')
    print('  Or the latest:    run.py --replay last')
    print('  Or pick it on screen: the Drives view in the carousel\n')


def resolve_replay(arg):
    """Turn --replay's argument into a path. Accepts 'last' and 'auto'."""
    if arg == 'last':
        # newest replayable thing, drive or capture — same library the Drives
        # view lists, so "last" means the same in both places
        rows = library.scan(HERE)
        return rows[0]['path'] if rows else None
    if arg in (None, 'auto'):
        return pick_default_capture()
    return arg


def pick_default_capture():
    """Pick the most interesting thing to replay when none was named.

    Prefers the capture holding the most actual driving. Size is a poor proxy:
    the biggest capture in this repo is a 27-minute idle, so ranking by how
    many moving samples a file holds surfaces a real drive instead.

    With no captures on disk — which is the normal state once you have your own
    recordings and have cleared the borrowed ones out — it falls back to the
    newest drive in the library. Without that fallback the double-click REPLAY
    launcher, which names no file, would simply refuse to start.
    """
    from mx5gauge import brc
    files = sorted(glob.glob(os.path.join(HERE, 'captures', '*.brc')))
    if not files:
        rows = library.scan(HERE)
        return rows[0]['path'] if rows else None
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


# A rotated drive shorter than this is not a drive — it is a stall, a restart,
# or the adapter flapping. Deleted rather than left to clutter the picker.
MIN_DRIVE_S = 60.0


def finish_drive(g, rec, here, quiet=False):
    """Close the recording with its summary, and announce it.

    Returns the saved path, or None when nothing was recorded or the drive was
    too short to keep. Used both at shutdown and on every ignition rotation, so
    a drive ended by switching the car off is saved exactly like one ended by
    closing the gauge.
    """
    if rec is None:
        return None
    snap = g.snapshot()
    d, s = snap['derived'], snap['score']
    path = rec.close(summary={'derived': d, 'score': s,
                              'supported_pids': snap.get('supported')})
    if not path:
        return None

    if (d['elapsed_s'] or 0) < MIN_DRIVE_S:
        # Scoped to the file this recorder just wrote, and only ever the pair
        # it created — nothing else on disk is touched.
        for victim in (path, path.replace('.csv', '.json')):
            try:
                os.remove(victim)
            except OSError:
                pass
        if not quiet:
            print('  (discarded %.0fs fragment)' % (d['elapsed_s'] or 0))
        return None

    if not quiet:
        print('\n  ── session saved ───────────────────────────')
        print('  %s' % os.path.relpath(path, here))
        print('  %d readings · %.2f km · %.0f min moving'
              % (rec.rows, d['dist_km'], (d['moving_s'] or 0) / 60.0))
        if s['total'] is not None:
            print('  driving score %.0f (%s)' % (s['total'], s['coach']))
        print('  replay it with:  run.py --replay "%s"'
              % os.path.relpath(path, here))
        print()
    return path


def on_ignition(g, here):
    """Build the callback that splits the recording on ignition events.

    Only the restart rotates. An ignition-off leaves the current file open and
    idling: the car may be off for ten seconds at a barrier, and there is no
    cost to waiting for the restart before deciding a drive has ended.
    """
    def handle(event):
        if event != 'on':
            print('  ignition off — drive will end if the engine restarts')
            return
        old = g.recorder
        finish_drive(g, old, here)
        g.recorder = recorder.Recorder(
            os.path.join(here, 'logs'),
            prefix='drive', enabled=(old is None or old.enabled))
        g.reset()
        print('  ignition on — new drive: %s'
              % os.path.basename(g.recorder.path or '(not recording)'))
    return handle


class Player(object):
    """Runs one source, and can swap it for another drive while running.

    The HTTP server lives on its own thread, so `select` may be called from
    anywhere: it only records the request and pokes the event loop, leaving all
    the source handling on the loop where it belongs.
    """

    def __init__(self, gauge, src, root, speed=4.0, make=None, model=None):
        self.g = gauge
        self.src = src
        self.root = root
        self.speed = speed
        self.make = make
        self.model = model
        self.loop = None
        self.request = None
        self.at_end = False
        self.event = asyncio.Event()

    def select(self, entry, seek_to_end=False):
        """Ask for `entry` to start playing. Thread-safe, returns immediately."""
        self.request = entry
        self.at_end = seek_to_end
        if self.loop is not None:
            self.loop.call_soon_threadsafe(self.event.set)

    def seek(self, kind, value):
        """Scrub the drive being replayed. Thread-safe.

        `kind` is 'frac' (a position along the samples, what the bar speaks) or
        't' (seconds). The work happens on the event loop rather than the HTTP
        thread: a seek feeds thousands of samples through the gauge, and doing
        that underneath the running source would interleave two writers into
        one set of totals.
        """
        if self.loop is not None:
            self.loop.call_soon_threadsafe(self._seek_now, kind, value)

    def _seek_now(self, kind, value):
        src = self.src
        if getattr(src, 'kind', None) != 'replay':
            return
        if kind == 'frac':
            src.seek_index(round(max(0.0, min(1.0, value)) * src.total), self.g)
        else:
            src.seek(value, self.g)

    def _load(self, entry, at_end=False):
        # a different drive must not inherit the last one's trip or channels
        self.g.reset()
        self.src = sources.ReplaySource(entry['path'], speed=self.speed,
                                        make=self.make, model=self.model)
        self.g.source_kind = self.src.kind
        self.g.status = self.src.status
        self.g.current_file = entry['name']
        self.g.replay = self.src
        if at_end:
            # opening a drive shows what it came to: its finished totals, with
            # the bar full. Scrubbing back from there is what replays it.
            self.src.seek(self.src.duration, self.g)
        print('  loaded : %s' % entry['name'])

    async def run(self):
        self.loop = asyncio.get_event_loop()
        while True:
            self.event.clear()
            playing = asyncio.create_task(self.src.run(self.g.sample))
            switch = asyncio.create_task(self.event.wait())
            done, _pending = await asyncio.wait(
                {playing, switch}, return_when=asyncio.FIRST_COMPLETED)

            if switch in done and self.request is not None:
                playing.cancel()
                try:
                    await playing
                except asyncio.CancelledError:
                    pass
                except Exception as exc:                  # noqa: BLE001
                    print('  (previous source stopped: %s)' % exc)
                entry, self.request = self.request, None
                self._load(entry, at_end=self.at_end)
                continue

            switch.cancel()
            if playing in done:
                exc = playing.exception()
                if self.src.kind == 'live':
                    # Nothing can revive a dead live source: switching drives is
                    # refused in live mode, so waiting here would leave a frozen
                    # gauge and no explanation. Fail loudly instead.
                    if exc:
                        raise exc
                    raise RuntimeError('the live source stopped unexpectedly')
                # a replay ran out; hold the last frame rather than spinning, so
                # the UI keeps showing where the drive ended
                if exc:
                    self.g.status = 'source error: %s' % exc
                    print('  !! source error: %s' % exc)
                await self.event.wait()


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
    ap.add_argument('--sweep', nargs='?', const='1000-7000', default=None,
                    metavar='LO-HI',
                    help='desk preview: sweep rpm continuously (default '
                         '1000-7000) instead of using the recorded revs, so '
                         'the visuals can be judged on a capture that idles. '
                         'Replay only — ignored in live mode.')
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

    # a synthetic rpm sweep is a desk aid; in the car the real revs are the
    # whole point, so refuse rather than quietly showing made-up data
    if args.sweep and src.kind == 'live':
        print('\n  !! --sweep is a desk preview aid and would replace the real '
              'rpm.\n     Ignoring it for this live session.')
    elif args.sweep:
        g.preview_sweep = args.sweep

    # Record every reading. Live drives are always worth keeping; replays are
    # not (we'd just be copying a file we already have).
    rec = recorder.Recorder(os.path.join(HERE, 'logs'),
                            prefix='drive' if src.kind == 'live' else 'replay',
                            enabled=(not args.no_record) and src.kind == 'live')
    g.recorder = rec
    if src.kind == 'live':
        g.on_ignition = on_ignition(g, HERE)

    player = Player(g, src, HERE, speed=args.speed,
                    make=args.make, model=args.model)
    if src.kind == 'replay':
        g.current_file = os.path.basename(src.path)
        g.replay = src

    try:
        httpd = server.serve(g, port=args.port, root=HERE,
                             on_select=player.select, on_seek=player.seek)
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
            # read through the player: the source it holds changes when the
            # on-screen picker loads a different drive
            g.status = player.src.status
            await asyncio.sleep(0.3)

    try:
        await asyncio.gather(player.run(), pump())
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        httpd.shutdown()
        finish_drive(g, g.recorder, HERE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(asyncio.run(main()) or 0)
    except KeyboardInterrupt:
        print('\nstopped')
