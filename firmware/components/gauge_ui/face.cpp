#include "face.h"

#include <cmath>
#include <cstdio>
#include <optional>



namespace gauge_ui {
namespace {

// ---- geometry ------------------------------------------------------------
// The panel, and the arc ui.cpp draws on it: 434 px across with a 14 px band,
// so the band's centre line is at radius 210.
constexpr double kCx = 233, kCy = 233;
constexpr double kArcR = 210;

// The simulator's radii, as fractions of its own 104. Written as the division
// rather than the answer so the source drawing stays readable from here.
constexpr double kTickOuterR = kArcR * (93.0 / 104.0);
constexpr double kTickInnerR = kArcR * (84.0 / 104.0);
constexpr double kNumR       = kArcR * (71.0 / 104.0);
constexpr double kHubR       = kArcR * ( 7.5 / 104.0);

// ---- the engine view's scale, on the same 104-unit source drawing --------
// The temperature scale it is laid out on. 120, not the 110 the plain arc
// used: the simulator's top zone runs 105-120 and clipping it at 110 would
// throw every zone boundary off.
constexpr double kTempLo = 40, kTempHi = 120;

// Power's needle stops short of the fill arc; its peak mark sits across the
// arc's inner edge so the two never occupy the same pixels.
constexpr double kPwrNeedleR  = kArcR * ( 88.0 / 104.0);
constexpr double kPwrPeakOutR = kArcR * ( 98.0 / 104.0);
constexpr double kPwrPeakInR  = kArcR * ( 88.0 / 104.0);
constexpr uint32_t kPwrPeak   = 0xFFC53D;

// Bottom-left, clockwise over the top, to bottom-right. The same pair ui.cpp
// hands lv_arc_set_bg_angles.
constexpr double kStartDeg = 135, kSweepDeg = 270;

// The needle is quantised for the reason the arc is: it is a big object, and
// every position it takes costs its bounding box. One degree of a 270-degree
// sweep moves the tip about three pixels, which is below what the eye follows
// on a needle that is already easing toward its target.
constexpr int kNeedleSteps = 270;

// The simulator's palette, unchanged.
constexpr uint32_t kTrack     = 0x23262E;

// ---- the rev scale's ramp -------------------------------------------------
// Blue at rest, amber halfway up, red at the limiter. seg_colour() explains
// why the middle stop is there rather than mixing blue straight to red.
//
// The blue is deliberately deeper than the engine view's cold blue (0x4D96FF).
// On this gauge a colour is a claim, and the two views are saying different
// things -- "the engine is cold" against "you are off the cam". A shade apart
// keeps them from reading as the same statement.
constexpr uint32_t kRevCold   = 0x2F6BFF;
constexpr uint32_t kRevMid    = 0xFFC53D;   // the gauge's accent amber
constexpr uint32_t kRevHot    = 0xFF3B30;   // the simulator's redline red
// An unlit segment, and an unlit segment above the redline. See seg_dim_opa.
constexpr lv_opa_t kRevDim    = 55;
constexpr lv_opa_t kRevDimRed = 90;
// The gap between one segment and the next, in degrees. Without it forty
// segments read as one continuous bar and the scale says nothing new.
constexpr double kTachoSegGap = 1.4;
// How far outside the stroke each clipping box reaches: a pixel for the
// anti-aliased edge, and a couple more so a rounding disagreement between the
// sampled box and LVGL's own arc rasteriser can never clip a segment's corner.
constexpr int kTachoSegPad = 3;
constexpr int kPanelPx     = 466;          // this board's round panel, both ways

// Aliases for the shared rim geometry in face.h, kept because the draw
// callback below reads better with short names.
constexpr int kArcOuterR = kRimOuterR;
constexpr int kArcWidth  = kRimWidth;
constexpr uint32_t kTick      = 0x7D818B;
constexpr uint32_t kTickHot   = 0xFF5B52;
constexpr uint32_t kNumber    = 0xC9CCD4;
constexpr uint32_t kNeedle    = 0xE1000A;
constexpr uint32_t kHubFill   = 0x1A1C22;

// The engine rim's colour ramp: cold blue, through green where the engine is
// happy, to red. temp_colour() lays it out.
constexpr uint32_t kTempCold  = 0x4D96FF;
constexpr uint32_t kTempReady = 0x35E06B;
constexpr uint32_t kTempHot   = 0xFF3B30;
// Where green sits on the 40-120 scale. 85 C is the middle of this engine's
// normal running band -- today's drive sat at 89-92 -- so "in the green" still
// means what it meant when green was a zone from 80 to 105.
constexpr double   kTempReadyC = 85.0;
// Where the dial stops being warm and starts being a warning. The old zone
// arcs put red from here to the top of the scale; now it is where an unlit
// segment is held back less, so the hot end is marked on a cold engine.
constexpr double   kTempDanger = 105.0;

// One channel of a two-colour mix.
//
// noinline, like the two ramp functions and the box sampler below: every one of
// them is called from a build_under_*() that sits on app_main's init chain,
// whose stack high-water mark is down to a couple of hundred bytes and has
// boot-looped this board once already. Inlined, their locals all land in the
// same frame and the deepest view's build costs 80 bytes more than it needs to;
// kept out of line, the chain pays for one of them at a time.
__attribute__((noinline))
uint32_t mix(uint32_t from, uint32_t to, double f) {
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const double a = (from >> sh) & 0xFF, b = (to >> sh) & 0xFF;
        out |= static_cast<uint32_t>(std::lround(a + (b - a) * f)) << sh;
    }
    return out;
}

lv_point_precise_t polar(double r, double deg) {
    const double a = deg * M_PI / 180.0;
    return {static_cast<lv_value_precise_t>(std::lround(kCx + r * std::cos(a))),
            static_cast<lv_value_precise_t>(std::lround(kCy + r * std::sin(a)))};
}

// Where a value sits on the dial. Scale comes from the car profile, not a
// constant: an MX-5's 8000 rpm dial would misread badly on a diesel.
double dial_angle(double v, double v_max) {
    double f = (v_max > 0) ? v / v_max : 0.0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return kStartDeg + kSweepDeg * f;
}

// Rounds the power dial's label step to a multiple of 20 kW, so the numbering
// stays readable whatever ceiling the car profile gives it (140 -> 20s,
// 250 -> 40s). Copied from the simulator's buildPower().
int power_label_step(double p_max) {
    int step = static_cast<int>(std::ceil(p_max / 7.0 / 20.0)) * 20;
    return step < 20 ? 20 : step;
}

// An lv_line sizes itself to the bounding box of its points, measured from the
// object's own top-left. Giving it absolute coordinates therefore makes the
// object span the whole way from the parent's origin -- which for the needle
// would be most of the screen, and so most of the screen invalidated every
// time it moves. Placing the object at the points' own bounding box instead
// keeps the invalidated area to the needle.
void set_line(lv_obj_t* line, lv_point_precise_t* pts, lv_point_precise_t a,
              lv_point_precise_t b) {
    const lv_value_precise_t x0 = LV_MIN(a.x, b.x), y0 = LV_MIN(a.y, b.y);
    pts[0] = {a.x - x0, a.y - y0};
    pts[1] = {b.x - x0, b.y - y0};
    lv_line_set_points(line, pts, 2);
    lv_obj_set_pos(line, static_cast<int32_t>(x0), static_cast<int32_t>(y0));
}

// Do the two rectangles touch? The one thing the old heat band left behind:
// every segment scale's draw callback asks it forty times a frame.
bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

// ---- the tacho's rev scale ----------------------------------------------
// Built here rather than in ui.cpp because the segments ARE the tacho's face
// now: there is no band under them and no shutter over them.
//
// ONE object that paints all forty, not forty objects. Forty boxed lv_arcs was
// the first version and it worked -- 51-57 fps against the shutter's 17-28 --
// but eighty objects took LVGL's pool from 59% used to 79%, and this firmware
// has already been round that loop: the pool's allocator does not degrade when
// it runs out, it SPINS, so the board goes silent and needs a replug. 19 KB
// free is the figure that was hanging every swipe at a 64 KB pool
// (sdkconfig.defaults, CONFIG_LV_MEM_SIZE_KILOBYTES), and 79% left 19 KB.
//
// So the segments are drawn by hand into one object, the way the heat band was,
// and the pruning that boxing bought is done in the draw callback instead: each
// segment's rectangle is cached at build time, and a redraw skips every segment
// that does not touch the area being repainted. The rpm digits therefore cost
// forty rectangle comparisons and no drawing at all, and a segment lighting up
// invalidates its own ~30 px box rather than the rim.
struct Scale {
    struct Seg {
        int16_t   a0, a1;  // degrees, clockwise from 3 o'clock
        uint32_t  colour;  // fixed for the life of the view
        lv_opa_t  dim;     // what it fades to when the reading is below it
        lv_area_t area;    // its own rectangle, in the object's coordinates
    };
    Seg       seg[kScaleSegs];
    int       lit = 0;              // how many are lit, low end first
    lv_obj_t* obj = nullptr;
};
// One per view that wears a scale. They are on screen at different times but
// both exist for the life of the program, so neither can borrow the other's.
Scale g_rev_scale;                  // the tacho's revs
Scale g_temp_scale;                 // the engine view's coolant

// Where segment `i` starts and ends. Kept unwrapped -- past 360 for the last
// few -- because the drawing takes angles in that form; only a comparison
// against 360 would care.
void seg_angles(int i, double* a0, double* a1) {
    const double span = kSweepDeg / kScaleSegs;
    *a0 = kStartDeg + i * span + kTachoSegGap / 2.0;
    *a1 = kStartDeg + (i + 1) * span - kTachoSegGap / 2.0;
}

// Segment `i`'s colour, fixed for the life of the view.
//
// Blue at rest, amber halfway to the limiter, red at it. Two straight mixes
// rather than one blue-to-red, because a single mix between those two passes
// through purple and grey in the middle -- the revs you spend most of your
// driving at would be the muddiest part of the dial. Amber in the middle is
// also the colour this gauge already uses for an accent, so the ramp says
// "climbing" on the way up and only says "limit" at the top.
//
// Above the redline every segment is the flat red, so the top of the dial
// still reads as a limit rather than as more colour.
__attribute__((noinline))
uint32_t rev_colour(double mid_rpm, const gauge::Identity& id) {
    const double red = (id.rpm_red > 0) ? id.rpm_red : id.rpm_max;
    if (red <= 0 || mid_rpm >= red) return kRevHot;
    double f = mid_rpm / red;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (f < 0.5) ? mix(kRevCold, kRevMid, f * 2.0)
                     : mix(kRevMid, kRevHot, (f - 0.5) * 2.0);
}

// The engine view's ramp: cold blue, through green where the engine is happy,
// to red. It was four hard-edged zone arcs -- blue/amber/green/red -- and the
// seams between them were the loudest thing on the view: four colours meeting
// rather than one temperature changing. The scale did not need four names; it
// needed to look like heat.
//
// Green sits at 85 C rather than in the middle of the 40-120 dial, because 85
// is the middle of this engine's normal running band -- today's drive sat at
// 89-92 -- so "in the green" still means what it meant when green was a zone.
__attribute__((noinline))
uint32_t temp_colour(double c) {
    const double f   = (c - kTempLo) / (kTempHi - kTempLo);
    const double mid = (kTempReadyC - kTempLo) / (kTempHi - kTempLo);
    return (f <= mid) ? mix(kTempCold, kTempReady, mid > 0 ? f / mid : 0.0)
                      : mix(kTempReady, kTempHot, (f - mid) / (1.0 - mid));
}

// How far back an unlit segment is held. Its own colour, not grey, so the ramp
// is readable across the whole dial before the engine has reached it -- the
// same choice the bezel's page scale makes, and the reason the tacho needs no
// separate track ring at all.
//
// Past the redline the unlit shade is stronger, so the limit is still marked
// on a cold dial. That is what the old separate redline arc was for, and it
// was a 434 px object on the rim; this costs nothing.
lv_opa_t seg_dim(bool past_limit) {
    return past_limit ? kRevDimRed : kRevDim;
}

// The rectangle one segment's stroke can paint in, in the object's own
// coordinates. Sampled rather than solved, as ui.cpp does for the bezel: five
// points along each of the two radii bound seven degrees of arc to well under a
// pixel, and the box is padded anyway.
//
// Not lv_draw_arc_get_area(), which would do the same job: that call sits on
// app_main's init chain, whose stack high-water mark is down to a couple of
// hundred bytes and has boot-looped this board once already. Plain arithmetic
// costs the chain nothing.
__attribute__((noinline))
void seg_box(double a0, double a1, lv_area_t* out) {
    // Floats and no container: this frame is the deepest point of app_main's
    // init chain, and doubles plus an initializer_list cost it 88 of the 220
    // bytes it had left.
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    // Ten samples, not nine: five angles across the segment at BOTH radii. The
    // outer radius at the far end is a corner of the box, and an odd count
    // left it out.
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(
            (a0 + (a1 - a0) * (i / 2) / 4.0) * M_PI / 180.0);
        const float r = (i & 1) ? static_cast<float>(kArcOuterR)
                                : static_cast<float>(kArcOuterR - kArcWidth);
        const float px = kRimPx / 2.0f + r * cosf(a);
        const float py = kRimPx / 2.0f + r * sinf(a);
        if (px < x0) x0 = px;
        if (px > x1) x1 = px;
        if (py < y0) y0 = py;
        if (py > y1) y1 = py;
    }
    out->x1 = static_cast<int32_t>(x0) - kTachoSegPad;
    out->y1 = static_cast<int32_t>(y0) - kTachoSegPad;
    out->x2 = static_cast<int32_t>(x1) + kTachoSegPad;
    out->y2 = static_cast<int32_t>(y1) + kTachoSegPad;
}

