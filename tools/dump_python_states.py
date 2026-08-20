"""Dump per-sample state from the Python implementation, as the reference the
C++ port is checked against.

Emits EVERY channel currently held in the state, plus every derived trip and
score value — not a hand-picked subset. A field the C++ has and the Python
does not (or vice versa) is itself a divergence, so the comparison cannot
quietly miss a channel nobody thought to list.

Usage:
    .venv/bin/python tools/dump_python_states.py logs/<drive>.csv > ref.txt
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mx5gauge import state  # noqa: E402


def fmt(v):
    if v is None:
        return 'None'
    if isinstance(v, str):
        return v
    return '%.17g' % v


def main(path):
    g = state.Gauge()
    out = sys.stdout
    with open(path) as fh:
        for row in csv.DictReader(fh):
            try:
                value = float(row['value'])
            except (TypeError, ValueError):
                continue
            g.sample(row['key'], value, float(row['t']))

            channels = ';'.join('%s=%s' % (k, fmt(v))
                                for k, v in sorted(g.values.items())
                                if not k.startswith('_'))
            sc, tr = g.score, g.trip
            derived = ';'.join('%s=%s' % (k, fmt(v)) for k, v in (
                ('dist_km', tr.dist_km),
                ('fuel_l', tr.fuel_l),
                ('moving_s', tr.moving_s),
                ('elapsed_s', tr.elapsed_s),
                ('cost_rm', tr.cost_rm),
                ('econ_l_per_100', tr.econ_l_per_100),
                ('econ_km_per_l', tr.econ_km_per_l),
                ('avg_speed_kph', tr.avg_speed_kph),
                ('thr_travel', sc.thr_travel),
                ('thr_seconds', sc.thr_seconds),
                ('eco_s', sc.eco_s),
                ('rev_s', sc.rev_s),
                ('econ_sum', sc.econ_sum),
                ('econ_s', sc.econ_s),
                ('harsh', sc.harsh),
                ('n_events', len(sc.events)),
                ('sum_events', sum(e[2] for e in sc.events)),
                ('smooth', sc.smooth),
                ('econ', sc.econ),
                ('calm', sc.calm),
                ('total', sc.total),
                ('coach', sc.coach),
                ('rejected', g.rejected),
                ('peak_rpm', g.peak_rpm),
                ('peak_kw', g.peak_kw),
            ))
            out.write(channels + '\t' + derived + '\n')


if __name__ == '__main__':
    main(sys.argv[1])
