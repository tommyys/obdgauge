// Does the carousel actually wrap? Section 6 says it must, in both directions.
#include "carousel.h"
#include "check.h"
using gauge_test::check;

int main() {
    const int n = 8;   // the eight views the firmware ships

    check("forward from the middle", gauge::ring_index(3, n, +1), 4);
    check("back from the middle",    gauge::ring_index(3, n, -1), 2);

    // The two the board appeared to stick on.
    check("ECONOMY (2) forward -> 3",   gauge::ring_index(2, n, +1), 3);
    check("ECONOMY (2) back -> 1",      gauge::ring_index(2, n, -1), 1);
    check("ELECTRICAL (7) forward wraps to 0", gauge::ring_index(7, n, +1), 0);
    check("ELECTRICAL (7) back -> 6",          gauge::ring_index(7, n, -1), 6);
    check("TACHO (0) back wraps to 7",         gauge::ring_index(0, n, -1), 7);

    // A full lap returns to where it started, from every starting view.
    bool lap_ok = true;
    for (int start = 0; start < n; ++start) {
        int at = start;
        for (int i = 0; i < n; ++i) at = gauge::ring_index(at, n, +1);
        if (at != start) lap_ok = false;
    }
    check("a full lap forward returns to the start, from any view", lap_ok, true);

    bool lap_back_ok = true;
    for (int start = 0; start < n; ++start) {
        int at = start;
        for (int i = 0; i < n; ++i) at = gauge::ring_index(at, n, -1);
        if (at != start) lap_back_ok = false;
    }
    check("a full lap backward too", lap_back_ok, true);

    check("degenerate ring of one stays put", gauge::ring_index(0, 1, +1), 0);
    check("no ring at all is safe",           gauge::ring_index(0, 0, +1), 0);

    // Shortest signed distance: last-to-first must be one step, not seven.
    check("distance 7 -> 0 is +1", gauge::ring_distance(7, 0, n), 1);
    check("distance 0 -> 7 is -1", gauge::ring_distance(0, 7, n), -1);
    check("distance 0 -> 4 is +4", gauge::ring_distance(0, 4, n), 4);
    return gauge_test::check_report();
}
