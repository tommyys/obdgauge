#include "gball.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>


namespace gauge_ui {
namespace {

// Everything below is built once, in gball_build, and only moved afterwards.
// Nothing here may allocate on the draw path -- see the header.
lv_obj_t* g_dot = nullptr;
lv_obj_t* g_trail[kGBallTrail] = {nullptr};
lv_obj_t* g_web = nullptr;          // spokes and tick ring, painted once
lv_obj_t* g_learn = nullptr;
// The four readings, laid out where the force is: braking throws you forward
// so it reads at the top, acceleration at the bottom, and the two corner
// directions to their own sides. Taken from the g-force gauge Tommy uses as
// his reference (2026-09-06) -- the numbers are in fixed places, so they are
// glanced at rather than read, which is the whole difficulty with text on a
// driving view.
lv_obj_t* g_val[4] = {nullptr, nullptr, nullptr, nullptr};
enum { kBrake = 0, kRight = 1, kAccel = 2, kLeft = 3 };
// The score number, the coach word, the nice/spirited/peak tally, the ring
// label and the two peak ghosts are all gone (Tommy, 2026-09-06: "i can't
// read them mid drive"). Only the pole word survives, because it is one word
// that changes twice a drive. Everything they said is on the drive summary
// afterwards, which is where reading belongs.

// The trail as a ring buffer of positions, oldest overwritten. Pixels, not g:
// converting once on the way in keeps the draw path free of maths as well as
// of allocation.
struct Pt { int x, y; };
Pt   g_hist[kGBallTrail];
int  g_hist_n = 0;          // how many are valid, up to kGBallTrail
int  g_hist_head = 0;       // where the next one goes
int  g_since_push = 0;      // updates since the last dot was laid down
bool g_trail_shown = false; // are the trail objects currently visible?

// Which pole the four numbers are currently inked in: -1 before the first
// update, so the opening colour is actually applied. A plain bool started at
// `false` and the first frame is also Nice, so the labels kept the grey they
// were built with and the pole was never shown at all.
int g_pole_ink = -1;
// Last colour step painted on the dot, on kDotSteps' grid.
int g_dot_step = -1;

constexpr int kCx = 233;    // panel centre, 466 px
// Was 214, lifted to sit under the glow's own 46% centre. Now that the circle
// IS the shared rim (gball.h), it has to be concentric with the rim every
// other view draws, or the two disagree by 19 px on a swipe between them.
constexpr int kCy = 233;

// The rings a driver judges by, in g -- a fifth of full scale apart, so the
// spacing itself says what they are worth.
//
// **One continuous ramp, not four colours.** The first attempt gave each ring
// a saturated colour of its own at 150/255 and 3 px, and it read as Christmas
// lights (Tommy, 2026-09-06). Four loud circles fight each other, and none of
// them is the thing the eye is actually following.
//
// What was really wrong was underneath that: the rim carried a SECOND
// green-to-red ramp for a different quantity -- the last half-minute's
// intensity -- so the outermost circle sat there green while everything
// inside it ramped to red. Two meanings, one colour language, touching.
// Tommy saw it straight away ("the outtest ring cannot be green"). The rim
// is now simply the limit, in red, and intensity is left to the one word on
// the view.
//
// So: a single green-to-red sweep by radius, thin and faint inside, opening
// up towards the edge. Nothing on this view changes colour except the dot.
constexpr double kRingG[4] = {0.2, 0.4, 0.6, 0.8};
constexpr uint32_t kWebColour  = 0x5A7FA8;   // blue-grey: rings and spokes
constexpr uint32_t kTickColour = 0x8FB4DC;   // a touch brighter, cooler
constexpr int kRingOpa   = 55;
constexpr int kRingWidth = 1;

// The blue band. **One thin ring, and it has to stay one thin ring.**
//
// This was a soft halo first: four overlapping translucent rings 13 px wide,
// spanning 100-136 px, to copy the glow on the reference gauge. It looked
// right and it took the view from 56 fps to a MEDIAN OF 7, with samples as
// low as 4. Nothing was wrong with the code -- the cost is simply the
// blended area, and four wide alpha rings over a 700 px circumference is an
// enormous number of pixels to re-blend every time anything above them
// moves. This is the same trap as the banned full-screen backdrop, at
// two-thirds the size.
//
// So: one ring, 5 px, at the radius the reference puts its glow -- outside
// the crowded middle, inside the numbers. About a ninth of the blended
// pixels, and it reads as the same band because a defined edge does not need
// the glow to be seen. If a soft glow is ever really wanted, it has to be a
// pre-rendered image blitted from PSRAM, not stacked alpha.
constexpr uint32_t kHaloColour = 0x2F86E0;
constexpr int kHaloR     = 124;
constexpr int kHaloOpa   = 65;
constexpr int kHaloWidth = 5;

// The tick ring, just inside the rim. Decoration in the strict sense -- no
// tick is ever read -- but it is what makes a round screen look like an
// instrument rather than a drawing of one, and it costs one object.
// **Nothing circular is drawn through a number.** The four readings sit on
// the four axes, and every ring used to run straight under them (Tommy,
// 2026-09-06: "the numbers shouldn't overlap with the ring"). There is no
// radius that dodges them either -- the rings are fifths of full scale, so
// wherever a number goes there is one within 15 px of it.
//
// So the scale is BROKEN where the labels are, the way a real instrument
// breaks its scale for its numbering: every ring, and the blue band, is drawn
// as four arcs with a gap of +/-kGapDeg at 0, 90, 180 and 270 degrees. The
// two spokes that lie along those axes stop short for the same reason. The
// tick ring is outside the numbers and needs none of this.
constexpr int kGapDeg      = 15;
constexpr int kSpokeStopPx = 104;   // where an axis spoke gives up

constexpr int kTicks       = 72;    // every 5 degrees
constexpr int kTickLong    = 12;    // every 6th, on the quarters
constexpr int kTickShort   = 6;
constexpr int kTickInsetPx = 10;    // clear of the rim's 14 px band
constexpr int kSpokes      = 12;    // radial lines of the web

// The dot's colour: green while cruising, amber when leaning on it, red at
// the limit. Quantised to this many steps so a reading that jitters in the
// third decimal does not repaint the object every frame -- the same reason
// the rim tracks gauge::kGlowSteps rather than its raw intensity.
constexpr int kDotSteps = 16;
constexpr uint32_t kDotCalm  = 0x35E06B;   // the green the NICE word uses
constexpr uint32_t kDotWarm  = 0xE8A317;   // amber
constexpr uint32_t kDotHot   = 0xFF2216;   // gauge::kGlowRed

// Do two rectangles touch? Same test face.cpp uses to skip rev-scale
// segments outside a repaint; stated again here rather than shared, because
// hoisting it into a header for two call sites is the kind of change that
// makes a one-pixel disagreement possible later.
bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return !(a.x2 < b.x1 || a.x1 > b.x2 || a.y2 < b.y1 || a.y1 > b.y2);
}

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

// The web: twelve radial spokes and a ring of ticks, painted by ONE object.
//
// One object and not sixty for the reason face.cpp gives about the rev scale:
// LVGL decides what to redraw from object rectangles, and sixty tick objects
// would be sixty rectangles dragged into the walk every time the dot moved a
// pixel. This paints only the pieces that touch the area actually being
// repainted and skips the rest before any drawing happens.
//
// Nothing in here ever changes. It is drawn from constants, so the object is
// never invalidated on its own account -- it repaints only because something
// moved over it.
void web_draw_cb(lv_event_t* e) {
    lv_obj_t*   obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t   coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = coords.x1 + lv_area_get_width(&coords) / 2;
    const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(kWebColour);
    d.round_start = d.round_end = 0;

    // Spokes first, then the rings, then ticks over the lot: a tick sitting on
    // the end of a spoke reads as one mark, and the other order leaves a
    // visible join.
    d.width = 1;
    d.opa   = 40;
    for (int i = 0; i < kSpokes / 2; ++i) {
        // Half as many lines as spokes -- each one is a diameter, so it draws
        // the spoke and the one opposite it in a single stroke. The two that
        // run along the axes stop short of the numbers instead: i == 0 is the
        // horizontal pair, i == kSpokes/4 the vertical one.
        const bool axis = (i == 0) || (i == kSpokes / 4);
        const int  len  = axis ? kSpokeStopPx : kGBallRadius;
        const double a = M_PI * i / (kSpokes / 2.0);
        const double dx = std::cos(a) * len, dy = std::sin(a) * len;
        lv_area_t box = {
            static_cast<int32_t>(cx - std::fabs(dx)), static_cast<int32_t>(cy - std::fabs(dy)),
            static_cast<int32_t>(cx + std::fabs(dx)), static_cast<int32_t>(cy + std::fabs(dy))};
        if (!areas_overlap(box, layer->_clip_area)) continue;
        d.p1.x = cx - dx; d.p1.y = cy - dy;
        d.p2.x = cx + dx; d.p2.y = cy + dy;
        lv_draw_line(layer, &d);
    }

    // The four scale rings and the blue band, each broken at the four axes.
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.rounded  = 0;
    for (int k = 0; k < 5; ++k) {
        const bool halo = (k == 4);
        ad.radius = halo ? kHaloR
                         : static_cast<int32_t>(kGBallRadius * kRingG[k] / kGBallFullG);
        ad.width  = halo ? kHaloWidth : kRingWidth;
        ad.color  = lv_color_hex(halo ? kHaloColour : kWebColour);
        ad.opa    = static_cast<lv_opa_t>(halo ? kHaloOpa : kRingOpa);
        for (int q = 0; q < 4; ++q) {
            ad.start_angle = q * 90 + kGapDeg;
            ad.end_angle   = q * 90 + 90 - kGapDeg;
            // A 60-degree arc's box, from the ends and the quadrant's own
            // corner. Cheap, a little generous, and only ever used to decide
            // whether to skip -- lv_draw_arc clips properly on its own.
            const double a0 = ad.start_angle * M_PI / 180.0;
            const double a1 = ad.end_angle * M_PI / 180.0;
            const double r  = ad.radius + ad.width;
            const double xs[3] = {std::cos(a0) * r, std::cos(a1) * r,
                                  std::cos((a0 + a1) / 2) * r};
            const double ys[3] = {std::sin(a0) * r, std::sin(a1) * r,
                                  std::sin((a0 + a1) / 2) * r};
            lv_area_t box = {
                static_cast<int32_t>(cx + std::min(std::min(xs[0], xs[1]), xs[2])),
                static_cast<int32_t>(cy + std::min(std::min(ys[0], ys[1]), ys[2])),
                static_cast<int32_t>(cx + std::max(std::max(xs[0], xs[1]), xs[2])),
                static_cast<int32_t>(cy + std::max(std::max(ys[0], ys[1]), ys[2]))};
            if (!areas_overlap(box, layer->_clip_area)) continue;
            lv_draw_arc(layer, &ad);
        }
    }

    d.opa = 90;
    for (int i = 0; i < kTicks; ++i) {
        const double a  = 2.0 * M_PI * i / kTicks;
        const double ca = std::cos(a), sa = std::sin(a);
        const int    r1 = kGBallRadius - kTickInsetPx;
        const int    r0 = r1 - ((i % 6) ? kTickShort : kTickLong);
        d.width = (i % 6) ? 1 : 2;
        d.color = lv_color_hex(kTickColour);
        const int32_t x0 = static_cast<int32_t>(cx + ca * r0);
        const int32_t y0 = static_cast<int32_t>(cy + sa * r0);
        const int32_t x1 = static_cast<int32_t>(cx + ca * r1);
        const int32_t y1 = static_cast<int32_t>(cy + sa * r1);
        lv_area_t box = {std::min(x0, x1) - 1, std::min(y0, y1) - 1,
                         std::max(x0, x1) + 1, std::max(y0, y1) + 1};
        if (!areas_overlap(box, layer->_clip_area)) continue;
        d.p1.x = x0; d.p1.y = y0;
        d.p2.x = x1; d.p2.y = y1;
        lv_draw_line(layer, &d);
    }
}

uint32_t mix(uint32_t a, uint32_t b, double t) {
    auto ch = [&](int sh) {
        const double x = (a >> sh) & 0xFF, y = (b >> sh) & 0xFF;
        return static_cast<uint32_t>(x + (y - x) * t) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

// The dot, on the same green-to-red language as the rim but on the instant's
// g rather than the half-minute's intensity. Bright the whole way along: the
// rim's ember is deep because a big ring at that brightness would light the
// whole panel, and a 15 px dot has the opposite problem.
uint32_t dot_colour(double f) {
    if (f < 0.5) return mix(kDotCalm, kDotWarm, f / 0.5);
    return mix(kDotWarm, kDotHot, (f - 0.5) / 0.5);
}

}  // namespace

void gball_build(lv_obj_t* parent) {
    // The spokes and ticks, under everything that moves.
    g_web = lv_obj_create(parent);
    lv_obj_remove_style_all(g_web);
    lv_obj_set_size(g_web, kGBallRadius * 2, kGBallRadius * 2);
    lv_obj_align(g_web, LV_ALIGN_TOP_LEFT, kCx - kGBallRadius, kCy - kGBallRadius);
    lv_obj_remove_flag(g_web, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_web, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_web, web_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_move_to_index(g_web, 0);
    // The trail, oldest faintest. Built once and moved -- see the header.
    for (int i = 0; i < kGBallTrail; ++i) {
        const int opa = 110 - 110 * i / kGBallTrail;
        g_trail[i] = mk_dot(parent, 5, 0x8FE3FF, opa);
    }
    g_dot = mk_dot(parent, 15, 0xEAF6FF, LV_OPA_COVER);

    // The four readings, and **no labels under them**.
    //
    // There were BRAKE / ACCEL / LEFT / RIGHT captions at 14 px. They are
    // gone (Tommy, 2026-09-06). The position already says which is which --
    // top is braking, bottom is acceleration, the sides are cornering -- so
    // the word is read once and is clutter every drive after that. It is the
    // same call that took the score number and the coach word off, and it
    // also ended the collision the captions had with the make/model banner:
    // a label you cannot fit is usually a label you do not need.
    //
    // 28 px. The space the captions freed went into 48 px for one build and
    // that was too big (Tommy, 2026-09-06: "too huge") -- on a 466 px panel
    // four 48 px numbers crowd the middle and leave the dot nowhere to live.
    // Readability was never about size here; it was about the numbers being
    // in fixed places.
    //
    // The panel is round, so the vertical pair sits tighter than the
    // horizontal one: the wall clock owns the top and the banner (at +178)
    // owns the bottom, while at mid-height the full width is free. 130
    // leaves the ACCEL number 23 px clear of the car's name.
    static const int kDx[4] = {0, 150, 0, -150};
    static const int kDy[4] = {-130, 0, 130, 0};
    for (int i = 0; i < 4; ++i) {
        g_val[i] = mk_label(parent, &lv_font_montserrat_28, 0x808080, 0);
        lv_obj_align(g_val[i], LV_ALIGN_CENTER, kDx[i], kDy[i]);
        lv_label_set_text(g_val[i], "0.0");
    }
    // Shown instead of the dot while the mounting angle is still being worked
    // out. A blank circle with no explanation reads as a broken gauge.
    g_learn = mk_label(parent, &lv_font_montserrat_14, 0x55606C, 0);
    lv_obj_align(g_learn, LV_ALIGN_TOP_LEFT, kCx - 40, kCy - 9);
}

void gball_update(const Model& m) {
    // The pole is the COLOUR of the numbers now, not a word next to them
    // (Tommy, 2026-09-06: "if i'm driving spiritedly, red text; driving
    // leisurely, green text"). One signal, carried by text that has to be on
    // the screen anyway, and nothing extra to read.
    const bool spirited = m.score.pole == gauge::Pole::Spirited;
    const uint32_t ink = spirited ? 0xE1000A : 0x35E06B;
    if (g_pole_ink != static_cast<int>(spirited)) {
        g_pole_ink = static_cast<int>(spirited);
        for (int i = 0; i < 4; ++i)
            lv_obj_set_style_text_color(g_val[i], lv_color_hex(ink), 0);
    }

    char buf[64];
    const gauge::GForce& g = m.score.g;
    if (!g.ready()) {
        // Honest about not knowing yet. The axes are learned from the car's
        // own speed changes, so this clears itself after a few minutes of
        // driving -- and after the first drive it is restored and never seen.
        show(g_dot, false);
        // Only once, on the way in. lv_obj_add_flag invalidates the object
        // whether or not the flag was already set, so re-hiding 48 trail dots
        // on every one of 62 frames a second cost the view 23 fps at the desk
        // -- where it is ALWAYS in this branch, because the recorder does not
        // read the IMU with no car connected. It was invisible at 24 dots.
        if (g_trail_shown) {
            g_trail_shown = false;
            for (int i = 0; i < kGBallTrail; ++i) show(g_trail[i], false);
        }
        g_since_push = 0;
        show(g_learn, true);
        std::snprintf(buf, sizeof buf, "LEARNING %.0f%%", g.confidence() * 100.0);
        set_text_if_changed(g_learn, buf);
        g_hist_n = g_hist_head = 0;
        return;
    }
    show(g_learn, false);

    // Each number is the force in ITS OWN direction, and zero when the car is
    // being thrown the other way -- so braking reads at the top and shows 0.0
    // at the bottom, which is how the reference gauge does it and how a
    // driver's body reports it. lon is positive under braking, lat positive
    // in a right-hander (gforce.h).
    const double dir[4] = {g.lon, g.lat, -g.lon, -g.lat};
    for (int i = 0; i < 4; ++i) {
        std::snprintf(buf, sizeof buf, "%.1f", dir[i] > 0.0 ? dir[i] : 0.0);
        set_text_if_changed(g_val[i], buf);
    }

    const Pt here = {px_of(g.lat), py_of(g.lon)};

    // One dot every kGBallTrailEveryN updates -- see the note in gball.h. The
    // trail objects are only touched on a frame that laid one down, so three
    // frames in four move nothing but the dot itself.
    if (++g_since_push >= kGBallTrailEveryN) {
        g_since_push = 0;
        g_hist[g_hist_head] = here;
        g_hist_head = (g_hist_head + 1) % kGBallTrail;
        if (g_hist_n < kGBallTrail) ++g_hist_n;

        // Walk backwards from the newest. Index 0 of the trail is the sample
        // just behind the dot, so the fade runs the right way round.
        g_trail_shown = true;
        for (int i = 0; i < kGBallTrail; ++i) {
            lv_obj_t* o = g_trail[i];
            const int age = i + 1;
            if (age > g_hist_n) { show(o, false); continue; }
            const Pt& p = g_hist[(g_hist_head - age + 2 * kGBallTrail) % kGBallTrail];
            show(o, true);
            place(o, p.x, p.y, 5);
        }
    }
    const Pt& now = here;
    show(g_dot, true);
    place(g_dot, now.x, now.y, 15);

    // The dot carries the green-to-red. One object, repainted only when it
    // crosses one of kDotSteps -- see the note on the rings above.
    const double gf = std::max(0.0, std::min(1.0, g.total() / kGBallFullG));
    const int dot_step = static_cast<int>(gf * kDotSteps + 0.5);
    if (dot_step != g_dot_step) {
        g_dot_step = dot_step;
        lv_obj_set_style_bg_color(
            g_dot, lv_color_hex(dot_colour(static_cast<double>(dot_step) / kDotSteps)), 0);
    }


}

}  // namespace gauge_ui
