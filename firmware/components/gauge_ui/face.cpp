#include "face.h"

#include <cmath>
#include <cstdio>

#include "glow.h"


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
constexpr double kNeedleR    = kArcR * (86.0 / 104.0);
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
constexpr uint32_t kRedline   = 0xFF3B30;

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
constexpr int kArcOuterR = 217;    // the 434 px band's outer edge
constexpr int kArcWidth  = 14;
constexpr uint32_t kTick      = 0x7D818B;
constexpr uint32_t kTickHot   = 0xFF5B52;
constexpr uint32_t kNumber    = 0xC9CCD4;
constexpr uint32_t kNeedle    = 0xE1000A;
constexpr uint32_t kHubFill   = 0x1A1C22;

// The engine view's four temperature zones, copied from the simulator with
// its alpha folded into the colour: LVGL would charge for a translucent arc
// over the track, and the track is a known flat colour, so the blend is done
// once here at build time instead of every repaint.
struct TempZone { double lo, hi; uint32_t colour; double alpha; };
constexpr TempZone kTempZones[] = {
    { 40,  60, 0x4D96FF, 0.50},   // cold
    { 60,  80, 0xFFC53D, 0.40},   // warming
    { 80, 105, 0x35E06B, 0.55},   // ready
    {105, 120, 0xFF3B30, 0.50},   // hot
};
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

// The band, coloured once. Held here rather than on the object because there
// is exactly one dial face and the draw callback has to be cheap.
struct BandSeg { int16_t a0, a1; uint32_t colour; };
BandSeg g_band[kHeatBands];
int     g_band_n = 0;

bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

// Paints the whole heat band. One object, so LVGL has one thing to walk no
// matter how finely the ramp is divided; the per-segment skip below is what
// keeps a small redraw small.
void band_draw_cb(lv_event_t* e) {
    lv_obj_t*   obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t   coords;
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = (coords.x1 + coords.x2) / 2;
    const int32_t cy = (coords.y1 + coords.y2) / 2;

    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.width    = kArcWidth;
    d.radius   = kArcOuterR;
    d.opa      = LV_OPA_COVER;
    // Square ends, butted together. Rounded ones scalloped every join, which
    // was half of why the old band looked stepped rather than graded.
    d.rounded  = 0;

    for (int i = 0; i < g_band_n; ++i) {
        lv_area_t a;
        lv_draw_arc_get_area(cx, cy, d.radius, g_band[i].a0, g_band[i].a1,
                             d.width, false, &a);
        if (!areas_overlap(a, layer->_clip_area)) continue;
        d.color       = lv_color_hex(g_band[i].colour);
        d.start_angle = g_band[i].a0;
        d.end_angle   = g_band[i].a1;
        lv_draw_arc(layer, &d);
    }
}

lv_obj_t* mk_rim_arc(lv_obj_t* root, int a0, int a1, uint32_t colour, lv_opa_t opa) {
    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_set_size(arc, 434, 434);
    lv_obj_center(arc);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_style(arc, nullptr, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(arc, a0, a1);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    return arc;
}

}  // namespace

// ---- the dial face -------------------------------------------------------

void build_under_tacho(lv_obj_t* root, const gauge::Identity& id) {
    // The rim carries the heat, because the screen cannot.
    //
    // The simulator warms the whole display as the engine picks up. Measured on
    // this board that costs 5-20 fps however it is drawn -- a backdrop covers
    // the panel, so every change to it repaints the panel, and rpm changes
    // constantly. Painting the frame by hand instead benched at 15 fps, worse
    // still.
    //
    // The rim does the same job for nothing, by making the colour a property of
    // POSITION on the dial rather than of time. The band is coloured once, cool
    // through ember to red, using the same gauge::glow_for ramp the simulator's
    // backdrop uses; rpm then only decides how much of it you can see. Nothing
    // ever changes colour, so nothing ever costs a repaint -- and what you read
    // at a glance, the dial getting hotter as you rev, is the same.
    g_band_n = 0;
    for (int i = 0; i < kHeatBands; ++i) {
        const double f0 = static_cast<double>(i) / kHeatBands;
        const double f1 = static_cast<double>(i + 1) / kHeatBands;
        // Read at the middle of the segment, and at full precision: the band
        // never repaints, so it has nothing to gain from the stepped form and
        // everything to lose -- the steps were the thing that looked wrong.
        const double h = gauge::glow_heat((f0 + f1) / 2 * id.rpm_max, id.rpm_red);
        const uint32_t hot = mix(gauge::kGlowEmber, gauge::kGlowRed, h);
        g_band[g_band_n].colour = mix(kTrack, hot, h * gauge::kGlowMaxOpa);
        g_band[g_band_n].a0 = static_cast<int16_t>(
            (static_cast<int>(kStartDeg) + i * kBandDeg) % 360);
        // One degree of overlap onto the next segment. Arcs that merely abut
        // leave a hairline of background between them the whole way round.
        g_band[g_band_n].a1 = static_cast<int16_t>(
            (static_cast<int>(kStartDeg) + (i + 1) * kBandDeg + 1) % 360);
        ++g_band_n;
    }
    lv_obj_t* band = lv_obj_create(root);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, 434, 434);
    lv_obj_center(band);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(band, band_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    // Past the redline the band is unambiguous rather than merely hot, so the
    // top of the dial still reads as a limit and not just as more colour.
    if (id.rpm_red > 0 && id.rpm_max > id.rpm_red) {
        const int a0 = static_cast<int>(std::lround(dial_angle(id.rpm_red, id.rpm_max))) % 360;
        mk_rim_arc(root, a0, 45, kRedline, LV_OPA_COVER);
    }
}

