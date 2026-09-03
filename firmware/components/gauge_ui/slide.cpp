#include "slide.h"

#include <cstdio>
#include <cstring>

#include "esp_cache.h"
#include <utility>

#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frame_slide.h"

namespace gauge_ui {
namespace {

constexpr int kSlideMs = 240;

// A full-frame blit fails. Measured on this board with slide_selftest():
//
//   466 rows (434312 B): ESP_ERR_NO_MEM
//   233 rows (217156 B): ESP_ERR_NO_MEM
//   156 rows (145392 B): ESP_OK, and every smaller size
//
// The failure is silent unless the return value is checked -- the panel gets
// its CASET/RASET window and then no data, so it shows a partial frame and
// keeps stale content everywhere else. The boundary is not a driver constant
// but the size of a bounce buffer the SPI driver allocates from internal RAM,
// so the largest size that happens to work today would fail under a more
// fragmented heap. 50 rows is what the adapter's own flush path uses on every
// frame, so it is proven on this hardware and leaves the margin alone.
//
// Cache-line alignment was tried and made no difference: 48-row bands (44736 B,
// a multiple of 64, so aligned in both length and start) measured identically to
// 50-row bands -- 5 frames, 17 fps, 38 ms of blit. So the SPI driver is not
// bouncing misaligned bands through internal RAM; the cost is the ~1.6 ms of
// per-band round-trip, ten times a frame. Left at 50, the value the adapter's
// own flush path uses on every frame.
//
// Dropped 50 -> 16 on 2026-08-27, together with a staging buffer (g_band
// below), because 50 was the number that broke the gauge once BLE was on. The
// note above was right that misaligned bands were not being bounced -- what it
// missed is that a PSRAM *source* is bounced regardless of alignment
// (spi_master.c, setup_priv_desc): the driver allocates a contiguous internal
// buffer the size of the whole band and copies into it, per blit. At 50 rows
// that is a 46 KB allocation ten times a frame, and with BLE holding internal
// RAM there was no 46 KB block to be had -- every blit failed and the slide
// froze the display for good.
constexpr int kBlitRows = 32;

// The make/model banner and the page indicator belong to the carousel frame,
// not to any one view, so they must not travel with the content -- a page
// indicator that slides off the screen tells you nothing. Rows inside this band
// are taken straight from the destination snapshot instead of being shifted.
// The band covers the banner (centre +178) and the indicator arc, which runs
// along the bottom of the bezel and is sized in ui.cpp to fit inside these rows
// for exactly this reason.
constexpr int kStaticTop = 396;
constexpr int kStaticBot = 452;

// Only these rows actually move, and only they are composed and sent each
// frame. Everything outside is painted ONCE at the start of the slide:
//
//   rows 0-16    above the dial arc (434 px centred, so it spans 16..450);
//                black in every view, so nothing to animate
//   rows 396-466 the banner and page indicator, which belong to the frame
//                and must not travel, plus the black sliver below the arc
//
// That is 18% of the panel removed from both the memcpy and the transfer at no
// visual cost. It is not more because the arc is nearly as tall as the screen:
// skipping further would make the top of the ring snap instead of slide.
constexpr int kMoveTop = 16;
constexpr int kMoveBot = 396;

lv_obj_t* g_screen = nullptr;
int g_w = 0;
int g_h = 0;
uint16_t* g_from = nullptr;
uint16_t* g_to = nullptr;
uint16_t* g_compose = nullptr;
size_t g_bytes = 0;
lv_draw_buf_t g_from_db{};
lv_draw_buf_t g_to_db{};
bool g_ready = false;
bool g_prepared = false;      // g_from already holds the on-screen view
uint32_t g_prepared_ms = 0;
int64_t g_prep_us = 0;
char g_note[320] = "slide: not run yet";

// A cheap fingerprint of half a buffer: how many pixels are non-black, and an
// XOR of the data. Two buffers that look the same on screen have the same pair;
// a buffer full of uninitialised PSRAM has a huge non-black count.
void fingerprint(const uint16_t* buf, int y0, int y1, int* nonzero, unsigned* xsum) {
    int nz = 0;
    unsigned x = 0;
    for (int y = y0; y < y1; ++y)
        for (int i = 0; i < g_w; ++i) {
            uint16_t v = buf[static_cast<size_t>(y) * g_w + i];
            if (v) ++nz;
            x ^= v;
        }
    *nonzero = nz;
    *xsum = x;
}

uint16_t* alloc_frame(size_t bytes) {
    return static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM));
}

