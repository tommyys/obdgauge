// The Drives view: what the gauge itself recorded, read back without a Mac.
// Design: docs/superpowers/specs/2026-08-29-drives-view-design.md.
//
// This file owns the screen only. It never touches flash -- the records live
// in the `logs` ring, reading them blocks, and this code runs on the LVGL
// draw task. Instead the board hands it a DrivesSource, which answers from a
// cache filled on the recorder's own task (main/drives_list.cpp). A row whose
// numbers have not been folded yet says so; it does not wait for them.
#pragma once
#include "drive_stats.h"
#include "lvgl.h"

namespace gauge_ui {

// One line of the list, as the board reports it.
struct DriveRowInfo {
    uint32_t          id        = 0;
    uint32_t          epoch_s   = 0;      // 0 when the clock was unknown
    bool              complete  = true;   // false: power cut, or recording now
    bool              table_ok  = true;   // recorded under this firmware's channel table
    bool              ready     = false;  // stats folded yet?
    gauge::DriveStats stats;
};

// Where the rows come from. Null members are not allowed; a board with no
// recorder installs a source that reports zero drives and a reason.
struct DrivesSource {
    int  (*count)();                            // drives held, newest first
    bool (*row)(int index, DriveRowInfo* out);  // false when index is past the end
    const char* (*empty_note)();                // shown when count() == 0
    // Called from drives_update, so on every frame this view is the one on
    // screen and never otherwise. It is how the board knows it may go to flash:
    // re-reading the ring stalls the whole gauge (an ESP32 flash read turns the
    // cache off, and LVGL's code and buffers are behind that cache), so the
    // board must not do it while some other view is being drawn. Cheap, and
    // called often -- it may do nothing but note the time.
    void (*watching)();
};

void drives_set_source(const DrivesSource* src);

// Build the view's objects under `parent`, and refresh them. Both are called
// with the display lock held, from ui.cpp.
void drives_build(lv_obj_t* parent);
void drives_update();

// Scrolls the list by `dy` pixels, as a thumb would. A bench hook, for the
// same reason SWIPE is one: a scroll is where this view's cost shows up, and
// measuring it should not need a person in front of the glass. False when
// there is no list to scroll. Call with the display lock held.
bool drives_scroll_by(int dy);

// Steps the list by whole rows, in one repaint. Negative moves down the list.
// This view does not scroll pixel by pixel: see drives_build for why.
bool drives_step(int rows);
int  drives_step_rows();
// The scrolling object, for attaching a gesture handler to.
lv_obj_t* drives_list_obj();
// Where the list currently sits, in pixels. -1 before it is built.
int drives_scroll_y();

}  // namespace gauge_ui
