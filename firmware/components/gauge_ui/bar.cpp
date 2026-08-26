#include "bar.h"

#include <cmath>

namespace gauge_ui {
namespace {

// The simulator's bar at 466 px: 62cqw wide, 4.6cqw tall, a 1.5cqw marker
// standing 8.4cqw proud of it.
constexpr int kBarW    = 289;
constexpr int kBarH    = 21;
constexpr int kMarkW   = 7;
constexpr int kMarkH   = 39;

// The scale the marker slides on, copied from the simulator: 11 V is a flat
// battery and 15.5 V is a charging system working hard, so the useful range
// sits comfortably inside the ends rather than pinning at them.
constexpr double kVLo = 11.0, kVHi = 15.5;

// The simulator's five gradient stops, as (position 0-1, colour). Red at the
// bottom through amber into a wide green plateau, then back toward amber at
// the top -- overcharging is a fault too, which a bar that simply got greener
// would not say.
struct Stop { double at; uint32_t colour; };
constexpr Stop kStops[] = {
    {0.00, 0xFF3B30},
    {0.34, 0xFFC53D},
    {0.52, 0x35E06B},
    {0.86, 0x35E06B},
    {1.00, 0xFFC53D},
};
constexpr int kStopCount = static_cast<int>(sizeof kStops / sizeof kStops[0]);

// How finely the gradient is sliced. The bar never repaints, so this costs
// nothing at run time; 48 across 289 px is about six pixels a slice, which is
// below what reads as a step.
constexpr int kSlices = 48;

uint32_t mix(uint32_t from, uint32_t to, double f) {
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const double a = (from >> sh) & 0xFF, b = (to >> sh) & 0xFF;
        out |= static_cast<uint32_t>(std::lround(a + (b - a) * f)) << sh;
    }
    return out;
}

// The gradient colour at `f` along the bar.
uint32_t colour_at(double f) {
    if (f <= kStops[0].at) return kStops[0].colour;
    for (int i = 1; i < kStopCount; ++i) {
        if (f > kStops[i].at) continue;
        const double span = kStops[i].at - kStops[i - 1].at;
        const double t = span > 0 ? (f - kStops[i - 1].at) / span : 0.0;
        return mix(kStops[i - 1].colour, kStops[i].colour, t);
    }
    return kStops[kStopCount - 1].colour;
}

void bar_draw_cb(lv_event_t* e) {
    lv_obj_t*   obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t   c;
    lv_obj_get_coords(obj, &c);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;

    // The ends are pills, the middle is square slices. Drawing order is what
    // makes that work: the rounded first slice has its right-hand rounding
    // covered by slice 1, and the rounded last slice is drawn AFTER its
    // neighbour so the notch its left-hand rounding cuts exposes that
    // neighbour's colour rather than the background -- and the two are adjacent
    // on the gradient, so the seam is invisible.
    const int radius = kBarH / 2;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < kSlices; ++i) {
            const bool last = (i == kSlices - 1);
            if ((pass == 0) == last) continue;   // last slice on the second pass
            const int x0 = c.x1 + kBarW * i / kSlices;
            const int x1 = c.x1 + kBarW * (i + 1) / kSlices - 1;
            lv_area_t a = {x0, c.y1, last ? c.x1 + kBarW - 1 : x1, c.y1 + kBarH - 1};
            d.radius   = (i == 0 || last) ? radius : 0;
            d.bg_color = lv_color_hex(colour_at((i + 0.5) / kSlices));
            lv_draw_rect(layer, &d, &a);
        }
    }
}

}  // namespace

Bar bar_build(lv_obj_t* root, int dy) {
    lv_obj_t* bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kBarW, kBarH);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, dy);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar, bar_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    Bar b;
    b.dy = dy;
    // The marker is a sibling of the bar, not a child: a child would be clipped
    // to the bar's 21 px and this one deliberately stands proud of it.
    b.marker = lv_obj_create(root);
    lv_obj_remove_style_all(b.marker);
    lv_obj_set_size(b.marker, kMarkW, kMarkH);
    lv_obj_set_style_radius(b.marker, kMarkW / 2, 0);
    lv_obj_set_style_bg_color(b.marker, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b.marker, LV_OPA_COVER, 0);
    lv_obj_align(b.marker, LV_ALIGN_CENTER, -kBarW / 2, dy);
    return b;
}

void bar_update(Bar& b, std::optional<double> v) {
    if (!b.marker) return;
    // No reading hides the marker outright. Unlike a needle, a marker parked at
    // the left end does not read as "waiting" -- it reads as a flat battery,
    // which is a specific and alarming claim to make about a car that simply
    // is not reporting volts (SPEC.md section 4).
    lv_obj_set_style_bg_opa(b.marker, v ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (!v) return;

    double f = (*v - kVLo) / (kVHi - kVLo);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    const int x = static_cast<int>(std::lround(-kBarW / 2.0 + f * kBarW));
    if (x == b.x_q) return;
    b.x_q = x;
    lv_obj_align(b.marker, LV_ALIGN_CENTER, x, b.dy);
}

}  // namespace gauge_ui
