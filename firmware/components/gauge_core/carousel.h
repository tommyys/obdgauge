// Index arithmetic for the infinite carousel (SPEC.md section 6).
//
// Pure and header-only so it is host-testable: "the carousel does not wrap"
// is a claim that should be settled by a test on the Mac, not by swiping a
// board. gauge_ui uses these; nothing here knows about LVGL.
#pragma once

namespace gauge {

// The view `step` places away from `cur` in a ring of `n`, wrapping in both
// directions. step may be any magnitude, positive or negative.
inline int ring_index(int cur, int n, int step) {
    if (n <= 0) return 0;
    int i = (cur + step) % n;
    if (i < 0) i += n;
    return i;
}

// Shortest signed distance from `from` to `to` in a ring of `n`. Section 6:
// each view is placed at its shortest signed distance from the current one, so
// the last-to-first step is one move rather than a rewind of the whole strip.
inline int ring_distance(int from, int to, int n) {
    if (n <= 0) return 0;
    int d = (to - from) % n;
    if (d < 0) d += n;
    if (d > n / 2) d -= n;
    return d;
}

}  // namespace gauge
