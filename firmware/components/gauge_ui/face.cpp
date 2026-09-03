#include "face.h"

#include <cmath>
#include <cstdio>
#include <optional>

#include "esp_heap_caps.h"


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

// ---- engine and power, same 104-unit source drawing ----------------------
// The engine mark spans the band, R+9 out to R-9, so it reads as a pointer
// ON the zones rather than a needle beneath them.
constexpr double kEngMarkOutR = kArcR * (113.0 / 104.0);
constexpr double kEngMarkInR  = kArcR * ( 95.0 / 104.0);
// The temperature scale the zones are laid out on. 120, not the 110 the plain
// arc used: the simulator's top zone runs 105-120 and clipping it at 110 would
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

// How finely the heat band is graded.
//
// This was 8, as separate lv_arc objects, and the steps were plainly visible.
// Raising the count that way is not an option: 16 objects already cost about
// four frames a second, because the rim is where every partial redraw lands and
// each object is another one to walk. So the band is now ONE object that paints
// its own segments (band_draw_cb), and segments outside the area being
// repainted are skipped before any drawing happens. The count is then close to
// free, and 54 divides the 270-degree sweep into whole degrees -- five each,
// which at this radius is about sixteen pixels of arc per shade.
constexpr int kHeatBands = 54;
constexpr int kBandDeg   = 5;      // kHeatBands * kBandDeg == kSweepDeg
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
// happy, to red.
//
// This was four hard-edged zone arcs, blue/amber/green/red, and the seams
// between them were the loudest thing on the view -- four colours meeting
// rather than one temperature changing. The scale did not need four names; it
// needed to look like heat.
//
// The alpha is folded in at build time, as the zones did it: LVGL would charge
// for a translucent arc over the track on every repaint, and the track is a
// known flat colour, so the blend is done once here.
constexpr uint32_t kTempCold  = 0x4D96FF;
constexpr uint32_t kTempReady = 0x35E06B;
constexpr uint32_t kTempHot   = 0xFF3B30;
// Where green sits on the 40-120 scale. 85 C is the middle of this engine's
// normal running band -- today's drive sat at 89-92 -- so "in the green" still
// means what it meant when green was a zone from 80 to 105.
constexpr double   kTempReadyC = 85.0;
constexpr double   kTempAlpha  = 0.55;
constexpr uint32_t kEngMark   = 0xFFFFFF;

// One channel of a two-colour mix.
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

// The same thing on a scale that does not start at zero -- the engine view's
// 40-120 C. Written separately rather than folding a `lo` into dial_angle()
// because every tacho call would then carry a zero it does not need.
double range_angle(double v, double lo, double hi) {
    double f = (hi > lo) ? (v - lo) / (hi - lo) : 0.0;
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

// A graded ring: 54 segments coloured once, painted as one object.
//
// There are two of these now. The tacho's runs cold-to-redline off the car's
// own rev limit; the engine's runs cold-to-hot across the coolant scale, which
// replaced four hard-edged zone arcs -- the same 54-segment machinery, so the
// gradient costs what the zones cost and looks like one colour changing rather
// than four colours meeting.
struct BandSeg { int16_t a0, a1; uint32_t colour; };
struct Band {
    BandSeg       seg[kHeatBands];
    int           n  = 0;
    uint16_t*     px = nullptr;      // its own PSRAM canvas, 377 KB
    lv_draw_buf_t db{};
};
Band g_engine_band;
// Diagnostic handle. The band is 54 arcs painted into one object, and since the
// tacho became a segment scale the engine view is the only one that has one --
// see face_set_band_enabled.
lv_obj_t* g_band_obj = nullptr;
// The tacho's rev scale, in dial order. Held here as well as in the Face so
// build_under_tacho can fill it before face_build_over runs; the Face copies
// the pointers and does the lighting.
lv_obj_t* g_tacho_seg[kTachoSegs] = {nullptr};
// Their clipping boxes, kept only so the bench can hide the whole rev scale
// and read what it costs -- see face_set_rev_scale_enabled.
lv_obj_t* g_tacho_box[kTachoSegs] = {nullptr};

bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

// Paints the whole heat band at `cx,cy` on `layer`. Called once into a canvas
// on a board with PSRAM to spare (mk_band), and per redraw by band_draw_cb on
// one without; the per-segment skip keeps that second case's small redraws
// small.
void paint_band(lv_layer_t* layer, int32_t cx, int32_t cy, const Band& b) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.width    = kArcWidth;
    d.radius   = kArcOuterR;
    d.opa      = LV_OPA_COVER;
    // Square where segments butt together -- rounded joins scalloped every one
    // of the 52 seams, which was half of why the old band looked stepped rather
    // than graded. The band's two EXTREMITIES are a different question: every
    // other ring on every other view is drawn with rounded caps, so a
    // square-ended tacho was the odd one out. Rounding just the first and last
    // segment gives the ring the same silhouette without touching the joins;
    // the rounding each of those two puts on its INNER end is painted over by
    // the neighbour that overlaps it.
    for (int i = 0; i < b.n; ++i) {
        d.rounded = (i == 0 || i == b.n - 1) ? 1 : 0;
        lv_area_t a;
        lv_draw_arc_get_area(cx, cy, d.radius, b.seg[i].a0, b.seg[i].a1,
                             d.width, d.rounded != 0, &a);
        if (!areas_overlap(a, layer->_clip_area)) continue;
        d.color       = lv_color_hex(b.seg[i].colour);
        d.start_angle = b.seg[i].a0;
        d.end_angle   = b.seg[i].a1;
        lv_draw_arc(layer, &d);
    }
}

