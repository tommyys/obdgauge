#include "gball.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "glow.h"

namespace gauge_ui {
namespace {

// Everything below is built once, in gball_build, and only moved afterwards.
// Nothing here may allocate on the draw path -- see the header.
lv_obj_t* g_rings[3] = {nullptr, nullptr, nullptr};
lv_obj_t* g_rim = nullptr;
lv_obj_t* g_dot = nullptr;
lv_obj_t* g_trail[kGBallTrail] = {nullptr};
lv_obj_t* g_peak_brake = nullptr;
lv_obj_t* g_peak_corner = nullptr;
lv_obj_t* g_pole = nullptr;
lv_obj_t* g_tally = nullptr;
lv_obj_t* g_score = nullptr;
lv_obj_t* g_coach = nullptr;
lv_obj_t* g_learn = nullptr;
lv_obj_t* g_ring_label = nullptr;

// The trail as a ring buffer of positions, oldest overwritten. Pixels, not g:
// converting once on the way in keeps the draw path free of maths as well as
// of allocation.
struct Pt { int x, y; };
Pt   g_hist[kGBallTrail];
int  g_hist_n = 0;          // how many are valid, up to kGBallTrail
int  g_hist_head = 0;       // where the next one goes

// Last intensity actually painted onto the rim, on the glow's own 40-step
// grid. Repainting a ring is cheap but not free, and intensity is a
// half-minute average -- it does not move most frames.
int g_rim_step = -1;
bool g_rim_over = false;

constexpr int kCx = 233;    // panel centre, 466 px
constexpr int kCy = 214;    // lifted, to match the glow's own 46% centre

// The rings a driver judges by, as fractions of full scale.
constexpr double kRingG[3] = {0.2, 0.4, 0.6};

lv_obj_t* mk_dot(lv_obj_t* parent, int d, uint32_t colour, int opa) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, static_cast<lv_opa_t>(opa), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

lv_obj_t* mk_ring(lv_obj_t* parent, int r, uint32_t colour, int opa, int w) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_border_opa(o, static_cast<lv_opa_t>(opa), 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, kCx - r, kCy - r);
    return o;
}

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour,
                   int dy) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
    return l;
}

void set_text_if_changed(lv_obj_t* l, const char* s) {
    if (!l) return;
    const char* cur = lv_label_get_text(l);
    if (cur && !strcmp(cur, s)) return;     // a redraw LVGL does not have to do
    lv_label_set_text(l, s);
}

void show(lv_obj_t* o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void place(lv_obj_t* o, int x, int y, int d) {
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x - d / 2, y - d / 2);
}

// g -> pixels. Right-hand cornering pushes you left in the seat and the dot
// goes the way the car is loaded -- right on the screen for a right-hander.
// Braking throws the dot forward, which is up. Both match what a driver's
// body feels, which is the only convention that can be read without thinking.
int px_of(double lat) {
    double f = lat / kGBallFullG;
    if (f > 1.0) f = 1.0;
    if (f < -1.0) f = -1.0;
    return kCx + static_cast<int>(f * kGBallRadius);
}
int py_of(double lon) {
    double f = lon / kGBallFullG;
    if (f > 1.0) f = 1.0;
    if (f < -1.0) f = -1.0;
    return kCy - static_cast<int>(f * kGBallRadius);
}

// Deep green -> ember over the first half, ember -> red over the second, so
// the handover sits near the pole threshold rather than at either end.
uint32_t rim_colour(double f) {
    auto mix = [](uint32_t a, uint32_t b, double t) {
        auto ch = [&](int sh) {
            const double x = (a >> sh) & 0xFF, y = (b >> sh) & 0xFF;
            return static_cast<uint32_t>(x + (y - x) * t) << sh;
        };
        return ch(16) | ch(8) | ch(0);
    };
    if (f < 0.5) return mix(gauge::kGlowCalm, gauge::kGlowEmber, f / 0.5);
    return mix(gauge::kGlowEmber, gauge::kGlowRed, (f - 0.5) / 0.5);
}

}  // namespace

void gball_build(lv_obj_t* parent) {
    for (int i = 0; i < 3; ++i) {
        const int r = static_cast<int>(kGBallRadius * kRingG[i] / kGBallFullG);
        g_rings[i] = mk_ring(parent, r, 0xFFFFFF, 28, 1);
    }
    // The rim. Lights up when the car goes past full scale rather than letting
    // the dot vanish off the edge: a dot you cannot see is the one moment you
    // most want to know about.
    g_rim = mk_ring(parent, kGBallRadius, 0xFFFFFF, 26, 2);
    // One number, on the ring worth knowing. Labelling all three would turn
    // the middle of the screen into a table; labelling none leaves the rings
    // as decoration.
    g_ring_label = lv_label_create(parent);
    lv_obj_set_style_text_font(g_ring_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_ring_label, lv_color_hex(0x55606C), 0);
    lv_label_set_text(g_ring_label, "0.4g");
    lv_obj_align(g_ring_label, LV_ALIGN_TOP_LEFT,
                 kCx + static_cast<int>(kGBallRadius * 0.4 / kGBallFullG) + 6,
                 kCy - 9);

    // Peak ghosts: the hardest stop and the hardest corner of the drive so
    // far, left where they happened.
    g_peak_brake  = mk_dot(parent, 7, 0xFFFFFF, 70);
    g_peak_corner = mk_dot(parent, 7, 0xFFFFFF, 70);

    // The trail, oldest faintest. Built once and moved -- see the header.
    for (int i = 0; i < kGBallTrail; ++i) {
        const int opa = 110 - 110 * i / kGBallTrail;
        g_trail[i] = mk_dot(parent, 5, 0x8FE3FF, opa);
    }
    g_dot = mk_dot(parent, 15, 0xEAF6FF, LV_OPA_COVER);

    g_pole  = mk_label(parent, &lv_font_montserrat_20, 0x808080, -150);
    g_tally = mk_label(parent, &lv_font_montserrat_14, 0x6A737F, -118);
    g_score = mk_label(parent, &lv_font_montserrat_48, 0xC8D4E4, 118);
    g_coach = mk_label(parent, &lv_font_montserrat_14, 0x808080, 158);
    // Shown instead of the dot while the mounting angle is still being worked
    // out. A blank circle with no explanation reads as a broken gauge.
    g_learn = mk_label(parent, &lv_font_montserrat_14, 0x55606C, 0);
    lv_obj_align(g_learn, LV_ALIGN_TOP_LEFT, kCx - 40, kCy - 9);
}

