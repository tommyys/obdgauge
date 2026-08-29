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
};

void drives_set_source(const DrivesSource* src);

// Build the view's objects under `parent`, and refresh them. Both are called
// with the display lock held, from ui.cpp.
void drives_build(lv_obj_t* parent);
void drives_update();

}  // namespace gauge_ui
