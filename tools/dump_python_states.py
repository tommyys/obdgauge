"""Dump per-sample state from the Python implementation, as the reference the
C++ port is checked against.

Both sides consume the same capture and must agree; any divergence is a port
bug, located to the exact sample.

Usage:
    .venv/bin/python tools/dump_python_states.py logs/<drive>.csv > ref.txt
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import state  # noqa: E402

FIELDS = ('rpm', 'speed', 'coolant', 'throttle', 'fuel_rate', 'power_kw',
          'dist_km', 'fuel_l', 'harsh', 'smooth', 'econ', 'calm', 'total',
          'rejected', 'peak_rpm')


def fmt(v):
    if v is None:
        return 'None'
    return '%.9g' % v


def main(path):
    g = state.Gauge()
    out = sys.stdout
    out.write(','.join(FIELDS) + '\n')
    with open(path) as fh:
        for row in csv.DictReader(fh):
            try:
                value = float(row['value'])
            except (TypeError, ValueError):
                continue
            g.sample(row['key'], value, float(row['t']))
            v = g.values
            out.write(','.join(fmt(x) for x in (
                v.get('rpm'), v.get('speed'), v.get('coolant'),
                v.get('throttle'), v.get('fuel_rate'), v.get('power_kw'),
                g.trip.dist_km, g.trip.fuel_l, g.score.harsh,
                g.score.smooth, g.score.econ, g.score.calm, g.score.total,
                g.rejected, g.peak_rpm)) + '\n')


if __name__ == '__main__':
    main(sys.argv[1])
