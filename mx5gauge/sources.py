"""Data sources that feed the gauge: live BLE, or replay from a .brc capture.

Both expose the same shape:  async run(on_sample)  where on_sample(key, value)
is called as readings arrive, plus a `.status` string for the UI.
"""
import asyncio
import bisect
import time

from . import brc, pids, vehicle

# ---------------------------------------------------------------------------
# Replay: play a Car Scanner capture back as if it were live
# ---------------------------------------------------------------------------

# Car Scanner sensor-id -> our state key. Standard ids match OBD PIDs.
REPLAY_MAP = {
    12: 'rpm', 13: 'speed', 5: 'coolant', 15: 'intake', 17: 'throttle',
    0: 'volts', 4: 'load', 16: 'maf', 123: 'fuel_rate', 14: 'timing',
    11: 'map', 67: 'baro', 90: 'ctrl_volt', 127: 'ref_torque',
    126: 'act_torque', 84: 'catalyst', 163: 'fuel_rail_temp', 902: 'power_kw',
}


class ReplaySource(object):
    """Replays a .brc file. Compresses long idle gaps so it stays watchable."""

    kind = 'replay'

    def __init__(self, path, speed=4.0, max_gap=2.0, loop=True,
                 make=None, model=None):
        self.path = path
        self.speed = speed
        self.max_gap = max_gap
        self.loop = loop
        self.status = 'loading'
        if path.lower().endswith('.csv'):
            # one of our own recordings — keys are already in our namespace
            from . import recorder
            self.header = {'carprofile': 'recorded'}
            self.rows = recorder.load_csv(path)
        else:
            header, names, rows, skipped = brc.parse(path)
            self.header = header
            self.rows = [(t, REPLAY_MAP[sid], v) for (t, sid, v) in rows
                         if sid in REPLAY_MAP]
        # what this car actually reported is exactly what the file contains —
        # no need to guess, and it lets views degrade the same way they will
        # on a live car that lacks a channel
        self.supported_keys = sorted(set(k for (_t, k, _v) in self.rows))
        base = vehicle.from_capture_header(self.header)
        self.car = vehicle.identify(make=make or base.get('make'),
                                    model=model or base.get('model'),
                                    year=base.get('year'), source='capture')
        self.status = 'replay: %s' % path.split('/')[-1]
        # Position and length along the *capture's own* timeline, which is what
        # the drive actually took — playing it back at 8x does not make the
        # drive eight times shorter, and the progress bar must not say it did.
        self.duration = self.rows[-1][0] if self.rows else 0.0
        self.pos = 0.0
        # Progress is measured in samples, not seconds. A Car Scanner capture
        # can span 53 minutes and hold 68 seconds of data (iOS suspends the
        # app, §2), so a bar drawn against the clock is 98% dead zone: drag
        # anywhere in it and you land in a hole and snap to the end. Against
        # the sample count every part of the bar reaches something real, and
        # for our own logs — which sample steadily — the two are the same bar.
        self.total = len(self.rows)
        self._times = [r[0] for r in self.rows]
        self._cursor = 0            # next row `run` will play
        self._seek = None           # a seek requested while the loop is awake
        # Parked at the end of the drive, showing its totals, waiting for a
        # scrub. This is what clicking a drive lands you in — distinct from a
        # replay that simply ran out, which loops back round.
        self.paused = False
        self._wake = asyncio.Event()

    # -- seeking -------------------------------------------------------------
    @property
    def index(self):
        """How many samples of this drive have been played."""
        return self._cursor

    def marks(self, count=129):
        """`count` evenly-spaced (by sample) timestamps along the drive.

        The UI needs to put a clock against a handle position before it has
        asked the server to move there, and it cannot do that without knowing
        how the sample axis maps onto the clock. Sending the whole timestamp
        list would be wasteful on a long log; this is the same curve at a
        resolution far finer than the bar is wide.
        """
        if not self.rows:
            return []
        last = self.total - 1
        return [self._times[round(k / (count - 1) * last)]
                for k in range(count)]

    def seek(self, t, gauge):
        """Seek to the last sample at or before `t` seconds. See `seek_index`."""
        i = bisect.bisect_right(self._times, max(0.0, min(float(t),
                                                          self.duration)))
        return self.seek_index(i, gauge)

    def seek_index(self, i, gauge):
        """Put `gauge` in the state it would hold after `i` samples.

        Those `i` rows are pushed through the gauge with their own logical
        timestamps and no sleeping at all, so the trip totals and the driving
        score are the genuine article rather than a second, parallel summary
        that could drift from what playback produces. The rows are already in
        memory, so even seeking to the end of a long drive is immediate.
        """
        i = max(0, min(int(i), self.total))
        gauge.reset()
        # reset() clears the metadata too, so put the identity and the channel
        # list back before any reading lands — otherwise the banner blanks and
        # every view greys out for as long as it takes to scrub
        gauge.sample('_car', self.car)
        gauge.sample('_supported_keys', self.supported_keys)
        for ts, key, val in self.rows[:i]:
            gauge.sample(key, val, ts)
        self._cursor = i
        # the clock reports where the data actually is, not where the handle
        # was dropped: landing in a recording hole means the last real sample
        self.pos = self._times[i - 1] if i else 0.0
        # Seeking to the very end means "show me this drive's totals", so park
        # there. Anywhere else means "play on from here".
        self.paused = self._cursor >= self.total
        # if the loop is mid-sleep it must abandon its place and pick this up
        self._seek = self.pos
        self._wake.set()
        return i

    async def run(self, on_sample):
        lap = 0.0
        on_sample('_car', self.car)
        on_sample('_supported_keys', self.supported_keys)
        while True:
            prev = None
            self._seek = None
            while self._cursor < len(self.rows):
                ts, key, val = self.rows[self._cursor]
                self._cursor += 1
                if prev is not None:
                    gap = min(ts - prev, self.max_gap)
                    if gap > 0:
                        await asyncio.sleep(gap / self.speed)
                # a scrub landed while we were asleep: drop the row we were
                # about to play and pick up from where the seek left the cursor
                if self._seek is not None:
                    self._seek = None
                    prev = None
                    continue
                prev = ts
                self.pos = ts
                # pass the capture's own timeline, not wall-clock: metrics must
                # see real-world seconds even when we play back at 10x
                on_sample(key, val, lap + ts)
            if self.paused:
                await self._park()
                continue
            if not self.loop:
                self.pos = self.duration
                self.status = 'replay finished'
                self.paused = True
                await self._park()
                continue
            lap += self.duration + 1.0
            self._cursor = 0
            self.pos = 0.0
            await asyncio.sleep(1.0)

    async def _park(self):
        """Hold the current frame until someone scrubs.

        The gauge keeps showing where the drive ended — which is the whole
        point of the summary — instead of spinning through the file again or
        blanking out.
        """
        self._wake.clear()
        while self.paused:
            await self._wake.wait()
            self._wake.clear()
        self._seek = None