// Paints the segments that touch the area being redrawn, and skips the rest
// before any drawing happens -- which is the whole point of caching the
// rectangles. Same shape as paint_band(), and centred the same way: at
// lv_arc's own centre, x1 + min(w,h)/2, which is a pixel off the midpoint of
// the coordinates and would show as a hairline against the ticks if it drifted.
void scale_draw_cb(lv_event_t* e) {
    const Scale& sc   = *static_cast<const Scale*>(lv_event_get_user_data(e));
    lv_obj_t*   obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t   coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = coords.x1 + lv_area_get_width(&coords) / 2;
    const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.width    = kArcWidth;
    d.radius   = kArcOuterR;
    // Square ends. A rounded cap adds half the stroke width along the arc at
    // each end -- 7 px here, against a gap of about 5 -- so rounding would eat
    // the gaps and put the dial back to being one continuous bar.
    d.rounded  = 0;
    for (int i = 0; i < kScaleSegs; ++i) {
        lv_area_t a = sc.seg[i].area;
        lv_area_move(&a, coords.x1, coords.y1);
        if (!areas_overlap(a, layer->_clip_area)) continue;
        d.color       = lv_color_hex(sc.seg[i].colour);
        d.opa         = (i < sc.lit) ? static_cast<lv_opa_t>(LV_OPA_COVER)
                                     : sc.seg[i].dim;
        d.start_angle = sc.seg[i].a0;
        d.end_angle   = sc.seg[i].a1;
        lv_draw_arc(layer, &d);
    }
}

