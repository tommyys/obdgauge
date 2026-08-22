// The view carousel (SPEC.md section 6).
//
// gauge_ui may depend on gauge_core; gauge_core may never depend on this. The
// one rule from the design doc: the pure logic stays compilable on the Mac.
#pragma once
#include "lvgl.h"
#include "metrics.h"
#include "state.h"
#include "vehicle.h"

namespace gauge_ui {

// Everything a view is allowed to read. Passed by const reference so a view
// cannot mutate state -- rendering is a pure function of the model.
struct Model {
    const gauge::VehicleState& st;
    const gauge::Trip&         trip;
    const gauge::DrivingScore& score;
    const gauge::Identity&     id;
};

// Build the carousel under `parent`. Call with the display lock held.
void init(lv_obj_t* parent, const gauge::Identity& id);

// Refresh the visible view. Call with the display lock held.
void update(const Model& m);

// Feed touch state so swipes can move between views. Lock held.
void handle_touch(lv_indev_t* indev);

int view_count();
int current_view();
const char* current_view_name();

}  // namespace gauge_ui
