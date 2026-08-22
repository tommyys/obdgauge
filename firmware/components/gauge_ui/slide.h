#pragma once
// A carousel slide that does not re-render anything.
//
// The obvious implementation -- move two live 466x466 LVGL objects and let LVGL
// redraw -- costs about 52 ms per frame on this panel, because every frame
// re-renders both views through a 466x50 partial buffer, ten bands at a time.
// A 240 ms slide is then four frames and reads as a stutter.
//
// The panel itself is far quicker than that: 466*466*2 bytes over QSPI at
// 40 MHz on four lanes is about 22 ms. So the slide renders each view exactly
// ONCE into a buffer (lv_snapshot), then every frame is a memcpy of two
// rectangles plus a direct blit past LVGL (esp_lv_adapter_dummy_draw_blit,
// which unlike the framebuffer API works with tear-avoid NONE -- the only mode
// this SPI panel accepts).
#include "lvgl.h"

namespace gauge_ui {

// Allocates the 466x466 RGB565 buffers the slide needs (~1.3 MB of PSRAM).
// Returns false if they do not fit, in which case the carousel keeps cutting
// instantly; a slide is a nicety and must never be the reason a view is stuck.
bool slide_init(lv_obj_t* screen);
bool slide_ready();

// Slides `dir` = +1 (incoming view enters from the right) or -1 (from the left).
//
// `flip` is called between the two snapshots and must make the target view the
// visible one in LVGL. It is ALWAYS called, even when the slide cannot run, so
// the caller never has to flip as well. The return value reports only whether
// the animation played.
bool slide_run(int dir, void (*flip)(void* ctx), void* ctx);

// Call when a finger lands: snapshots the on-screen view early, so the slide
// does not have to render it after the gesture fires.
void slide_prepare();

// The last slide's timings and buffer fingerprints, latched so a serial capture
// that misses the swipe itself can still read them.
const char* slide_note();

// Pushes four solid colour bands through the blit path and holds them, so the
// panel itself answers whether the transfer is sound independently of anything
// LVGL rendered.
bool slide_selftest(int hold_ms);

}  // namespace gauge_ui
