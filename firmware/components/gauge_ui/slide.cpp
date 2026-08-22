#include "slide.h"

#include <cstdio>
#include <cstring>

#include "esp_cache.h"
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
constexpr int kBlitRows = 50;

// The make/model banner and the page dots belong to the carousel frame, not to
// any one view, so they must not travel with the content -- a page indicator
// that slides off the screen tells you nothing. Rows inside this band are taken
// straight from the destination snapshot instead of being shifted. The band
// covers the banner (centre +178) and the dots (centre +205) on a 466 panel.
constexpr int kStaticTop = 396;
constexpr int kStaticBot = 452;

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
char g_note[200] = "slide: not run yet";

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

bool slide_run(int dir, void (*flip)(void* ctx), void* ctx) {
    if (!g_ready || dir == 0) { flip(ctx); return false; }
    lv_display_t* disp = lv_obj_get_display(g_screen);

    const int64_t t_snap = esp_timer_get_time();
    const bool got_from =
        lv_snapshot_take_to_draw_buf(g_screen, LV_COLOR_FORMAT_RGB565, &g_from_db) == LV_RESULT_OK;
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
    if (got_from) lv_draw_sw_rgb565_swap(g_from, static_cast<uint32_t>(g_w) * g_h);
    if (got_to)   lv_draw_sw_rgb565_swap(g_to, static_cast<uint32_t>(g_w) * g_h);
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

    // Paced by the wall clock, not by frame index: a slide that cannot keep up
    // must drop frames rather than run long. The travel eases out, so the view
    // arrives rather than stopping dead.
    int frames = 0;
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
        gauge::slide_compose(g_compose, g_from, g_to, g_w, g_h, off, dir,
                             kStaticTop, kStaticBot);
        // The compose buffer lives in PSRAM and was just written by the CPU
        // through a write-back cache; the panel's DMA reads PSRAM directly and
        // sees whatever has actually been written back. The adapter does this
        // for us on the normal LVGL flush path (display_cache_msync_range on
        // the colour map) but NOT in dummy_draw_blit, which hands our pointer
        // straight to esp_lcd_panel_draw_bitmap. Without this the frame is
        // part new pixels and part stale cache lines, which on the panel is
        // bands of garbage and bands that never move.
        esp_err_t serr = esp_cache_msync(g_compose, g_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (serr != ESP_OK && !sync_err) sync_err = serr;
        for (int y = 0; y < g_h; y += kBlitRows) {
            const int hh = (y + kBlitRows <= g_h) ? kBlitRows : (g_h - y);
            esp_err_t berr = esp_lv_adapter_dummy_draw_blit(
                disp, 0, y, g_w, y + hh,
                g_compose + static_cast<size_t>(y) * g_w, true);
            if (berr != ESP_OK && !blit_err) blit_err = berr;
        }
        ++frames;
    }
    const int64_t anim_us = esp_timer_get_time() - t0;

    // Leaving dummy draw triggers a full LVGL refresh, which lands the real
    // widgets exactly where the last slide frame drew them.
    esp_lv_adapter_set_dummy_draw(disp, false);

    // Latched, not just printed: a serial capture that misses the moment of the
    // swipe was losing this every time, which is why the same question kept
    // having to be asked again. main() repeats it on every status line.
    snprintf(g_note, sizeof g_note,
             "slide: snap %lldms %dfr %lldms (%.0ffps) sync=%s blit=%s "
             "from[t %d/%04x b %d/%04x] to[t %d/%04x b %d/%04x]",
             snap_us / 1000, frames, anim_us / 1000,
             frames * 1e6 / static_cast<double>(anim_us ? anim_us : 1),
             esp_err_to_name(sync_err), esp_err_to_name(blit_err),
             fnz_t, fx_t, fnz_b, fx_b, tnz_t, tx_t, tnz_b, tx_b);
    printf("%s\n", g_note);
    return true;
}

const char* slide_note() { return g_note; }

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