// The engine view's zones. Same idea as the tacho's heat band and for the same
// reason: colour is a property of POSITION on the dial, fixed at build time, so
// a coolant reading that climbs all drive never repaints a single zone. Only
// the mark moves.
void build_under_engine(lv_obj_t* root) {
    mk_rim_arc(root, static_cast<int>(kStartDeg), 45, kTrack, LV_OPA_COVER);
    for (const TempZone& z : kTempZones) {
        const int a0 = static_cast<int>(std::lround(range_angle(z.lo, kTempLo, kTempHi))) % 360;
        const int a1 = static_cast<int>(std::lround(range_angle(z.hi, kTempLo, kTempHi))) % 360;
        mk_rim_arc(root, a0, a1, mix(kTrack, z.colour, z.alpha), LV_OPA_COVER);
    }
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

    Face f;
    f.needle_pts = new lv_point_precise_t[2];
    f.needle = lv_line_create(root);
    lv_obj_set_style_line_width(f.needle, 7, 0);
    lv_obj_set_style_line_rounded(f.needle, true, 0);
    lv_obj_set_style_line_color(f.needle, lv_color_hex(kNeedle), 0);
    set_line(f.needle, f.needle_pts, {static_cast<lv_value_precise_t>(kCx),
                                      static_cast<lv_value_precise_t>(kCy)},
             polar(kNeedleR, kStartDeg));

    // The hub last, so the needle's blunt end is covered by it rather than
    // ending in mid-air at the centre of the screen.
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

void update_tacho(Face& f, const Model& m) {
    if (!f.needle) return;
    const auto rpm = m.st.get("rpm");

    // Recoloured, unlike the band, because a needle is small: the area this
    // invalidates is a fraction of the screen, and the needle is being
    // invalidated by its own movement in the same frame regardless.
    const gauge::Glow g = gauge::glow_for(rpm, m.id.rpm_red);
    if (g.step != f.heat) {
        f.heat = g.step;
        lv_obj_set_style_line_color(
            f.needle, lv_color_hex(mix(kNeedle, gauge::kGlowRed, g.opa / 255.0)), 0);
    }

    // No reading: the needle stays parked at zero and dims, rather than being
    // hidden -- an instrument with no needle at all reads as broken, where a
    // dim one at rest reads as waiting (SPEC.md section 4).
    lv_obj_set_style_line_opa(f.needle, rpm ? LV_OPA_COVER : 64, 0);

    const double v = rpm ? *rpm : 0.0;
    int q = static_cast<int>(std::lround(dial_angle(v, m.id.rpm_max) - kStartDeg));
    if (q < 0) q = 0;
    if (q > kNeedleSteps) q = kNeedleSteps;
    if (q == f.needle_q) return;
    f.needle_q = q;
    set_line(f.needle, f.needle_pts,
             {static_cast<lv_value_precise_t>(kCx), static_cast<lv_value_precise_t>(kCy)},
             polar(kNeedleR, kStartDeg + q));
}

// Engine: the mark tracks coolant across the four zones. It is never hidden --
// an instrument with nothing on it reads as broken, where a dim mark parked at
// the cold end reads as waiting (SPEC.md section 4).
void update_engine(Face& f, const Model& m) {
    if (!f.needle) return;
    const auto c = m.st.get("coolant");
    lv_obj_set_style_line_opa(f.needle, c ? LV_OPA_COVER : 64, 0);

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
    const auto kw = m.st.get("power_kw");
    lv_obj_set_style_line_opa(f.needle, kw ? LV_OPA_COVER : 64, 0);

    const double peak = m.st.peak_kw();
    if (f.peak) {
        lv_obj_set_style_line_opa(f.peak, (kw && peak > 0) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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

void face_update(Face& f, const Model& m) {
    switch (f.kind) {
        case FaceKind::Tacho:  update_tacho(f, m);  break;
        case FaceKind::Engine: update_engine(f, m); break;
        case FaceKind::Power:  update_power(f, m);  break;
    }
}

}  // namespace gauge_ui
