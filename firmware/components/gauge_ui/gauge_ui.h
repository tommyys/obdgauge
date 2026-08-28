// The view carousel (SPEC.md section 6).
//
// gauge_ui may depend on gauge_core; gauge_core may never depend on this. The
// one rule from the design doc: the pure logic stays compilable on the Mac.
#pragma once
#include <set>
#include <string>

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
    // The channels this car reports, or nullptr while it is still being
    // identified. Drives the not-available screens -- see gauge::view_available
    // for why nullptr must mean "show everything" rather than "show nothing".
    const std::set<std::string>* supported = nullptr;
    // Seconds since the last update. The instruments ease toward their
    // readings rather than jumping to them (gauge_core/ease.h), and the ease
    // has to know how much time a frame took or it runs faster on a busy
    // screen than on a quiet one.
    double dt_s = 0.0;
};

// Build the carousel under `parent`. Call with the display lock held.
void init(lv_obj_t* parent, const gauge::Identity& id);

// Refresh the visible view. Call with the display lock held.
void update(const Model& m);

// Swipes are handled by LVGL's own gesture detection, registered in init().
// Polling the indev from the app loop missed fast flicks: at ~30Hz a quick
// swipe could begin and end between two polls and never be seen, which is why
// switching worked only sometimes.
const char* slide_note();
bool slide_selftest(int hold_ms);
// Runs a real view change -- the same slide a swipe produces -- without a
// finger on the glass. Kept for the same reason slide_selftest() is: the swipe
// path is where this firmware's memory failures show up, and being able to
// trigger it from the app loop is what let the SPI bounce-buffer exhaustion be
// measured rather than argued about. step is +1 or -1.
void advance_view(int step);

// Queue a view change for the app loop to run on its next pass, which is the
// path a real swipe takes. Same slide, different caller: the console's SWIPE
// uses it to reproduce a gesture exactly, because the two were costing wildly
// different amounts of time and the difference had to be pinned on something.
void queue_view_step(int step);
int gesture_count();
int press_count();
int release_count();

// Dev readout appended to the banner, so the frame rate is visible on the
// device rather than only in a log. Pass 0 to hide it.
void set_fps(uint32_t fps);

// How long an instrument takes to reach a new reading, in milliseconds -- the
// time for the remaining gap to close to about a third. 0 turns easing off, so
// needles jump to each reading the way they did before. Settable from the
// console (EASE) so the two can be compared on the same binary, on the bench,
// without a reflash.
void set_ease_tau_ms(uint32_t ms);
uint32_t ease_tau_ms();

// Diagnostic: hide every rim dial. The arc is 434px across, so it invalidates
// nearly the whole screen whenever it moves -- this isolates that cost.
void set_dial_enabled(bool on);

// Diagnostic: hide the tacho's 54-segment heat band, for the same reason.
void set_band_enabled(bool on);

int view_count();
int current_view();
const char* current_view_name();

}  // namespace gauge_ui
