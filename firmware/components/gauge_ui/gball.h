// The Driving view: the g-ball.
// Design: docs/superpowers/specs/2026-08-29-driving-score-design.md.
//
// The screen is round, so it already is a traction circle. The dot is where
// the car is being pushed this instant -- left and right for cornering, up and
// down for braking -- and a fading trail behind it draws the shape of the
// corner. A good one is an arc. A clumsy one is a scribble, and you see your
// own mistake as a shape rather than as a number that dropped.
//
// The rim carries the pole. Its colour runs deep green through ember to red
// with the score's intensity -- how hard you have been driving over the last
// half-minute, which is what decides whether you are judged as Nice or as
// Spirited. It moves slowly on purpose: the dot is what you just did, the rim
// is how you have been driving, and they are meant to move at different
// speeds.
//
// The simulator says this with a full-screen gradient. The board must not.
// A coloured backdrop covers the panel, so every change to it repaints the
// whole panel -- measured at 5-20 fps on this board, which is why SPEC.md
// section 4 put the tacho's heat on the rim in the first place. A ring is a
// small object, and intensity changes slowly, so the same signal costs
// nothing here.
//
// Like every other view this one allocates nothing per frame. The trail is a
// fixed set of LVGL objects built once and moved -- the rule from the panel
// DMA trap, and undoing it kills the display after the first swipe.
#pragma once
#include "gauge_ui.h"
#include "lvgl.h"

namespace gauge_ui {

// Full-scale g at the rim. The mounted 2026-08-29 drive sat 95% under 0.24 g
// and peaked at 0.56 g braking, so 0.8 keeps ordinary driving visible in the
// middle of the circle instead of pinned to a dot in the centre, and still
// leaves headroom above anything that drive produced.
inline constexpr double kGBallFullG = 0.8;

// Radius of the circle, in pixels on the 466 px panel. Smaller than the
// screen on purpose: at full radius the dot under a hard stop climbs to the
// very top of the panel, straight through the pole word, and a hard stop is
// the one moment you must be able to read.
inline constexpr int kGBallRadius = 130;

// About three seconds of trail at the panel's frame rate.
inline constexpr int kGBallTrail = 24;

void gball_build(lv_obj_t* parent);
void gball_update(const Model& m);

}  // namespace gauge_ui
