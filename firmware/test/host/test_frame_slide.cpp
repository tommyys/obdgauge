// Does a slide frame actually contain the right pixels? The failure mode this
// guards is silent: a wrong offset or a mirrored direction smears the panel and
// logs nothing, so it can only be judged by eye from the board -- exactly the
// kind of guessing that cost seven builds on the swipe bug.
//
// Each view is filled with a distinct value per column, so a pixel's value says
// which view it came from and which column of it.
#include <vector>
#include "check.h"
#include "frame_slide.h"
using gauge_test::check;

namespace {

constexpr int W = 12;
constexpr int H = 6;

uint16_t from_px(int x) { return static_cast<uint16_t>(0x1000 + x); }
uint16_t to_px(int x)   { return static_cast<uint16_t>(0x2000 + x); }

std::vector<uint16_t> run(int off, int dir, int stop = 0, int sbot = 0) {
    std::vector<uint16_t> f(W * H), t(W * H), d(W * H, 0xFFFF);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) { f[y * W + x] = from_px(x); t[y * W + x] = to_px(x); }
    gauge::slide_compose(d.data(), f.data(), t.data(), W, H, off, dir, stop, sbot);
    return d;
}

// The composed row as a readable string: "f0 f1 t0 ..." for from/to column n.
std::string row(const std::vector<uint16_t>& d, int y) {
    std::string s;
    for (int x = 0; x < W; ++x) {
        uint16_t v = d[y * W + x];
        s += (x ? " " : "");
        s += (v >> 12) == 1 ? "f" : ((v >> 12) == 2 ? "t" : "?");
        s += std::to_string(v & 0xFFF);
    }
    return s;
}

}  // namespace

int main() {
    // off == 0 is entirely the outgoing view: a slide must start where the eye
    // already is, or the first frame is a visible jump.
    check("dir +1, off 0 is all 'from'", row(run(0, +1), 0),
          std::string("f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11"));
    check("dir -1, off 0 is all 'from'", row(run(0, -1), 0),
          std::string("f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11"));

    // off == w is entirely the incoming view, and it must be aligned at x=0 --
    // not shifted by one, which would show as a permanent 1px offset after the
    // slide hands back to LVGL.
    check("dir +1, off w is all 'to' aligned", row(run(W, +1), 0),
          std::string("t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11"));
    check("dir -1, off w is all 'to' aligned", row(run(W, -1), 0),
          std::string("t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11"));

    // Mid-slide, +1: the old view has moved left by `off`, so its column `off`
    // sits at x=0, and the new view's leading columns fill the gap on the right.
    check("dir +1, off 5", row(run(5, +1), 0),
          std::string("f5 f6 f7 f8 f9 f10 f11 t0 t1 t2 t3 t4"));

    // Mid-slide, -1 is the mirror: the new view enters from the LEFT, showing
    // its trailing columns, and the old view is pushed right.
    check("dir -1, off 5", row(run(5, -1), 0),
          std::string("t7 t8 t9 t10 t11 f0 f1 f2 f3 f4 f5 f6"));

    // Both halves must be present at every offset: no row may keep a byte of
    // the destination buffer's previous contents.
    bool all_written = true;
    for (int dir = -1; dir <= 1; dir += 2)
        for (int off = 0; off <= W; ++off) {
            auto d = run(off, dir);
            for (auto v : d) if (v == 0xFFFF) all_written = false;
        }
    check("every pixel written at every offset", all_written, true);

    // The static band is the page indicator and banner. It must come from the
    // destination view, whole and unshifted, at every offset and in either
    // direction -- an indicator that slides away tells you nothing.
    bool band_ok = true;
    for (int dir = -1; dir <= 1; dir += 2)
        for (int off = 0; off <= W; ++off) {
            auto d = run(off, dir, 2, 4);
            for (int y = 2; y < 4; ++y)
                for (int x = 0; x < W; ++x)
                    if (d[y * W + x] != to_px(x)) band_ok = false;
            // and the rows outside it must still be shifted
            if (row(d, 1) != row(run(off, dir), 1)) band_ok = false;
        }
    check("static band is 'to', unshifted, always", band_ok, true);

    // Out-of-range offsets are clamped rather than read out of bounds. The
    // caller derives `off` from a wall clock, so a late frame can overshoot.
    check("off > w clamps to all 'to'", row(run(W + 40, +1), 0),
          std::string("t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 t10 t11"));
    check("off < 0 clamps to all 'from'", row(run(-7, +1), 0),
          std::string("f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11"));

    return gauge_test::check_report();
}