// One band's worth of DMA-capable internal RAM, allocated once and reused for
// every blit for the rest of the boot. This is the whole point: the copy from
// PSRAM happens either way -- the SPI driver would do it itself -- but doing it
// here means the memory is reserved up front instead of being asked for, and
// refused, mid-slide.
uint16_t* g_band = nullptr;
bool g_band_tried = false;
int64_t g_copy_us = 0, g_send_us = 0;
int g_band_count = 0;

uint16_t* band_buffer(int w) {
    if (!g_band_tried) {
        g_band_tried = true;
        g_band = static_cast<uint16_t*>(heap_caps_aligned_alloc(
            64, static_cast<size_t>(w) * kBlitRows * 2,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        printf("slide: band buffer %s\n", g_band ? "reserved" : "UNAVAILABLE");
    }
    return g_band;
}

}  // namespace

// Declared in gauge_ui.h. Defined here, beside band_buffer, because the point
// of it is *when* that allocation happens.
bool reserve_slide_band(int width) { return band_buffer(width) != nullptr; }

namespace {

// Rows [y0,y1) of a full-width frame, in kBlitRows bands because one blit of
// the whole thing returns ESP_ERR_NO_MEM (see kBlitRows above).
esp_err_t blit_bands(lv_display_t* disp, const uint16_t* frame, int w,
                     int y0, int y1) {
    esp_err_t first = ESP_OK;
    uint16_t* band = band_buffer(w);
    for (int y = y0; y < y1; y += kBlitRows) {
        const int hh = (y + kBlitRows <= y1) ? kBlitRows : (y1 - y);
        const uint16_t* src = frame + static_cast<size_t>(y) * w;
        const int64_t b0 = esp_timer_get_time();
        if (band) {
            memcpy(band, src, static_cast<size_t>(w) * hh * 2);
            src = band;
        }
        const int64_t b1 = esp_timer_get_time();
        esp_err_t e = esp_lv_adapter_dummy_draw_blit(
            disp, 0, y, w, y + hh, src, true);
        g_copy_us += b1 - b0;
        g_send_us += esp_timer_get_time() - b1;
        ++g_band_count;
        if (e != ESP_OK && first == ESP_OK) first = e;
    }
    return first;
}

}  // namespace

bool slide_ready() { return g_ready; }

bool slide_init(lv_obj_t* screen) {
    g_screen = screen;
    g_w = lv_obj_get_width(screen);
    g_h = lv_obj_get_height(screen);
    const uint32_t stride = static_cast<uint32_t>(g_w) * 2;
    const size_t bytes = stride * static_cast<size_t>(g_h);
    g_bytes = bytes;

    g_from = alloc_frame(bytes);
    g_to = alloc_frame(bytes);
    g_compose = alloc_frame(bytes);
    if (!g_from || !g_to || !g_compose) {
        printf("slide: no PSRAM for %dx%d buffers (3 x %u B) -- cutting instantly\n",
               g_w, g_h, (unsigned)bytes);
        heap_caps_free(g_from);
        heap_caps_free(g_to);
        heap_caps_free(g_compose);
        g_from = g_to = g_compose = nullptr;
        return false;
    }
    if (lv_draw_buf_init(&g_from_db, g_w, g_h, LV_COLOR_FORMAT_RGB565, stride,
                         g_from, bytes) != LV_RESULT_OK ||
        lv_draw_buf_init(&g_to_db, g_w, g_h, LV_COLOR_FORMAT_RGB565, stride,
                         g_to, bytes) != LV_RESULT_OK) {
        printf("slide: lv_draw_buf_init rejected %dx%d -- cutting instantly\n", g_w, g_h);
        return false;
    }
    g_ready = true;
    printf("slide: ready, 3 x %u B in PSRAM, %d ms slide\n", (unsigned)bytes, kSlideMs);
    return true;
}

// Called the moment a finger lands. The outgoing view is whatever is on screen
// now, so its snapshot can be taken here -- while the finger is still travelling
// towards the swipe threshold -- instead of after the gesture fires. That is
// half the dead time before the slide starts moving, and it costs nothing at
// rest because nothing in this UI responds to a tap.
void slide_prepare() {
    if (!g_ready) return;
    const int64_t t0 = esp_timer_get_time();
    if (lv_snapshot_take_to_draw_buf(g_screen, LV_COLOR_FORMAT_RGB565, &g_from_db) != LV_RESULT_OK)
        return;
    lv_draw_sw_rgb565_swap(g_from, static_cast<uint32_t>(g_w) * g_h);
    g_prep_us = esp_timer_get_time() - t0;
    g_prepared = true;
    g_prepared_ms = lv_tick_get();
}

bool slide_run(int dir, void (*flip)(void* ctx), void* ctx) {
    if (!g_ready || dir == 0) { flip(ctx); return false; }
    lv_display_t* disp = lv_obj_get_display(g_screen);

    const int64_t t_snap = esp_timer_get_time();
    // Reuse the press-time snapshot, but only while it is fresh: a stale one
    // would make the outgoing view jump to old readings just as it starts to
    // move, which is more jarring than the delay it saves.
    bool got_from = g_prepared && (lv_tick_get() - g_prepared_ms) < 1200;
    const bool carried = got_from;
    int64_t prep_us = got_from ? g_prep_us : 0;
    if (!got_from) {
        got_from = lv_snapshot_take_to_draw_buf(
                       g_screen, LV_COLOR_FORMAT_RGB565, &g_from_db) == LV_RESULT_OK;
        if (got_from) lv_draw_sw_rgb565_swap(g_from, static_cast<uint32_t>(g_w) * g_h);
    }
    g_prepared = false;
    flip(ctx);
    const bool got_to =
        lv_snapshot_take_to_draw_buf(g_screen, LV_COLOR_FORMAT_RGB565, &g_to_db) == LV_RESULT_OK;
    // The panel takes RGB565 big-endian; LVGL renders little-endian. The
    // adapter swaps on our behalf in its flush path -- guarded on exactly this
    // panel interface and colour format (lvgl_bridge_v9.c) -- but
    // dummy_draw_blit bypasses all of that, so an unswapped frame reaches the
    // panel with every pixel's bytes reversed. Swapping the two SNAPSHOTS once
    // each, rather than the composed frame every time, keeps the per-frame path
    // a pure memcpy: the cost is paid twice per slide instead of twice per frame.
    if (got_to) lv_draw_sw_rgb565_swap(g_to, static_cast<uint32_t>(g_w) * g_h);
    const int64_t snap_us = esp_timer_get_time() - t_snap;
    if (!got_from || !got_to) {
        printf("slide: snapshot failed (%d/%d) -- view cut instead\n", got_from, got_to);
        return false;
    }

    // Which side of the boundary is the corruption on? These fingerprints are
    // taken from OUR buffers, before the panel ever sees them. If the top half
    // of a snapshot is already wrong here, the snapshot is at fault; if the
    // buffers are sane and the panel is not, the fault is in the blit or DMA.
    int fnz_t = 0, fnz_b = 0, tnz_t = 0, tnz_b = 0;
    unsigned fx_t = 0, fx_b = 0, tx_t = 0, tx_b = 0;
    fingerprint(g_from, 0, g_h / 2, &fnz_t, &fx_t);
    fingerprint(g_from, g_h / 2, g_h, &fnz_b, &fx_b);
    fingerprint(g_to, 0, g_h / 2, &tnz_t, &tx_t);
    fingerprint(g_to, g_h / 2, g_h, &tnz_b, &tx_b);

    if (esp_lv_adapter_set_dummy_draw(disp, true) != ESP_OK) {
        printf("slide: dummy draw unavailable -- view cut instead\n");
        return false;
    }

    // The rows that never move, painted once. From the DESTINATION snapshot, so
    // the page indicator has already stepped when the slide starts moving.
    {
        // Banded rather than sent whole: these two were the last blits still
        // asking the SPI driver for a bounce buffer the size of the region,
        // and they are the one failure per slide that survived every other
        // fix here -- the ESP_ERR_NO_MEM this file's comments have recorded
        // as unavoidable since the slide was written. It was not unavoidable;
        // it was the band size.
        esp_err_t e1 = blit_bands(disp, g_to, g_w, 0, kMoveTop);
        esp_err_t e2 = blit_bands(disp, g_to, g_w, kMoveBot, g_h);
        if (e1 != ESP_OK || e2 != ESP_OK) printf("slide: fixed rows failed\n");
    }

    // Paced by the wall clock, not by frame index: a slide that cannot keep up
    // must drop frames rather than run long. The travel eases out, so the view
    // arrives rather than stopping dead.
    int frames = 0;
    g_copy_us = g_send_us = 0;
    g_band_count = 0;
    int64_t compose_us = 0, sync_us = 0, blit_us = 0;
    esp_err_t blit_err = ESP_OK;
    esp_err_t sync_err = ESP_OK;
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        const int64_t el = esp_timer_get_time() - t0;
        if (el >= kSlideMs * 1000) break;
        const double p = static_cast<double>(el) / (kSlideMs * 1000.0);
        const double q = 1.0 - p;
        const double eased = 1.0 - q * q * q;
        int off = static_cast<int>(eased * g_w + 0.5);
        if (off < 1) off = 1;
        if (off > g_w) off = g_w;
        const int64_t c0 = esp_timer_get_time();
        // Composed a band at a time, straight into the DMA buffer the panel
        // will read from -- see the loop below. The old shape (compose the
        // whole moving region into a PSRAM scratch, flush the cache over it,
        // then copy each band into the DMA buffer) walked 350 KB across the
        // PSRAM bus three times per frame to put the same pixels on screen.
        uint16_t* band = band_buffer(g_w);
        const int64_t c1 = esp_timer_get_time();
        compose_us += c1 - c0;
        // The cache flush that used to sit here is gone with the PSRAM scratch
        // it existed for: that buffer was written through a write-back cache,
        // and dummy_draw_blit hands our pointer straight to
        // esp_lcd_panel_draw_bitmap without the msync the adapter does on its
        // own flush path -- so without it the panel showed bands of stale
        // pixels. The band buffer is internal RAM, read coherently by DMA.
        const int64_t c2 = esp_timer_get_time();
        sync_us += c2 - c1;
        esp_err_t berr = ESP_OK;
        for (int y = kMoveTop; y < kMoveBot; y += kBlitRows) {
            const int hh = (y + kBlitRows <= kMoveBot) ? kBlitRows : (kMoveBot - y);
            const size_t off_px = static_cast<size_t>(y) * g_w;
            const int64_t k0 = esp_timer_get_time();
            uint16_t* dst = band ? band : g_compose + off_px;
            gauge::slide_compose(dst, g_from + off_px, g_to + off_px,
                                 g_w, hh, off, dir,
                                 0, 0);   // no static band inside the moving rows
            const int64_t k1 = esp_timer_get_time();
            g_copy_us += k1 - k0;
            esp_err_t e = esp_lv_adapter_dummy_draw_blit(disp, 0, y, g_w, y + hh,
                                                         dst, true);
            g_send_us += esp_timer_get_time() - k1;
            ++g_band_count;
            if (e != ESP_OK && berr == ESP_OK) berr = e;
        }
        if (berr != ESP_OK && !blit_err) blit_err = berr;
        blit_us += esp_timer_get_time() - c2;
        ++frames;
    }
    const int64_t anim_us = esp_timer_get_time() - t0;

    // Leaving dummy draw triggers a full LVGL refresh, which lands the real
    // widgets exactly where the last slide frame drew them.
    esp_lv_adapter_set_dummy_draw(disp, false);

    // The view that just arrived is the one the NEXT swipe will slide away
    // from, and it is already drawn: g_to holds it, byte-swapped and all.
    // Handing it over costs a pointer swap and saves that swipe a full-screen
    // render -- 160 ms of the 320 ms dead pause before a slide starts moving,
    // and it lands where the pause is felt most, on a run of quick swipes
    // through the carousel.
    //
    // This is the way round that WORKS. Rendering the outgoing view when the
    // finger lands is the obvious alternative and it breaks the gauge: the
    // render holds the display lock for 160 ms, the touchscreen is not sampled
    // while it runs, and LVGL then never sees the finger travel that tells it
    // a swipe happened. Measured on the board -- gestures went to zero.
    //
    // The freshness rule above throws this away once the readings have had
    // time to move on, so a swipe minutes later still renders the real thing.
    std::swap(g_from, g_to);
    std::swap(g_from_db, g_to_db);
    g_prepared = true;
    g_prepared_ms = lv_tick_get();
    g_prep_us = 0;

    // Latched, not just printed: a serial capture that misses the moment of the
    // swipe was losing this every time, which is why the same question kept
    // having to be asked again. main() repeats it on every status line.
    snprintf(g_note, sizeof g_note,
             "slide: snap %lldms(from %s, prep %lldms) %dfr %lldms (%.0ffps) "
             "per-frame blit %lldms (compose %lldus send %lldus over %d bands) | %s %s "
             "from[t %d/%04x b %d/%04x] to[t %d/%04x b %d/%04x]",
             snap_us / 1000, carried ? "carried" : "rendered", prep_us / 1000,
             frames, anim_us / 1000,
             frames * 1e6 / static_cast<double>(anim_us ? anim_us : 1),
             frames ? blit_us / frames / 1000 : 0,
             frames ? g_copy_us / frames : 0, frames ? g_send_us / frames : 0,
             frames ? g_band_count / frames : 0,
             esp_err_to_name(sync_err), esp_err_to_name(blit_err),
             fnz_t, fx_t, fnz_b, fx_b, tnz_t, tx_t, tnz_b, tx_b);
    printf("%s\n", g_note);
    return true;
}