// Hides one scale. Both bench commands land here.
void show_scale(Scale& sc, bool on) {
    if (!sc.obj) return;
    if (on) lv_obj_clear_flag(sc.obj, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(sc.obj, LV_OBJ_FLAG_HIDDEN);
}

// The object the segments are painted by. One per scale, sized to the rim and
// centred, carrying nothing but the draw callback -- so what LVGL sees is a
// single object whose own drawing skips everything the repaint does not touch.
lv_obj_t* mk_scale(lv_obj_t* root, Scale& sc) {
    lv_obj_t* obj = lv_obj_create(root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, kRimPx, kRimPx);
    lv_obj_center(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    // Built unlit, because the gauge comes up before the car has answered and
    // a dial that starts full would flash its whole range on the first frame
    // of a swipe onto the view.
    sc.lit = 0;
    lv_obj_add_event_cb(obj, scale_draw_cb, LV_EVENT_DRAW_MAIN, &sc);
    sc.obj = obj;
    return obj;
}

// How many segments a reading lights, and the repaint that costs.
//
// `min_lit` is the floor for a reading that exists at all. The tacho's is zero
// -- an engine at rest should show nothing -- but the engine view's is one: a
// cold car sits below the bottom of a 40 C dial, and an empty scale there
// would read as no data rather than as cold (SPEC.md section 4). A MISSING
// reading is what leaves every segment dim, on both views.
void scale_update(Scale& sc, std::optional<double> v, double lo, double hi,
                  int min_lit) {
    if (!sc.obj) return;
    int lit = 0;
    if (v && hi > lo) {
        lit = static_cast<int>((*v - lo) / (hi - lo) * kScaleSegs + 0.5);
        if (lit < min_lit) lit = min_lit;
        if (lit > kScaleSegs) lit = kScaleSegs;
    }
    // The whole point of the segments: the reading has to cross a segment
    // boundary before a single pixel changes. Most readings land here and stop.
    if (lit == sc.lit) return;
    const int from = (lit < sc.lit) ? lit : sc.lit;
    const int to   = (lit < sc.lit) ? sc.lit : lit;
    sc.lit = lit;
    // Only the segments the reading actually crossed, and each by its own
    // rectangle. Invalidating the object instead would repaint the whole rim
    // for one segment, which is the cost this design exists to avoid.
    lv_area_t coords;
    lv_obj_get_coords(sc.obj, &coords);
    for (int i = from; i < to; ++i) {
        lv_area_t a = sc.seg[i].area;
        lv_area_move(&a, coords.x1, coords.y1);
        lv_obj_invalidate_area(sc.obj, &a);
    }
}

lv_obj_t* mk_rim_arc(lv_obj_t* root, int a0, int a1, uint32_t colour, lv_opa_t opa,
                     bool rounded = true) {
    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_set_size(arc, kRimPx, kRimPx);
    lv_obj_center(arc);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_style(arc, nullptr, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(arc, a0, a1);
    lv_obj_set_style_arc_width(arc, kRimWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, rounded, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    return arc;
}

}  // namespace

// ---- the dial face -------------------------------------------------------

void build_under_tacho(lv_obj_t* root, const gauge::Identity& id) {
    // The rim carries the revs, because the middle of the screen cannot.
    //
    // The simulator warms the whole display as the engine picks up. Measured on
    // this board that costs 5-20 fps however it is drawn -- a backdrop covers
    // the panel, so every change to it repaints the panel, and rpm changes
    // constantly. Painting the frame by hand instead benched at 15 fps, worse
    // still.
    //
    // The rim does the same job for almost nothing, by making the colour a
    // property of POSITION on the dial rather than of time. Every segment is
    // coloured once here, blue at rest through amber to red at the rev limit;
    // rpm then only decides how many of them are lit. Nothing ever changes
    // colour, so nothing ever costs a repaint for its colour -- and what you
    // read at a glance, the dial climbing and getting hotter as you rev, is the
    // same thing the simulator says with a bar.
    Scale& sc = g_rev_scale;
    for (int i = 0; i < kScaleSegs; ++i) {
        double a0 = 0, a1 = 0;
        seg_angles(i, &a0, &a1);
        // Read at the middle of the segment: a segment is one flat colour, so
        // the middle is the only honest place to sample the ramp.
        const double mid_rpm = (i + 0.5) / kScaleSegs * id.rpm_max;
        const double red = (id.rpm_red > 0) ? id.rpm_red : id.rpm_max;
        sc.seg[i].a0     = static_cast<int16_t>(std::lround(a0)) % 360;
        sc.seg[i].a1     = static_cast<int16_t>(std::lround(a1)) % 360;
        sc.seg[i].colour = rev_colour(mid_rpm, id);
        sc.seg[i].dim    = seg_dim(red > 0 && mid_rpm >= red);
        // The rectangle this segment can paint in, cached once. This is what
        // lets a redraw skip 39 of the 40 and a lit segment invalidate only
        // itself. Taken from the UNWRAPPED angles, because a segment that
        // straddles 3 o'clock has a start larger than its end and sampling
        // between them the short way round would bound the wrong arc.
        seg_box(a0, a1, &sc.seg[i].area);
    }
    mk_scale(root, sc);
}

// The engine view's rim, on the same segment scale the tacho wears, and for
// the same reasons: colour is a property of POSITION on the dial, fixed at
// build time, so a coolant reading that climbs all drive never recolours
// anything -- and the reading is the COUNT of lit segments, so there is no mark
// to move. The white mark that used to slide along a graded band went the way
// of the tacho's needle: it said what the level already says, and it was the
// only object on this view that moved.
//
// The band it replaces was 54 arcs painted once into 377 KB of PSRAM. This
// costs one object and no allocation at all.
void build_under_engine(lv_obj_t* root) {
    Scale& sc = g_temp_scale;
    for (int i = 0; i < kScaleSegs; ++i) {
        double a0 = 0, a1 = 0;
        seg_angles(i, &a0, &a1);
        // Read at the middle of the segment, as the tacho does.
        const double c = kTempLo + (i + 0.5) / kScaleSegs * (kTempHi - kTempLo);
        sc.seg[i].a0     = static_cast<int16_t>(std::lround(a0)) % 360;
        sc.seg[i].a1     = static_cast<int16_t>(std::lround(a1)) % 360;
        sc.seg[i].colour = temp_colour(c);
        // Above 105 the unlit shade is stronger, so the hot end of the dial is
        // marked on a cold engine -- the same thing the tacho does above its
        // redline, and what the old red zone arc was for.
        sc.seg[i].dim    = seg_dim(c >= kTempDanger);
        seg_box(a0, a1, &sc.seg[i].area);
    }
    mk_scale(root, sc);
}

// Power's track only. The fill arc over it is the view's own lv_arc, built by
// ui.cpp, and the ticks and numbering go on top in build_over_power().
void build_under_power(lv_obj_t* root) {
    mk_rim_arc(root, static_cast<int>(kStartDeg), 45, kTrack, LV_OPA_COVER);
}

void face_build_under(lv_obj_t* root, const gauge::Identity& id, FaceKind kind) {
    switch (kind) {
        case FaceKind::Tacho:  build_under_tacho(root, id); break;
        case FaceKind::Engine: build_under_engine(root);    break;
        case FaceKind::Power:  build_under_power(root);     break;
    }
}

Face build_over_tacho(lv_obj_t* root, const gauge::Identity& id) {
    // A tick and a number per 1000 rpm, placed by value rather than by count,
    // so a 6500 redline still lands on the right mark.
    for (int i = 0; i * 1000 <= static_cast<int>(id.rpm_max); ++i) {
        const double rpm = i * 1000.0;
        const double a   = dial_angle(rpm, id.rpm_max);
        const bool   hot = (id.rpm_red > 0) && (rpm >= id.rpm_red);

        lv_obj_t* tick = lv_line_create(root);
        // Owned for the life of the program, like every other object here:
        // lv_line keeps the pointer, so the points cannot live on the stack or
        // inside a vector that may reallocate.
        set_line(tick, new lv_point_precise_t[2], polar(kTickOuterR, a), polar(kTickInnerR, a));
        lv_obj_set_style_line_width(tick, 4, 0);
        lv_obj_set_style_line_rounded(tick, true, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(hot ? kTickHot : kTick), 0);

        const lv_point_precise_t np = polar(kNumR, a);
        lv_obj_t* num = lv_label_create(root);
        lv_obj_set_style_text_font(num, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(num, lv_color_hex(hot ? kTickHot : kNumber), 0);
        lv_label_set_text_fmt(num, "%d", i);
        lv_obj_update_layout(num);
        lv_obj_set_pos(num, static_cast<int32_t>(np.x) - lv_obj_get_width(num) / 2,
                            static_cast<int32_t>(np.y) - lv_obj_get_height(num) / 2);
    }

    // No needle, and no hub for it to pivot on.
    //
    // The scale already says the revs, and a needle saying them a second time
    // was the last object on this view that moved every frame: a line from the
    // middle of the panel out to the rim, so its bounding box swept a quarter
    // of the screen, and it was invalidated by its own movement AND recoloured
    // as it heated. Forty boxed segments that change only when the reading
    // crosses 200 rpm cost a fraction of that.
    Face f;
    // Nothing of the tacho's is left in the Face but its ease: the scale owns
    // its own segments and its own lit count (see Scale).
    (void)id;
    return f;
}

// A line from the hub outward, at the dial's start position. Shared by every
// face: what differs between them is colour, width and length, not the object.
lv_obj_t* mk_needle(lv_obj_t* root, lv_point_precise_t* pts, uint32_t colour,
                    int width, double r) {
    lv_obj_t* n = lv_line_create(root);
    lv_obj_set_style_line_width(n, width, 0);
    lv_obj_set_style_line_rounded(n, true, 0);
    lv_obj_set_style_line_color(n, lv_color_hex(colour), 0);
    set_line(n, pts, {static_cast<lv_value_precise_t>(kCx),
                      static_cast<lv_value_precise_t>(kCy)},
             polar(r, kStartDeg));
    return n;
}

// The hub, drawn last so a needle's blunt inner end is covered rather than
// left hanging in the middle of the screen.
void mk_hub(lv_obj_t* root) {
    const int32_t hub = static_cast<int32_t>(std::lround(kHubR * 2));
    lv_obj_t* boss = lv_obj_create(root);
    lv_obj_remove_style_all(boss);
    lv_obj_set_size(boss, hub, hub);
    lv_obj_set_pos(boss, static_cast<int32_t>(kCx) - hub / 2,
                         static_cast<int32_t>(kCy) - hub / 2);
    lv_obj_set_style_radius(boss, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(boss, lv_color_hex(kHubFill), 0);
    lv_obj_set_style_bg_opa(boss, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boss, 5, 0);
    lv_obj_set_style_border_color(boss, lv_color_hex(kNeedle), 0);
    lv_obj_set_style_border_opa(boss, LV_OPA_COVER, 0);
}

// Engine: no ticks, no numbering, and no mark.
//
// The zones ARE the scale -- you read "in the green", not "94 degrees" -- and
// the number is already the hero, so ticks would only crowd a rim that the
// colour has made legible on its own. The white mark went with the tacho's
// needle: on a scale that fills to the reading it repeated what the lit end
// already says, and it was the last thing on this view that moved.
Face build_over_engine(lv_obj_t* root) {
    (void)root;
    Face f;
    f.kind = FaceKind::Engine;
    return f;
}

// Power: a tick and a kW number every `step`, then the peak mark and the
// needle. The peak goes on before the needle so that when the two coincide --
// which is exactly when you are at your best -- the red needle is on top.
Face build_over_power(lv_obj_t* root, const gauge::Identity& id) {
    const double p_max = id.power_max > 0 ? id.power_max : 140.0;
    const int step = power_label_step(p_max);
    for (int v = 0; v <= static_cast<int>(p_max); v += step) {
        const double a = dial_angle(v, p_max);
        lv_obj_t* tick = lv_line_create(root);
        set_line(tick, new lv_point_precise_t[2],
                 polar(kArcR * (93.0 / 104.0), a), polar(kArcR * (84.0 / 104.0), a));
        lv_obj_set_style_line_width(tick, 4, 0);
        lv_obj_set_style_line_rounded(tick, true, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(kTick), 0);

        const lv_point_precise_t np = polar(kNumR, a);
        lv_obj_t* num = lv_label_create(root);
        lv_obj_set_style_text_font(num, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(num, lv_color_hex(kNumber), 0);
        lv_label_set_text_fmt(num, "%d", v);
        lv_obj_update_layout(num);
        lv_obj_set_pos(num, static_cast<int32_t>(np.x) - lv_obj_get_width(num) / 2,
                            static_cast<int32_t>(np.y) - lv_obj_get_height(num) / 2);
    }

    Face f;
    f.kind = FaceKind::Power;
    f.peak_pts = new lv_point_precise_t[2];
    f.peak = lv_line_create(root);
    lv_obj_set_style_line_width(f.peak, 5, 0);
    lv_obj_set_style_line_rounded(f.peak, true, 0);
    lv_obj_set_style_line_color(f.peak, lv_color_hex(kPwrPeak), 0);
    set_line(f.peak, f.peak_pts, polar(kPwrPeakOutR, kStartDeg),
             polar(kPwrPeakInR, kStartDeg));
    lv_obj_set_style_line_opa(f.peak, LV_OPA_TRANSP, 0);

    f.needle_pts = new lv_point_precise_t[2];
    f.needle = mk_needle(root, f.needle_pts, kNeedle, 7, kPwrNeedleR);
    mk_hub(root);
    return f;
}

Face face_build_over(lv_obj_t* root, const gauge::Identity& id, FaceKind kind) {
    switch (kind) {
        case FaceKind::Engine: return build_over_engine(root);
        case FaceKind::Power:  return build_over_power(root, id);
        case FaceKind::Tacho:  break;
    }
    Face f = build_over_tacho(root, id);
    f.kind = FaceKind::Tacho;
    return f;
}

// The reading this face should draw: the reported one, eased.
//
// A channel that is absent resets the ease rather than easing toward zero. A
// needle sweeping down the dial because the link dropped would read as the
// engine stopping, and the same sweep back up when it returns would read as it
// starting -- both are lies about the car.
std::optional<double> ease_reading(Face& f, std::optional<double> raw, const Model& m) {
    if (!raw) { f.ease.reset(); return raw; }
    return f.ease.step(*raw, m.dt_s, ease_tau_ms() / 1000.0);
}

// Writes a line's opacity only when it changes. See Face::needle_opa.
void set_line_opa(lv_obj_t* line, int& last, int opa) {
    if (!line || last == opa) return;
    last = opa;
    lv_obj_set_style_line_opa(line, static_cast<lv_opa_t>(opa), 0);
}

void update_tacho(Face& f, const Model& m) {
    // Eased, then quantised to whole segments. Easing first is what puts a new
    // segment up on the frames between two readings; quantising first would
    // throw those frames away again.
    const auto rpm = ease_reading(f, m.st.get("rpm"), m);
    // Floor of zero: an engine at rest should show nothing lit, and a missing
    // reading leaves the whole scale dim rather than hidden -- an instrument
    // with nothing on it reads as broken, where a dim dial reads as waiting
    // (SPEC.md section 4). Every unlit segment still carries its own colour,
    // so the dial is legible before the car has said anything.
    scale_update(g_rev_scale, rpm, 0.0, m.id.rpm_max, 0);
}

// Engine: coolant lights the scale from the cold end up. Floor of one segment,
// because a car below 40 C is off the bottom of the dial and an empty scale
// there would read as no data rather than as cold.
void update_engine(Face& f, const Model& m) {
    const auto c = ease_reading(f, m.st.get("coolant"), m);
    scale_update(g_temp_scale, c, kTempLo, kTempHi, 1);
}

// Power: the needle follows kW, the amber mark stays at the best the drive has
// managed. The peak is hidden until there is one, because a mark sitting at
// zero would claim the car has been measured at zero rather than not yet
// measured at all.
void update_power(Face& f, const Model& m) {
    if (!f.needle) return;
    const double p_max = m.id.power_max > 0 ? m.id.power_max : 140.0;
    const auto kw = ease_reading(f, m.st.get("power_kw"), m);
    set_line_opa(f.needle, f.needle_opa, kw ? LV_OPA_COVER : 64);

    const double peak = m.st.peak_kw();
    if (f.peak) {
        set_line_opa(f.peak, f.peak_opa, (kw && peak > 0) ? LV_OPA_COVER : LV_OPA_TRANSP);
        int pq = static_cast<int>(std::lround(dial_angle(peak, p_max) - kStartDeg));
        if (pq < 0) pq = 0;
        if (pq > kNeedleSteps) pq = kNeedleSteps;
        if (pq != f.peak_q) {
            f.peak_q = pq;
            set_line(f.peak, f.peak_pts, polar(kPwrPeakOutR, kStartDeg + pq),
                     polar(kPwrPeakInR, kStartDeg + pq));
        }
    }

    const double v = kw ? *kw : 0.0;
    int q = static_cast<int>(std::lround(dial_angle(v, p_max) - kStartDeg));
    if (q < 0) q = 0;
    if (q > kNeedleSteps) q = kNeedleSteps;
    if (q == f.needle_q) return;
    f.needle_q = q;
    set_line(f.needle, f.needle_pts,
             {static_cast<lv_value_precise_t>(kCx), static_cast<lv_value_precise_t>(kCy)},
             polar(kPwrNeedleR, kStartDeg + q));
}

// Diagnostic: hide the engine view's scale. Kept pointed at that view because
// its serial command is "BAND", which is what the engine rim was when this
// trick caught the tacho's old band costing 210 ms of every full render.
void face_set_band_enabled(bool on) {
    show_scale(g_temp_scale, on);
}

// Diagnostic: hide the whole rev scale. The tacho's dial used to be one arc
// that "DIALS 0" could hide; it is forty boxed segments now, so this is what
// that command reaches for -- see gauge_ui::set_dial_enabled.
void face_set_rev_scale_enabled(bool on) {
    show_scale(g_rev_scale, on);
}

void face_update(Face& f, const Model& m) {
    switch (f.kind) {
        case FaceKind::Tacho:  update_tacho(f, m);  break;
        case FaceKind::Engine: update_engine(f, m); break;
        case FaceKind::Power:  update_power(f, m);  break;
    }
}

}  // namespace gauge_ui
