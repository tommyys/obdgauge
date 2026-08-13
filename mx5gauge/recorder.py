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
        self._lock = threading.Lock()
        self._channels = {}          # key -> count, for the summary
        if not enabled:
            return
        # The path is decided now so it can be printed at startup, but the file
        # is not created until there is something to put in it. A live session
        # that never connects — no adapter, BLE denied, phone still holding the
        # link — would otherwise leave a header-only CSV behind on every
        # attempt, and the Drives view would list each one as an empty drive.
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.path = os.path.join(directory, '%s-%s.csv' % (prefix, stamp))

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
        if summary is not None:
            meta = dict(summary)
            meta['rows'] = self.rows
            meta['channels'] = self._channels
            meta['csv'] = os.path.basename(self.path)
            try:
                with open(self.path.replace('.csv', '.json'), 'w') as fh:
                    json.dump(meta, fh, indent=2, default=str)
            except Exception:
                pass
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
