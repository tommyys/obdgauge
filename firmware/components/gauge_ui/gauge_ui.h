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

// Whether the gauge is still looking for the car. While it is, the line under
// the views is a small sliding bar rather than the car's name: the gauge does
// not know what it is plugged into yet, and a bar that moves says "still
// looking" without a sentence to read. Once the car answers, the bar goes and
// the make and model take its place.
void set_scanning(bool scanning);

// Claims the slide's DMA band buffer now rather than on the first swipe.
// Call it at boot, right after the radio, and before the display starts.
//
// The buffer is 466 * 32 * 2 = 29,824 bytes and it must be DMA-capable
// internal RAM, which is the same scarce pool the BT controller takes a
// contiguous 30,720 bytes from. Allocated lazily it was refused: measured on
// the board 2026-08-28 there were 32,831 bytes free but the largest hole was
// 23,552, so the first swipe would have printed "band buffer UNAVAILABLE" and
// slid nothing. Claimed at boot, while the heap is still whole, it is there
// for the rest of the run -- which is what the comment in slide.cpp always
// intended by "reserved up front".
//
// Returns false if it could not be had, which is a real failure worth logging
// rather than discovering at the first swipe.
bool reserve_slide_band(int width);
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

// A vertical flick on the Drives list, without a finger. Whole rows.
void queue_drives_step(int rows);

// A full-screen message over everything, for the moments when the gauge is
// about to leave the glass for a few seconds -- a button restart, so far. Both
// calls need the display lock, like every other UI write. See note_show() in
// ui.cpp on why it is opaque.
void note_show(const char* text);
void note_hide(void);

// BENCH ONLY: hides the scrims and the bezel scale, so what each of them costs
// during a redraw can be measured instead of argued about.
void chrome_show(bool scrims, bool bezel);
int gesture_count();
int press_count();
int release_count();


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
