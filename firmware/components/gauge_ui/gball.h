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

// Full-scale g at the rim.
//
// 1.0, not the 0.8 it was. 0.8 came off the 2026-08-29 drive, which peaked at
// 0.56 g -- but the mountain drive of 2026-09-06 cornered at 0.77 g, which
// left the dot sitting just inside a rim it could never reach, and the
// over-scale signal never fired once in 68 minutes. 1.0 g is also the number
// worth teaching: it is roughly where this car's road tyres let go, so the
// rim is the car's limit rather than an arbitrary mark, and each ring below
// is a fifth of it.
inline constexpr double kGBallFullG = 1.0;

// Radius of the dot's travel, in pixels on the 466 px panel: the centre line
// of face.h's shared rim (kRimOuterR - kRimWidth / 2 = 210), so this circle
// is THE SAME RING as the tacho's rev scale and the engine view's heat scale.
// It was 130 -- barely half the glass -- and Tommy could not read the view at
// a glance because of it (2026-09-06). Keep it equal to that expression: two
// views whose rims differ by a few pixels read as a mistake on whichever you
// see second.
inline constexpr int kGBallRadius = 210;

// The trail: how many dots, and how often one is laid down.
//
// **The old comment here said "about three seconds" and it was wrong.** The
// dots were pushed one per call to gball_update, which main.cpp runs every
// 16 ms -- so 24 dots was 0.4 SECONDS, barely a smear behind the dot, and
// Tommy asked for a longer one on the first drive he watched it (2026-09-06).
//
// Three seconds at 62 Hz would be 190 objects, which is out of the question:
// every trail dot is an LVGL object, and the frame rate falls off almost
// linearly with how many of them are on screen. Measured 2026-09-06 at the
// desk, worst case with every dot stacked on one spot:
//
//   24 dots -> 49 fps     32 dots -> 48 fps     48 dots -> 33 fps
//
// So the trail is DECIMATED instead: one dot laid down every
// kGBallTrailEveryN updates. **Duration is bought with the divider, not with
// objects** -- 32 x 6 x 16 ms is the same 3.1 seconds that 48 x 4 gives, at
// 48 fps instead of 33. Anything that wants a longer trail should raise the
// divider first and the count only if the dots become too far apart (they are
// 96 ms apart here).
//
// It is also cheaper per frame than what it replaced: nothing on the trail
// moves on five frames in six.
inline constexpr int kGBallTrail      = 32;
inline constexpr int kGBallTrailEveryN = 6;

void gball_build(lv_obj_t* parent);
void gball_update(const Model& m);

}  // namespace gauge_ui