// The fallback path, used only when the band could not be painted into a
// buffer. Kept because a gauge that draws its dial slowly is still a gauge.
void band_draw_cb(lv_event_t* e) {
    lv_obj_t*   obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t   coords;
    lv_obj_get_coords(obj, &coords);
    // Centre exactly where lv_arc puts one, which is NOT the midpoint of the
    // coordinates. lv_arc's get_center() uses x1 + min(w,h)/2; the midpoint
    // (x1 + x2)/2 truncates half a pixel low, because x2 is the last pixel
    // rather than one past it. For a 434-wide object at x1=16 that is 232 here
    // against 233 there -- one pixel, but the band is UNDER the shutter and the
    // redline, so a one-pixel offset uncovers one pixel of band along an edge
    // for the entire sweep. Where the band is hot that edge is red, which is
    // the red seen bleeding out of the grey rim.
    const Band* b = static_cast<const Band*>(lv_event_get_user_data(e));
    paint_band(layer, coords.x1 + lv_area_get_width(&coords) / 2,
               coords.y1 + lv_area_get_height(&coords) / 2, *b);
}

// The band, painted ONCE into a buffer instead of on every redraw.
//
// The 54 segments never change: their colours come from the car's redline, not
// from the reading, and the shutter arc on top is what shows the engine's
// share of them. Drawing them live cost the tacho 210 ms of every full render
// -- measured on the board by hiding the band, which took a slide into the
// tacho from 283 ms to 73 ms while every other view sat at 56-96 ms. A swipe
// onto the tacho renders the whole view into a buffer, so that 210 ms was paid
// in full on every single one.
//
// Painted into a canvas, the same band costs one image blit, and a partial
// redraw costs only the part of the blit it clips to. The buffer is 377 KB of
// PSRAM, which this board has 8 MB of.
//
// Opaque RGB565 rather than ARGB8888: the band sits at the bottom of the
// tacho's z-order over a black screen, so there is nothing underneath for it
// to blend with, and the alpha format would cost twice the memory and a slower
// blit. If the allocation fails the old live-drawing callback is used instead
// -- slow, but the gauge still comes up, which matters more.
lv_obj_t* mk_band(lv_obj_t* root, Band& band, const char* who) {
    const size_t bytes = static_cast<size_t>(kRimPx) * kRimPx * 2;
    if (!band.px)
        band.px = static_cast<uint16_t*>(
            heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM));

    if (band.px &&
        lv_draw_buf_init(&band.db, kRimPx, kRimPx, LV_COLOR_FORMAT_RGB565,
                         LV_STRIDE_AUTO, band.px, bytes) == LV_RESULT_OK) {
        lv_obj_t* canvas = lv_canvas_create(root);
        lv_canvas_set_draw_buf(canvas, &band.db);
        lv_obj_center(canvas);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        // Canvas-local coordinates, and the centre lv_arc would use for an
        // object of this size: x1 + min(w,h)/2 with x1 = 0. The one-pixel
        // difference from the midpoint is load-bearing -- see band_draw_cb.
        paint_band(&layer, kRimPx / 2, kRimPx / 2, band);
        lv_canvas_finish_layer(canvas, &layer);
        printf("%s: band painted once into %u B of PSRAM\n", who, (unsigned)bytes);
        return canvas;
    }

    printf("%s: no PSRAM for the band -- drawing it live every frame\n", who);
    lv_obj_t* obj = lv_obj_create(root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, kRimPx, kRimPx);
    lv_obj_center(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, band_draw_cb, LV_EVENT_DRAW_MAIN, &band);
    return obj;
}

