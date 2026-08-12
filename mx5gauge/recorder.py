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
        self._fh = None
        self._w = None
        self._t0 = None
        self._last_flush = 0.0
        self._lock = threading.Lock()
        self._channels = {}          # key -> count, for the summary
        if not enabled:
            return
        os.makedirs(directory, exist_ok=True)
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.path = os.path.join(directory, '%s-%s.csv' % (prefix, stamp))
        self._fh = open(self.path, 'w', newline='')
        self._w = csv.writer(self._fh)
        self._w.writerow(['iso', 't', 'key', 'value'])

    def write(self, key, value, t=None):
        if not self.enabled or self._w is None:
            return
        if key.startswith('_'):
            return
        now = time.time()
        if t is None:
            t = now
        with self._lock:
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
        """Flush, close, and drop a .json summary beside the CSV."""
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
