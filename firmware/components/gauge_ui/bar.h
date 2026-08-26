// The electrical view's voltage bar.
//
// The simulator draws a CSS linear-gradient with five stops and slides a white
// marker along it. LVGL is built here with LV_GRADIENT_MAX_STOPS=2, so the
// gradient is painted by hand instead -- which is no loss, because it makes the
// bar behave like the tacho's heat band: the colours are a property of POSITION
// and are fixed at build time, so a voltage that moves all drive repaints only
// the marker, never the bar.
#pragma once
#include "gauge_ui.h"
#include "lvgl.h"

namespace gauge_ui {

struct Bar {
    lv_obj_t* marker = nullptr;
    int       dy     = 0;    // the bar's own offset from centre, so the marker
                             // can be re-aligned without re-deriving it
    int       x_q    = -1;   // last marker position, in pixels
};

// Builds the bar and its marker under `root`, centred, `dy` below centre.
Bar bar_build(lv_obj_t* root, int dy);

// Moves the marker to `v` on an 11.0-15.5 V scale, or hides it when the car
// reports no voltage at all.
void bar_update(Bar& b, std::optional<double> v);

}  // namespace gauge_ui