// ---- the tacho's rev scale ----------------------------------------------
// Built here rather than in ui.cpp because the segments ARE the tacho's face
// now: there is no band under them and no shutter over them.

// Where segment `i` starts and ends, in LVGL's clockwise-from-3-o'clock
// degrees. Left unwrapped -- past 360 for the last few -- because seg_box()
// below feeds them to cos() and sin(), which do not care; only the angles
// handed to LVGL are brought back into range.
void seg_angles(int i, double* a0, double* a1) {
    const double span = kSweepDeg / kTachoSegs;
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
uint32_t seg_colour(int i, const gauge::Identity& id) {
    const double mid_rpm = (i + 0.5) / kTachoSegs * id.rpm_max;
    const double red = (id.rpm_red > 0) ? id.rpm_red : id.rpm_max;
    if (mid_rpm >= red) return kRevHot;
    double f = (red > 0) ? mid_rpm / red : 0.0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (f < 0.5) ? mix(kRevCold, kRevMid, f * 2.0)
                     : mix(kRevMid, kRevHot, (f - 0.5) * 2.0);
}

// How far back an unlit segment is held. Its own colour, not grey, so the ramp
// is readable across the whole dial before the engine has reached it -- the
// same choice the bezel's page scale makes, and the reason the tacho needs no
// separate track ring at all.
//
// Past the redline the unlit shade is stronger, so the limit is still marked
// on a cold dial. That is what the old separate redline arc was for, and it
// was a 434 px object on the rim; this costs nothing.
lv_opa_t seg_dim_opa(int i, double rpm_max, double rpm_red) {
    const double mid_rpm = (i + 0.5) / kTachoSegs * rpm_max;
    const double red = (rpm_red > 0) ? rpm_red : rpm_max;
    return (mid_rpm >= red) ? kRevDimRed : kRevDim;
}

// The bounding rectangle, in panel coordinates, of one segment's stroke.
// Sampled rather than solved, as ui.cpp does for the bezel: five points along
// each radius bound seven degrees of arc to well under a pixel, and the box is
// padded anyway.
void seg_box(double a0, double a1, int* x, int* y, int* w, int* h) {
    const double r_out = kArcOuterR, r_in = kArcOuterR - kArcWidth;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i <= 4; ++i) {
        const double a = (a0 + (a1 - a0) * i / 4.0) * M_PI / 180.0;
        for (const double r : {r_in, r_out}) {
            const double px = kCx + r * std::cos(a), py = kCy + r * std::sin(a);
            if (px < x0) x0 = px;
            if (px > x1) x1 = px;
            if (py < y0) y0 = py;
            if (py > y1) y1 = py;
        }
    }
    *x = static_cast<int>(x0) - kTachoSegPad;
    *y = static_cast<int>(y0) - kTachoSegPad;
    *w = static_cast<int>(x1 - x0) + 2 * kTachoSegPad + 1;
    *h = static_cast<int>(y1 - y0) + 2 * kTachoSegPad + 1;
}