# ---------------------------------------------------------------------------
# Live: BLE -> ELM327 -> PID polling
# ---------------------------------------------------------------------------

# Nordic UART Service, which most BLE ELM327 clones (incl. vLinker) expose.
NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
NUS_WRITE = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
NUS_NOTIFY = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'


class BleElm327(object):
    """Minimal ELM327 client over BLE (Nordic UART style)."""

    def __init__(self, client, write_uuid, notify_uuid, verbose=False):
        self._c = client
        self._w = write_uuid
        self._n = notify_uuid
        self._buf = ''
        self._evt = asyncio.Event()
        self.verbose = verbose

    def _on_notify(self, _sender, data):
        try:
            self._buf += data.decode('latin-1')
        except Exception:
            return
        if '>' in self._buf:
            self._evt.set()

    async def start(self):
        await self._c.start_notify(self._n, self._on_notify)

    async def cmd(self, text, timeout=5.0):
        """Send a command, wait for the '>' prompt, return the reply body."""
        self._buf = ''
        self._evt.clear()
        payload = (text + '\r').encode('ascii')
        # BLE writes are chunked; 20 bytes is the safe default MTU payload
        for i in range(0, len(payload), 20):
            await self._c.write_gatt_char(self._w, payload[i:i + 20], response=False)
        try:
            await asyncio.wait_for(self._evt.wait(), timeout)
        except asyncio.TimeoutError:
            return ''
        reply = self._buf.replace('>', '')
        # strip the echoed command if echo is still on
        lines = [ln.strip() for ln in reply.replace('\r', '\n').split('\n')]
        lines = [ln for ln in lines if ln and ln.upper() != text.upper()]
        out = ' '.join(lines)
        if self.verbose:
            print('   %-8s -> %s' % (text, out))
        return out

    async def init(self):
        """Standard ELM327 bring-up sequence."""
        for c, wait in [('ATZ', 2.0), ('ATE0', 1.0), ('ATL0', 1.0),
                        ('ATS0', 1.0), ('ATH0', 1.0), ('ATSP0', 1.0)]:
            await self.cmd(c, timeout=wait + 3)
            await asyncio.sleep(0.15)

    async def discover(self):
        """Query the supported-PID bitmasks. Returns a set of PID numbers."""
        supported = set()
        for base in (0x00, 0x20, 0x40, 0x60):
            reply = await self.cmd('%02X%02X' % (0x01, base))
            data = pids.parse_mode01(reply, base)
            if not data:
                break
            found = pids.parse_supported(data, base)
            supported |= found
            # bit 32 of each block indicates "next block supported"
            if (base + 0x20) not in found:
                break
        return supported

    async def read_vin(self):
        """Read the VIN via mode 09 PID 02. Returns '' if the car won't say.

        Wrapped in a broad try: mode 09 is optional, and a car that ignores it
        must not take the whole connection down with it — we simply carry on
        without an identity.
        """
        try:
            reply = await self.cmd('0902', timeout=4.0)
            data = pids.parse_mode09(reply, 0x02)
            if not data:
                return ''
            return ''.join(chr(c) for c in data)
        except Exception:
            return ''