const char* slide_note() { return g_note; }

bool direct_draw_begin(lv_display_t* disp) {
    return esp_lv_adapter_set_dummy_draw(disp, true) == ESP_OK;
}

void direct_draw_end(lv_display_t* disp) {
    esp_lv_adapter_set_dummy_draw(disp, false);
}

bool direct_draw_frame(lv_display_t* disp, uint16_t* frame, int w, int h,
                       bool pixels_big_endian,
                       int64_t* out_swap_us, int64_t* out_sync_us,
                       int64_t* out_blit_us) {
    const int64_t t0 = esp_timer_get_time();
    // The panel takes RGB565 big-endian and this path bypasses the adapter's
    // flush, which is where the swap would otherwise happen (see slide_run).
    // A caller whose pixels are already in that order pays nothing here.
    if (!pixels_big_endian)
        lv_draw_sw_rgb565_swap(frame, static_cast<uint32_t>(w) * h);
    const int64_t t1 = esp_timer_get_time();
    // The CPU just wrote this through a write-back cache; the panel's DMA reads
    // PSRAM directly and would otherwise see stale lines (see slide_run).
    esp_err_t serr = esp_cache_msync(frame, static_cast<size_t>(w) * h * 2,
                                     ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                     ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    const int64_t t2 = esp_timer_get_time();
    esp_err_t berr = blit_bands(disp, frame, w, 0, h);
    const int64_t t3 = esp_timer_get_time();
    if (out_swap_us) *out_swap_us += t1 - t0;
    if (out_sync_us) *out_sync_us += t2 - t1;
    if (out_blit_us) *out_blit_us += t3 - t2;
    return serr == ESP_OK && berr == ESP_OK;
}

bool slide_selftest(int hold_ms) {
    if (!g_ready) return false;
    lv_display_t* disp = lv_obj_get_display(g_screen);
    // Four solid bands written by the CPU and pushed through exactly the path a
    // slide frame takes. Nothing here comes from LVGL or from a snapshot, so
    // what the panel shows is a verdict on the blit alone: correct bands mean
    // the transfer is sound and the corruption is upstream, in the snapshots.
    static const uint16_t band[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};  // R G B W
    for (int y = 0; y < g_h; ++y) {
        uint16_t v = band[(y * 4) / g_h];
        uint16_t* row = g_compose + static_cast<size_t>(y) * g_w;
        for (int x = 0; x < g_w; ++x) row[x] = v;
    }
    lv_draw_sw_rgb565_swap(g_compose, static_cast<uint32_t>(g_w) * g_h);
    if (esp_lv_adapter_set_dummy_draw(disp, true) != ESP_OK) {
        printf("selftest: dummy draw unavailable\n");
        return false;
    }
    esp_err_t serr = esp_cache_msync(g_compose, g_bytes,
                                     ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                     ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // How many rows can go in ONE blit? A full frame returned ESP_ERR_NO_MEM,
    // and the answer decides the whole design of the slide loop, so measure it
    // rather than reason about the SPI driver's bounce-buffer rules.
    printf("selftest: sync=%s. bands top-to-bottom are RED GREEN BLUE WHITE;\n"
           "          if they read BLUE RED GREEN WHITE the byte order is swapped\n",
           esp_err_to_name(serr));
    const int best = kBlitRows;
    // Then paint the whole screen in bands of that size, so the panel shows
    // whether banded output is clean end to end.
    esp_err_t berr = ESP_OK;
    if (best > 0) {
        for (int y = 0; y < g_h; y += best) {
            const int hh = (y + best <= g_h) ? best : (g_h - y);
            esp_err_t e = esp_lv_adapter_dummy_draw_blit(
                disp, 0, y, g_w, y + hh,
                g_compose + static_cast<size_t>(y) * g_w, true);
            if (e != ESP_OK && berr == ESP_OK) berr = e;
        }
    }
    printf("selftest: R/G/B/W bands painted, blit=%s -- holding %d ms\n",
           esp_err_to_name(berr), hold_ms);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    esp_lv_adapter_set_dummy_draw(disp, false);
    return berr == ESP_OK;
}

}  // namespace gauge_ui