// One segment: an arc drawn by its BACKGROUND part only, so both of its ends
// can be placed by hand. The indicator and knob are the value-tracking half of
// lv_arc and this is not a meter -- what the reading changes is which segments
// are lit, not any one segment's length.
lv_obj_t* mk_seg_arc(lv_obj_t* box, uint32_t colour, int box_x, int box_y,
                     double a0, double a1) {
    lv_obj_t* a = lv_arc_create(box);
    lv_obj_set_size(a, kRimPx, kRimPx);
    // Positioned by hand rather than centred, because the parent is the
    // segment's own little box and not the view: the arc has to keep the
    // PANEL's centre, so it hangs out of its parent on every side and is
    // clipped back to it.
    lv_obj_set_pos(a, kPanelPx / 2 - kRimPx / 2 - box_x,
                      kPanelPx / 2 - kRimPx / 2 - box_y);
    lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(a, 0, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, kArcWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(colour), LV_PART_MAIN);
    // Square ends. A rounded cap adds half the stroke width along the arc at
    // each end -- 7 px here, against a gap of about 5 -- so rounding would eat
    // the gaps and put the dial back to being one continuous bar.
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    // LVGL wants its angles in range, and the segment that straddles 3 o'clock
    // ends up with a start larger than its end. That is the same wrap the
    // 135-to-45 rings use and lv_arc reads it the same way.
    lv_arc_set_bg_angles(a, static_cast<lv_value_precise_t>(std::fmod(a0, 360.0)),
                            static_cast<lv_value_precise_t>(std::fmod(a1, 360.0)));
    return a;
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
    // coloured once at build time, blue at rest through amber to red at the rev
    // limit; rpm then only decides how many of them are lit. Nothing ever
    // changes colour, so nothing ever costs a repaint for its colour -- and
    // what you read at a glance, the dial climbing and getting hotter as you
    // rev, is the same thing the simulator says with a bar.
    for (int i = 0; i < kTachoSegs; ++i) {
        double a0 = 0, a1 = 0;
        seg_angles(i, &a0, &a1);
        int bx = 0, by = 0, bw = 0, bh = 0;
        seg_box(a0, a1, &bx, &by, &bw, &bh);

        // Its own little clipping box, which is the whole performance argument
        // -- see kTachoSegs in face.h. An lv_arc's object is the full 434 px
        // square whatever it paints, so forty of them loose in the view would
        // each be walked by every dirty rectangle the rpm digits make. Boxed,
        // LVGL prunes the ones the digits do not touch, which is all of them.
        lv_obj_t* box = lv_obj_create(root);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, bw, bh);
        lv_obj_set_pos(box, bx, by);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
        g_tacho_box[i] = box;
        g_tacho_seg[i] = mk_seg_arc(box, seg_colour(i, id), bx, by, a0, a1);
        // Built unlit, because the gauge comes up before the car has answered
        // and a dial that starts fully lit would flash the whole rev range on
        // the first frame of a swipe onto the tacho.
        lv_obj_set_style_arc_opa(g_tacho_seg[i],
                                 seg_dim_opa(i, id.rpm_max, id.rpm_red), LV_PART_MAIN);
    }
}