class LiveSource(object):
    """Connects to the adapter over BLE and polls PIDs continuously."""

    kind = 'live'

    def __init__(self, name_hint='vlinker', address=None, verbose=False,
                 make=None, model=None):
        self.name_hint = (name_hint or '').lower()
        self.address = address
        self.verbose = verbose
        self.status = 'idle'
        self.supported = set()
        self.reconnects = 0
        self._known = None          # cached device, so we can skip the scan
        # user-supplied identity; overrides whatever the VIN suggests
        self.make_hint = make
        self.model_hint = model
        self.car = vehicle.identify(make=make, model=model, source='config')

    def _log(self, msg):
        if self.verbose:
            print('   [live] %s' % msg)

    async def _find(self):
        from bleak import BleakScanner
        # reconnecting to a device we already know is much faster (and more
        # reliable) than a fresh 8-second scan
        if self._known is not None:
            return self._known
        self.status = 'scanning for adapter...'
        devices = await BleakScanner.discover(timeout=8.0)
        cands = []
        for d in devices:
            nm = (d.name or '').lower()
            if self.address and str(d.address).upper() == self.address.upper():
                return d
            if self.name_hint and self.name_hint in nm:
                cands.append(d)
            elif any(k in nm for k in ('obd', 'elm', 'vlink', 'vgate')):
                cands.append(d)
        if not cands:
            names = ', '.join(sorted(set((d.name or '?') for d in devices))[:12])
            raise RuntimeError('no OBD adapter found. Visible: ' + (names or 'none'))
        return cands[0]

    @staticmethod
    def _pick_uart(client):
        """Choose the write/notify pair to talk ELM327 over.

        Prefer the Nordic UART Service explicitly. Picking merely the *first*
        write+notify characteristics found can land on an unrelated service,
        where commands vanish and every read times out — which looks exactly
        like a flaky adapter.
        """
        write = notify = None
        for svc in client.services:
            if str(svc.uuid).lower() == NUS_SERVICE:
                for ch in svc.characteristics:
                    u = str(ch.uuid).lower()
                    if u == NUS_WRITE:
                        write = ch.uuid
                    elif u == NUS_NOTIFY:
                        notify = ch.uuid
        if write and notify:
            return write, notify, 'Nordic UART'
        # fall back: any service that has BOTH a writable and a notify char
        for svc in client.services:
            w = n = None
            for ch in svc.characteristics:
                p = ch.properties
                if ('write' in p or 'write-without-response' in p) and w is None:
                    w = ch.uuid
                if 'notify' in p and n is None:
                    n = ch.uuid
            if w and n:
                return w, n, 'service %s' % svc.uuid
        return None, None, None

    async def run(self, on_sample):
        from bleak import BleakClient
        backoff = 1.0
        while True:
            dropped = asyncio.Event()
            try:
                dev = await self._find()
                label = getattr(dev, 'name', None) or str(getattr(dev, 'address', dev))
                self.status = 'connecting to %s' % label

                def _on_disconnect(_c):
                    # fires the moment CoreBluetooth drops the link, so the
                    # poll loop stops instantly instead of timing out per-command
                    self._log('link dropped by adapter/OS')
                    dropped.set()

                async with BleakClient(
                        dev, timeout=20.0,
                        disconnected_callback=_on_disconnect) as client:
                    self._known = dev          # cache for fast reconnects
                    w_uuid, n_uuid, how = self._pick_uart(client)
                    if not (w_uuid and n_uuid):
                        raise RuntimeError('no usable write/notify pair on this device')
                    self._log('using %s' % how)

                    elm = BleElm327(client, w_uuid, n_uuid, verbose=self.verbose)
                    await elm.start()
                    self.status = 'initialising ELM327'
                    await elm.init()
                    self.status = 'discovering PIDs'
                    self.supported = await elm.discover()
                    cycle = pids.build_poll_cycle(self.supported)
                    if not cycle:
                        cycle = [0x0C, 0x0D, 0x05, 0x11]     # fall back to basics

                    # identify the car once per connection: the VIN gives the
                    # make and model year, the PID set gives the channels
                    self.status = 'identifying car'
                    vin = await elm.read_vin()
                    self.car = vehicle.identify(
                        vin=vin, make=self.make_hint, model=self.model_hint,
                        source='vin' if vehicle.valid_vin(
                            vehicle.clean_vin(vin)) else 'config')
                    self._log('car: %s (vin %s)'
                              % (self.car['label'], self.car['vin'] or 'n/a'))
                    on_sample('_car', self.car)
                    # ATRV is an adapter command rather than a PID, so battery
                    # voltage is always available even though no PID advertises it
                    on_sample('_supported_keys',
                              sorted(pids.keys_for(self.supported) | {'volts'}))

                    suffix = '' if not self.reconnects else ' · %d reconnects' % self.reconnects
                    self.status = 'live (%d PIDs)%s' % (len(self.supported), suffix)
                    on_sample('_supported', len(self.supported))
                    backoff = 1.0                            # a good connect resets backoff

                    i = 0
                    last_v = 0.0
                    misses = 0
                    while client.is_connected and not dropped.is_set():
                        pid = cycle[i % len(cycle)]
                        i += 1
                        reply = await elm.cmd('01%02X' % pid, timeout=1.5)
                        got = pids.decode(pid, pids.parse_mode01(reply, pid))
                        if got:
                            on_sample(got[0], got[1], time.time())
                            misses = 0
                        else:
                            # empty reply = the adapter stopped answering even
                            # though BLE still claims connected. Force a reset.
                            misses += 1
                            if misses >= 12:
                                raise RuntimeError('adapter stopped responding '
                                                   '(12 empty replies) — resetting link')
                        now = time.time()
                        if now - last_v > 5.0:
                            last_v = now
                            v = pids.parse_voltage(await elm.cmd('ATRV', timeout=1.5))
                            if v:
                                on_sample('volts', v, now)
                        await asyncio.sleep(0.01)

                self.reconnects += 1
                self.status = 'reconnecting (drop #%d)' % self.reconnects
            except Exception as exc:
                self.status = 'error: %s' % exc
                self._log(exc)
                # if we cannot reach the cached device, rescan next time round
                self._known = None
            await asyncio.sleep(backoff)
            backoff = min(backoff * 1.6, 10.0)
