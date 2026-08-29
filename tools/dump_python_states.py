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
                ('thr_s', sc.thr_s),
                ('thr_bad', sc.thr_bad),
                ('brake_s', sc.brake_s),
                ('brake_bad', sc.brake_bad),
                ('corner_s', sc.corner_s),
                ('corner_bad', sc.corner_bad),
                ('care_s', sc.care_s),
                ('care_bad', sc.care_bad),
                ('rev_s', sc.rev_s),
                ('intensity', sc.intensity),
                ('pole', sc.pole),
                ('nice_s', sc.nice_s),
                ('spirited_s', sc.spirited_s),
                ('n_events', len(sc.events)),
                ('g_ready', sc.g.ready),
                ('g_lat', sc.g.lat),
                ('g_lon', sc.g.lon),
                ('g_peak_lat', sc.g.peak_lat),
                ('g_peak_brake', sc.g.peak_lon_brake),
                ('throttle', sc.throttle),
                ('braking', sc.braking),
                ('cornering', sc.cornering),
                ('care', sc.care),
                ('total', sc.total),
                ('coach', sc.coach),
                ('rejected', g.rejected),
                ('peak_rpm', g.peak_rpm),
                ('peak_kw', g.peak_kw),
                ('peak_speed', g.peaks.get('speed')),
                ('peak_coolant', g.peaks.get('coolant')),
                ('peak_intake', g.peaks.get('intake')),
                ('peak_catalyst', g.peaks.get('catalyst')),
            ))
            out.write(channels + '\t' + derived + '\n')


if __name__ == '__main__':
    main(sys.argv[1])