void gball_update(const Model& m) {
    const bool spirited = m.score.pole == gauge::Pole::Spirited;
    set_text_if_changed(g_pole, spirited ? "SPIRITED" : "NICE");
    lv_obj_set_style_text_color(g_pole,
                                lv_color_hex(spirited ? 0xE1000A : 0x35E06B), 0);

    char buf[64];
    auto total = m.score.total();
    if (total) std::snprintf(buf, sizeof buf, "%.0f", *total);
    else       std::snprintf(buf, sizeof buf, "--");
    set_text_if_changed(g_score, buf);
    set_text_if_changed(g_coach, m.score.coach().c_str());

    const gauge::GForce& g = m.score.g;
    if (!g.ready()) {
        // Honest about not knowing yet. The axes are learned from the car's
        // own speed changes, so this clears itself after a few minutes of
        // driving -- and after the first drive it is restored and never seen.
        show(g_dot, false);
        show(g_peak_brake, false);
        show(g_peak_corner, false);
        for (int i = 0; i < kGBallTrail; ++i) show(g_trail[i], false);
        show(g_learn, true);
        std::snprintf(buf, sizeof buf, "LEARNING %.0f%%", g.confidence() * 100.0);
        set_text_if_changed(g_learn, buf);
        set_text_if_changed(g_tally, "finding which way the car points");
        g_hist_n = g_hist_head = 0;
        return;
    }
    show(g_learn, false);

    g_hist[g_hist_head] = {px_of(g.lat), py_of(g.lon)};
    g_hist_head = (g_hist_head + 1) % kGBallTrail;
    if (g_hist_n < kGBallTrail) ++g_hist_n;

    // Walk backwards from the newest. Index 0 of the trail is the sample just
    // behind the dot, so the fade runs the right way round.
    for (int i = 0; i < kGBallTrail; ++i) {
        lv_obj_t* o = g_trail[i];
        const int age = i + 2;                  // 1 back is under the dot
        if (age > g_hist_n) { show(o, false); continue; }
        const Pt& p = g_hist[(g_hist_head - age + 2 * kGBallTrail) % kGBallTrail];
        show(o, true);
        place(o, p.x, p.y, 5);
    }
    const Pt& now = g_hist[(g_hist_head - 1 + kGBallTrail) % kGBallTrail];
    show(g_dot, true);
    place(g_dot, now.x, now.y, 15);

    // The rim: colour is the pole, thickness is whether the car has just gone
    // past full scale. Two signals on one object, and neither is lost -- a dot
    // that vanishes off the edge is the one moment you most want to see.
    const double f = std::max(0.0, std::min(1.0, m.score.intensity));
    const int step = static_cast<int>(f * gauge::kGlowSteps + 0.5);
    const bool over = g.total() >= kGBallFullG;
    if (step != g_rim_step || over != g_rim_over) {
        g_rim_step = step;
        g_rim_over = over;
        lv_obj_set_style_border_color(
            g_rim, lv_color_hex(rim_colour(static_cast<double>(step)
                                           / gauge::kGlowSteps)), 0);
        lv_obj_set_style_border_opa(g_rim, static_cast<lv_opa_t>(over ? 255 : 190), 0);
        lv_obj_set_style_border_width(g_rim, over ? 6 : 3, 0);
    }

    if (g.peak_lon_brake > 0.05) {
        show(g_peak_brake, true);
        place(g_peak_brake, kCx, py_of(g.peak_lon_brake), 7);
    }
    if (g.peak_lat > 0.05) {
        show(g_peak_corner, true);
        place(g_peak_corner, px_of(g.peak_lat), kCy, 7);
    }

    double peak = g.peak_lat;
    if (g.peak_lon_brake > peak) peak = g.peak_lon_brake;
    if (g.peak_lon_accel > peak) peak = g.peak_lon_accel;
    std::snprintf(buf, sizeof buf, "nice %.0fm  spirited %.0fm  peak %.2fg",
                  m.score.nice_s / 60.0, m.score.spirited_s / 60.0, peak);
    set_text_if_changed(g_tally, buf);
}

}  // namespace gauge_ui
