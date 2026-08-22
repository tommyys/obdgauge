#pragma once
// A carousel slide that does not re-render anything.
//
// The obvious implementation -- move two live 466x466 LVGL objects and let LVGL
// redraw -- costs about 52 ms per frame on this panel, because every frame
// re-renders both views through a 466x50 partial buffer, ten bands at a time.
// A 240 ms slide is then four frames and reads as a stutter.
//
// The panel itself is far quicker than that: 466*466*2 bytes over QSPI at the
// 80 MHz this project's vendored BSP sets is 19 ms measured, cache sync
// included (it was 37 ms at the BSP's stock 40 MHz). So the slide renders each view exactly
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

// ---- the same blit path, for anything that owns its own pixels -------------
//
// The boot clip needs exactly what a slide frame needs -- a full-screen RGB565
// buffer on the panel as fast as this hardware allows -- and none of what LVGL
// offers. Playing it through an lv_canvas cost 154 ms a frame and dropped 14 of
// 31 frames; this path costs what a slide frame costs. Exposed here rather than
// reimplemented in main.cpp because the band size, the cache sync and the byte
// order are three separate lessons that took a panel to learn, and there should
// be one copy of each.
//
// begin() takes the panel away from LVGL; end() gives it back and triggers the
// full refresh that lands the real widgets. Call begin ONCE around a whole
// animation, never per frame: end() forces an LVGL redraw.
bool direct_draw_begin(lv_display_t* disp);
void direct_draw_end(lv_display_t* disp);

// Pushes one full-width frame to the panel. `frame` is w*h RGB565 as LVGL and
// ffmpeg produce it (little endian) and is BYTE-SWAPPED IN PLACE, so a buffer
// shown twice must be re-read rather than re-blitted. It must live in DMA-
// reachable memory -- PSRAM is fine, flash-mapped memory is not.
//
// The optional out_* pointers accumulate microseconds, so a caller can report
// where a frame's time actually went instead of guessing.
bool direct_draw_frame(lv_display_t* disp, uint16_t* frame, int w, int h,
                       int64_t* out_swap_us = nullptr,
                       int64_t* out_sync_us = nullptr,
                       int64_t* out_blit_us = nullptr);

}  // namespace gauge_ui