// The engine view's zones. Same idea as the tacho's heat band and for the same
// reason: colour is a property of POSITION on the dial, fixed at build time, so
// a coolant reading that climbs all drive never repaints a single zone. Only
// the mark moves.
void build_under_engine(lv_obj_t* root) {
    mk_rim_arc(root, static_cast<int>(kStartDeg), 45, kTrack, LV_OPA_COVER);
    // The same 54-segment band the tacho uses, so the gradient costs one blit
    // rather than 54 objects on the rim -- the lesson build_under_tacho paid
    // for. Its own canvas: the two rings hold different colours and are on
    // screen at different times.
    Band& band = g_engine_band;
    band.n = 0;
    for (int i = 0; i < kHeatBands; ++i) {
        // Read at the middle of the segment: the band never repaints, so
        // stepping it would only make it look stepped.
        const double f = (static_cast<double>(i) + 0.5) / kHeatBands;
        const double c = kTempLo + f * (kTempHi - kTempLo);
        const double mid = (kTempReadyC - kTempLo) / (kTempHi - kTempLo);
        const uint32_t hue = (f <= mid)
            ? mix(kTempCold, kTempReady, mid > 0 ? f / mid : 0.0)
            : mix(kTempReady, kTempHot, (f - mid) / (1.0 - mid));
        (void)c;
        band.seg[band.n].colour = mix(kTrack, hue, kTempAlpha);
        band.seg[band.n].a0 = static_cast<int16_t>(
            (static_cast<int>(kStartDeg) + i * kBandDeg) % 360);
        // One degree of overlap onto the next segment so no hairline of
        // background shows through the seam -- but not on the last, which has
        // no neighbour and would spill past the end of the sweep.
        const bool last = (i == kHeatBands - 1);
        band.seg[band.n].a1 = static_cast<int16_t>(
            (static_cast<int>(kStartDeg) + (i + 1) * kBandDeg + (last ? 0 : 1)) % 360);
        ++band.n;
    }
    g_band_obj = mk_band(root, band, "engine");
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
    for (int i = 0; i < kTachoSegs; ++i) f.seg[i] = g_tacho_seg[i];
    f.lit = 0;
    // Remembered for the dim shades, which differ above the redline.
    f.rpm_max = id.rpm_max;
    f.rpm_red = id.rpm_red;
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

// Engine: no ticks and no numbering. The zones ARE the scale -- you read "in
// the green", not "94 degrees" -- and the number is already the hero, so ticks
// would only crowd a rim that four colours have made legible on their own.
Face build_over_engine(lv_obj_t* root) {
    Face f;
    f.kind = FaceKind::Engine;
    f.needle_pts = new lv_point_precise_t[2];
    // Not from the hub: the engine mark is a short white bar lying across the
    // band, so its two ends are both out at the rim.
    f.needle = lv_line_create(root);
    lv_obj_set_style_line_width(f.needle, 6, 0);
    lv_obj_set_style_line_rounded(f.needle, true, 0);
    lv_obj_set_style_line_color(f.needle, lv_color_hex(kEngMark), 0);
    set_line(f.needle, f.needle_pts, polar(kEngMarkOutR, kStartDeg),
             polar(kEngMarkInR, kStartDeg));
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
    if (!f.seg[0]) return;
    const auto raw = m.st.get("rpm");
    // Eased, then quantised to whole segments. Easing first is what puts a new
    // segment up on the frames between two readings; quantising first would
    // throw those frames away again.
    const auto rpm = ease_reading(f, raw, m);

    // No reading: nothing lit, rather than the scale hidden. An instrument with
    // nothing on it reads as broken, where a dim dial at rest reads as waiting
    // (SPEC.md section 4) -- and every unlit segment still carries its own
    // colour, so the dial is legible before the car has said anything.
    const double v = rpm ? *rpm : 0.0;
    int lit = (f.rpm_max > 0)
        ? static_cast<int>(v / f.rpm_max * kTachoSegs + 0.5) : 0;
    if (lit < 0) lit = 0;
    if (lit > kTachoSegs) lit = kTachoSegs;

    // The whole point of the segments: rpm has to cross a 200 rpm boundary
    // before a single pixel changes. Most readings land here and stop.
    if (lit == f.lit) return;
    const int from = (lit < f.lit) ? lit : f.lit;
    const int to   = (lit < f.lit) ? f.lit : lit;
    f.lit = lit;
    // Only the segments the reading actually crossed. A style write is a
    // repaint whether or not the value changed, so writing "still dim" to the
    // other thirty-odd would repaint the whole rim on every step.
    for (int i = from; i < to; ++i) {
        if (!f.seg[i]) continue;
        lv_obj_set_style_arc_opa(
            f.seg[i], (i < lit) ? static_cast<lv_opa_t>(LV_OPA_COVER)
                                : seg_dim_opa(i, f.rpm_max, f.rpm_red),
            LV_PART_MAIN);
    }
}

// Engine: the mark tracks coolant along the gradient. It is never hidden --
// an instrument with nothing on it reads as broken, where a dim mark parked at
// the cold end reads as waiting (SPEC.md section 4).
void update_engine(Face& f, const Model& m) {
    if (!f.needle) return;
    const auto c = ease_reading(f, m.st.get("coolant"), m);
    set_line_opa(f.needle, f.needle_opa, c ? LV_OPA_COVER : 64);

    const double v = c ? *c : kTempLo;
    int q = static_cast<int>(std::lround(range_angle(v, kTempLo, kTempHi) - kStartDeg));
    if (q < 0) q = 0;
    if (q > kNeedleSteps) q = kNeedleSteps;
    if (q == f.needle_q) return;
    f.needle_q = q;
    set_line(f.needle, f.needle_pts, polar(kEngMarkOutR, kStartDeg + q),
             polar(kEngMarkInR, kStartDeg + q));
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

// Diagnostic: hide the engine view's gradient band. This is how the tacho's
// old 54-segment band was caught costing 210 ms of every full render, back
// when the tacho had one; the engine view still does.
void face_set_band_enabled(bool on) {
    if (!g_band_obj) return;
    if (on) lv_obj_clear_flag(g_band_obj, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(g_band_obj, LV_OBJ_FLAG_HIDDEN);
}

// Diagnostic: hide the whole rev scale. The tacho's dial used to be one arc
// that "DIALS 0" could hide; it is forty boxed segments now, so this is what
// that command reaches for -- see gauge_ui::set_dial_enabled.
void face_set_rev_scale_enabled(bool on) {
    for (lv_obj_t* box : g_tacho_box) {
        if (!box) continue;
        if (on) lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
}

void face_update(Face& f, const Model& m) {
    switch (f.kind) {
        case FaceKind::Tacho:  update_tacho(f, m);  break;
        case FaceKind::Engine: update_engine(f, m); break;
        case FaceKind::Power:  update_power(f, m);  break;
    }
}

}  // namespace gauge_ui
