// The tacho's dial face: the heat band, ticks, numbering and needle.
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
// lives on the rim, where it costs nothing; face.cpp explains how.
#pragma once
#include "gauge_ui.h"
#include "lvgl.h"

namespace gauge_ui {

// Which instrument a view wears. Three of the eight views draw a real face;
// the rest get a plain arc or nothing, which ui.cpp handles on its own.
enum class FaceKind {
    Tacho,    // heat band, ticks per 1000 rpm, numbering, needle
    Engine,   // four fixed temperature zones and a white mark
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
    int                 heat       = -1;   // last colour step the needle took
    // Power only: the amber high-water mark, which moves far more rarely than
    // the needle and so is tracked separately.
    lv_obj_t*           peak       = nullptr;
    lv_point_precise_t* peak_pts   = nullptr;
    int                 peak_q     = -1;
};

// The face is built in two halves around the view's own arc, which for a
// tacho is not a fill but a MASK: under() lays down the heat band, the arc
// covers the part of it the engine has not reached yet, and over() puts the
// ticks, numbering and needle on top. Engine and Power use the same split for
// the same reason -- their zones and track have to sit beneath the arc.
void face_build_under(lv_obj_t* root, const gauge::Identity& id, FaceKind kind);
Face face_build_over(lv_obj_t* root, const gauge::Identity& id, FaceKind kind);
void face_update(Face& f, const Model& m);

}  // namespace gauge_ui
