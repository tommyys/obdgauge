"""Records every reading to disk so a drive is never lost.

Writes a long-format CSV — one row per sample — which handles sparse/irregular
channels far better than a wide table, and can be replayed straight back
through ReplaySource.

  iso,t,key,value
  2026-08-12T20:15:03.412,0.000,rpm,764.0

The file is flushed on a short interval, so pulling the plug (or the adapter
dropping) costs at most a second of data rather than the whole session.
"""
import csv
import datetime
import json
import os
import threading
import time

FLUSH_SECONDS = 1.0

# How often the .json summary is rewritten while the drive is under way. The
# drive in the car ends when its power is cut, so this is the most a finished
# drive's summary can be out of date.
SIDECAR_SECONDS = 10.0


def _free_path(path):
    """`path`, or the first '-2', '-3'... variant of it that is not taken.

    The name carries a timestamp only to the second, which was unique enough
    when a drive lasted as long as the process. Ignition rotation can open a
    second recorder in the same second as the one it just closed, and without
    this the new drive would open on top of the finished one and erase it.
    """
    if not os.path.exists(path):
        return path
    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists('%s-%d%s' % (stem, n, ext)):
        n += 1
    return '%s-%d%s' % (stem, n, ext)


class Recorder(object):
    def __init__(self, directory, prefix='drive', enabled=True):
        self.enabled = enabled
        self.path = None
        self.rows = 0
        self._dir = directory
        self._fh = None
        self._w = None
        self._t0 = None
        self._last_flush = 0.0
        self._last_side = 0.0
        self._lock = threading.Lock()
        self._channels = {}          # key -> count, for the summary
        # Set by the caller to a callable returning the summary dict. Without
        # one the sidecar is only written at close(), which in the car never
        # happens — see _write_sidecar.
        self.summary_fn = None
        if not enabled:
            return
        # The path is decided now so it can be printed at startup, but the file
        # is not created until there is something to put in it. A live session
        # that never connects — no adapter, BLE denied, phone still holding the
        # link — would otherwise leave a header-only CSV behind on every
        # attempt, and the Drives view would list each one as an empty drive.
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.path = _free_path(
            os.path.join(directory, '%s-%s.csv' % (prefix, stamp)))

    def _open(self):
        os.makedirs(self._dir, exist_ok=True)
        self._fh = open(self.path, 'w', newline='')
        self._w = csv.writer(self._fh)
        self._w.writerow(['iso', 't', 'key', 'value'])

    def write(self, key, value, t=None):
        if not self.enabled:
            return
        if key.startswith('_'):
            return
        now = time.time()
        if t is None:
            t = now
        with self._lock:
            if self._w is None:
                self._open()          # first real reading: commit to a file
                # start the sidecar clock here, not at construction: a summary
                # of a drive that has not begun says nothing worth saying
                self._last_side = now
            if self._t0 is None:
                self._t0 = t
            iso = datetime.datetime.fromtimestamp(now).isoformat(timespec='milliseconds')
            try:
                self._w.writerow([iso, '%.3f' % (t - self._t0), key, value])
            except ValueError:      # file closed mid-write during shutdown
                return
            self.rows += 1
            self._channels[key] = self._channels.get(key, 0) + 1
            if now - self._last_flush > FLUSH_SECONDS:
                self._last_flush = now
                self._fh.flush()
            due = (self.summary_fn is not None
                   and now - self._last_side > SIDECAR_SECONDS)
            if due:
                self._last_side = now

        # Outside the lock on purpose: `summary_fn` reaches into the gauge and
        # takes its lock, and the two must never be acquired in both orders.
        if due:
            self._write_sidecar(self.summary_fn())

    def _write_sidecar(self, summary):
        """Drop the .json summary beside the CSV, overwriting the last one.

        Written as the drive goes rather than only at the end, because the end
        is a power cut: the car's supply is ignition-switched, so `close()` is
        never reached in the car. Without this every drive would land with no
        summary and the picker could only report a row count — see
        `library._summarise_csv`, which falls back to exactly that.
        """
        if summary is None or self.path is None:
            return
        meta = dict(summary)
        meta['rows'] = self.rows
        meta['channels'] = dict(self._channels)
        meta['csv'] = os.path.basename(self.path)
        try:
            with open(self.path.replace('.csv', '.json'), 'w') as fh:
                json.dump(meta, fh, indent=2, default=str)
        except Exception:
            pass

    def close(self, summary=None):
        """Flush, close, and drop a .json summary beside the CSV.

        Returns None when nothing was ever recorded, so the caller knows there
        is no session to announce and no file was left on disk.
        """
        if not self.enabled or self._fh is None:
            return None
        with self._lock:
            try:
                self._fh.flush()
                self._fh.close()
            except Exception:
                pass
            self._w = None
        self._write_sidecar(summary)
        return self.path


def load_csv(path):
    """Read a recorded CSV back into [(t, key, value)] for replay."""
    out = []
    with open(path, newline='') as fh:
        for row in csv.DictReader(fh):
            try:
                out.append((float(row['t']), row['key'], float(row['value'])))
            except (TypeError, ValueError, KeyError):
                continue
    out.sort(key=lambda r: r[0])
    return out
