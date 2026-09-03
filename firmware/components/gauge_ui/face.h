// The tacho's dial face: the rev scale, ticks and numbering.
//
// Kept out of ui.cpp because that file is about the carousel -- what a view is,
// how one replaces another, how a swipe is debounced. This is about what the
// tacho looks like, which is a separate question and the only view that asks
// it.
//
// The geometry is the simulator's tacho (mx5gauge/web/index.html) at 466 px.
// The simulator draws into a 260-unit box with the value arc's centre line at
// radius 104; this panel's arc centre line is at 210, so every radius below is
// the simulator's ratio scaled by 210/104, and the colours are copied outright.
//
// The one thing deliberately NOT copied is where the heat goes. The simulator
// warms the whole display as the engine picks up; this panel cannot. Measured
// on the board, a backdrop covering the screen ran at 5-20 fps whichever way it
// was drawn -- a radial gradient, a flat colour, a precomputed alpha mask --
// because it covers the panel, so every change to it repaints the panel, and
// rpm changes constantly. Compositing the frame by hand instead, the trick that
// rescued the boot clip, benched at 65 ms a frame, worse still. So the heat
// lives on the rim, where it costs nothing -- see kScaleSegs below.
#pragma once
#include "ease.h"
#include "gauge_ui.h"
#include "lvgl.h"

namespace gauge_ui {

// ---- one rim geometry, shared by every ring on every view ------------------
// The tacho's rev scale, the engine's temperature zones, the
// power dial's track and fill, and the driving score's ring are all THE SAME
// RING at different colours. They are drawn by three different LVGL paths
// (a custom draw callback, lv_arc backgrounds, an lv_arc indicator), so the
// numbers have to be stated once here rather than repeated at each call site --
// they were, and a one-pixel disagreement between two of them put a red line
// around the whole tacho.
constexpr int kRimPx     = 434;          // outer diameter on the 466 px panel
constexpr int kRimWidth  = 14;           // 13/104 of the simulator's drawing
constexpr int kRimOuterR = kRimPx / 2;   // 217

// ---- the segment scale ------------------------------------------------------
// Neither the tacho nor the engine view draws a bar any more. Each draws a row
// of separate boxes around the rim, lit from the cold end up to the reading --
// the same shape as the page indicator on the bezel, and for the same two
// reasons. The tacho runs blue through amber to red at the rev limit, one
// segment per 200 rpm; the engine view runs blue through green to red across
// 40-120 C, one per 2 C.
//
// It reads better: these are things you glance at, and forty lit blocks say
// "most of the way up" faster than the end of a smooth bar does.
//
// It is also cheaper, in two separate ways.
//
// First, nothing moves. The tacho's rim was a graded band with a SHUTTER arc
// over it, and an lv_arc's object is the whole 452 px square however little of
// it is painted. LVGL decides what to redraw from object rectangles, so the
// rpm digits in the middle of the screen -- which change constantly -- dragged
// the shutter, its black mask and the needle into the walk on every frame. The
// engine view was cheaper but the same shape: a graded band, 377 KB of PSRAM
// under it, and a white mark sliding along it.
// All forty are painted by ONE object per view that skips every segment outside
// the area being repainted, so the digits cost forty rectangle comparisons and
// no drawing. Forty boxed lv_arcs did the same job -- ui.cpp's page indicator
// is built that way -- and benched the same, but eighty objects took LVGL's
// pool to 79% used, and 19 KB free is the figure that hangs swipes. face.cpp
// has the measurements.
//
// Second, most readings now change nothing at all. A segment is lit or it is
// not, so the reading has to cross a boundary before any pixel is different.
// The shutter and the mark moved on almost every sample.
//
// 40 segments, because 200 rpm a step is the resolution a rev counter is
// actually read at, and 270/40 leaves 6.75 degrees a segment -- about 25 px of
// arc at this radius, which is wide enough to tell from its neighbour across a
// 1.4 degree gap. The engine view takes the same count rather than one of its
// own: the two rims are the same ring (see kRimPx), and two views whose
// segments did not line up would read as a mistake on whichever you saw second.
constexpr int kScaleSegs = 40;

// Which instrument a view wears. Three of the eight views draw a real face;
// the rest get a plain arc or nothing, which ui.cpp handles on its own.
enum class FaceKind {
    Tacho,    // a rev scale of lit segments, ticks per 1000 rpm, numbering
    Engine,   // a cold-to-hot scale of lit segments, and nothing else
    Power,    // ticks and kW numbering, fill arc, amber peak mark, needle
};

// A view's moving parts, and the last positions they were drawn at. The
// quantised `_q` fields are what stop a reading that jitters in the last
// decimal from invalidating the needle's bounding box every single frame.
struct Face {
    FaceKind            kind       = FaceKind::Tacho;
    lv_obj_t*           needle     = nullptr;
    lv_point_precise_t* needle_pts = nullptr;
    int                 needle_q   = -1;
    // The opacity each line was last SET to, not what it looks like. LVGL's
    // style setters invalidate the object whether or not the value changed,
    // so writing "still dim" every frame repaints the needle every frame --
    // which is what left the engine view sweeping at 52 fps while the gauge
    // was still looking for the adapter, with nothing feeding it at all.
    int                 needle_opa = -1;
    int                 peak_opa   = -1;
    // Power only: the amber high-water mark, which moves far more rarely than
    // the needle and so is tracked separately.
    lv_obj_t*           peak       = nullptr;
    lv_point_precise_t* peak_pts   = nullptr;
    int                 peak_q     = -1;
    // The drawn reading as it chases the reported one. The car answers about
    // eight times a second; without this the needle moved eight times a second
    // too, however fast the panel was. See gauge_core/ease.h.
    gauge::Ease         ease;
};

// The face is built in two halves around the view's own arc. Power needs the
// split: its track has to sit beneath that arc and its ticks, numbering and
// needle on top of it. The tacho and the engine view have no arc of their own
// left -- the lit segments ARE the reading -- so they build their scale in
// under(), and only the tacho puts anything (ticks and numbering) in over().
void face_build_under(lv_obj_t* root, const gauge::Identity& id, FaceKind kind);
Face face_build_over(lv_obj_t* root, const gauge::Identity& id, FaceKind kind);
void face_update(Face& f, const Model& m);
void face_set_band_enabled(bool on);
void face_set_rev_scale_enabled(bool on);

}  // namespace gauge_ui
