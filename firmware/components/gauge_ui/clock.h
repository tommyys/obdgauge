// The Clock view: setting the gauge's wall clock by hand, in the car.
//
// This board has no real-time clock. The I2C scan names 0x51 as a PCF85063,
// but 0x51 is not on the bus -- the part is not fitted (checked 2026-08-31).
// So the only clock the gauge has is one somebody gave it, kept in NVS, and
// until now the only way to give it one was a Mac on the USB socket. A drive
// recorded without it is stamped "date unknown" and stays that way for ever,
// which has now happened on every drive recorded so far.
//
// Five wheels, the way a phone's alarm does it: date on the top row, time on
// the bottom, each one scrolling under a thumb and snapping to the value in
// the centre band. `lv_roller` is LVGL's own, so the momentum, the snap and
// the band come for free. Vertical drag is unclaimed on this screen -- the
// carousel's swipes are horizontal, and the Drives view already scrolls
// vertically without fighting it.
//
// SET commits. Without it the clock would lurch every time a wheel passed a
// value on its way somewhere else.
#pragma once
#include <cstdint>

#include "lvgl.h"

namespace gauge_ui {

// Where the time comes from and where it goes. The view knows nothing about
// NVS or the recorder -- gauge_ui may not depend on main. Same shape as
// DrivesSource, and installed the same way.
struct ClockSource {
    // The wall clock now, or 0 if nobody has ever set it.
    uint32_t (*now)();
    // The last wall clock a previous run persisted -- effectively the end of
    // the last drive. 0 if there has never been one. This is what the wheels
    // start on, so a drive a day later is a nudge rather than a full entry.
    uint32_t (*floor)();
    // Set the clock. False if it could not be stored.
    bool (*set)(uint32_t epoch_s);
};

void clock_set_source(const ClockSource* src);

// Build the view's objects under `parent`, and refresh them. Both are called
// with the display lock held, from ui.cpp.
void clock_build(lv_obj_t* parent);
void clock_update();

}  // namespace gauge_ui
