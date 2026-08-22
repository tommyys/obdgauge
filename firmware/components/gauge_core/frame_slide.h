#pragma once
// The pixel arithmetic of a carousel slide, kept pure so it can be tested on
// the host. An off-by-one here is a visual smear on a 466x466 panel and
// nothing else -- no crash, no log -- which is the worst kind of bug to chase
// from the board. See test_frame_slide.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gauge {

// Composes one frame of a slide into `dst`.
//
//   from      the outgoing view, `w` by `h` pixels, row-major, no padding
//   to        the incoming view, same shape
//   off       how far the outgoing view has travelled, 0..w. off == 0 is
//             entirely `from`, off == w is entirely `to`.
//   dir       +1: the incoming view enters from the right, so `from` exits left
//             -1: the incoming view enters from the left, so `from` exits right
//   static_top/static_bot
//             a band of rows [static_top, static_bot) taken whole from `to`
//             rather than shifted: the page indicator and the make/model banner
//             belong to the carousel frame, not to a view, and must not travel
//             with the content. Pass an empty range to shift everything.
inline void slide_compose(uint16_t* dst, const uint16_t* from, const uint16_t* to,
                          int w, int h, int off, int dir,
                          int static_top, int static_bot) {
    if (w <= 0 || h <= 0) return;
    if (off < 0) off = 0;
    if (off > w) off = w;
    const size_t px = sizeof(uint16_t);
    for (int y = 0; y < h; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
        uint16_t* d = dst + row;
        const uint16_t* f = from + row;
        const uint16_t* t = to + row;
        if (y >= static_top && y < static_bot) {
            std::memcpy(d, t, static_cast<size_t>(w) * px);
            continue;
        }
        if (dir >= 0) {
            const int keep = w - off;
            if (keep > 0) std::memcpy(d, f + off, static_cast<size_t>(keep) * px);
            if (off > 0)  std::memcpy(d + keep, t, static_cast<size_t>(off) * px);
        } else {
            if (off > 0)     std::memcpy(d, t + (w - off), static_cast<size_t>(off) * px);
            if (off < w)     std::memcpy(d + off, f, static_cast<size_t>(w - off) * px);
        }
    }
}

}  // namespace gauge
