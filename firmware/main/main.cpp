// The boot splash (SPEC.md section 14) followed by the engine-vitals home view.
//
// Section 14's rule is that the instruments are *withheld rather than covered*:
// nothing shows until the splash finishes, which is how a cluster behaves and
// spares you the first second of half-populated channels. So the sequence here
// is WAKING (the clip) -> FADING (a dip to black) -> RUNNING (the gauge), and
// the gauge objects are not even created until the dip is over.
//
// The clip is 31 frames of raw RGB565 in the `assets` partition, paced by the
// wall clock across BOOT_MS. Pacing by time rather than by frame index means a
// panel that cannot keep up drops frames instead of running the animation slow
// -- the splash always lasts 2.5 s, which is what section 14 specifies.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "metrics.h"
#include "replay.h"
#include "state.h"
#include "vehicle.h"
#include "drive_source.h"
#include "gauge_ui.h"
#include "esp_lv_adapter.h"
#include "version.h"

namespace {

// state.py: BOOT_MS = 2500, BOOT_FADE_MS = 600
constexpr int64_t kBootUs = 2500 * 1000;
constexpr int64_t kFadeUs = 600 * 1000;

constexpr int W = BSP_LCD_H_RES;
constexpr int H = BSP_LCD_V_RES;

struct AssetHeader {
    char     magic[4];   // "MX5B"
    uint16_t w, h;
    uint16_t frames;
    uint16_t fps;
    uint32_t reserved;
} __attribute__((packed));

uint16_t* g_fb = nullptr;

// ---- the boot clip -------------------------------------------------------

bool play_boot_clip(lv_display_t* disp, lv_obj_t* scr) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!part) { printf("boot: no assets partition\n"); return false; }

    AssetHeader hdr{};
    if (esp_partition_read(part, 0, &hdr, sizeof hdr) != ESP_OK ||
        memcmp(hdr.magic, "MX5B", 4) != 0) {
        printf("boot: no clip in assets (magic mismatch) -- skipping splash\n");
        return false;
    }
    if (hdr.w != W || hdr.h != H) {
        printf("boot: clip is %ux%u, panel is %dx%d -- skipping\n", hdr.w, hdr.h, W, H);
        return false;
    }
    const size_t frame_bytes = static_cast<size_t>(hdr.w) * hdr.h * 2;
    printf("boot: %u frames %ux%u @%u fps\n", hdr.frames, hdr.w, hdr.h, hdr.fps);

    lv_obj_t* canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, g_fb, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    int64_t t0 = esp_timer_get_time();
    int shown = 0, last = -1;
    for (;;) {
        int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed >= kBootUs) break;
        // Which frame *should* be on screen at this instant.
        int idx = static_cast<int>(elapsed * hdr.frames / kBootUs);
        if (idx >= hdr.frames) idx = hdr.frames - 1;
        if (idx != last) {
            esp_partition_read(part, sizeof(AssetHeader) + static_cast<size_t>(idx) * frame_bytes,
                               g_fb, frame_bytes);
            lv_obj_invalidate(canvas);
            lv_refr_now(disp);
            last = idx;
            ++shown;
        }
    }
    printf("boot: showed %d of %u frames in %.2f s\n", shown, hdr.frames,
           (esp_timer_get_time() - t0) / 1e6);

    // FADING: dip to black. The clip ends on a lit car filling the screen, and
    // cutting straight from that to a dial would read as a glitch rather than a
    // hand-off (section 14).
    lv_obj_delete(canvas);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    int64_t f0 = esp_timer_get_time();
    while (esp_timer_get_time() - f0 < kFadeUs) vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

}  // namespace

extern "C" void app_main(void) {
    printf("\n=== mx5-gauge %s: boot splash + home view ===\n", gauge::core_version());
    g_fb = static_cast<uint16_t*>(heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM));
    if (!g_fb) { printf("FATAL: no PSRAM\n"); return; }

    auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    // Single-buffered, because that is all this panel offers: the adapter only
    // permits tear-avoid NONE or TE_SYNC on a SPI interface, so the double and
    // triple buffering that would overlap render with DMA is unavailable.
    // A full-screen change therefore costs ~52ms (~22ms of 40MHz QSPI transfer
    // plus ~30ms of render), which is the hard ~19fps ceiling.
    lv_display_t* disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    play_boot_clip(disp, scr);
    // Instruments are withheld, not covered: they are created only now.
    gauge_ui::init(scr, id);
    bsp_display_unlock();
    esp_lv_adapter_fps_stats_enable(disp, true);
    int64_t last_fps_log = esp_timer_get_time();
    printf("ui: %d views, starting on %s\n",
           gauge_ui::view_count(), gauge_ui::current_view_name());

    // Replay a real drive out of flash rather than animating a sine wave.
    // Playback runs at 4x, matching the simulator's --replay default, but the
    // metrics are fed the capture's OWN timeline: a sped-up replay must not
    // read as violent acceleration (SPEC.md section 4).
    constexpr double kSpeed = 4.0;

    size_t lib_len = 0;
    const uint8_t* lib = drive_library_map(&lib_len);
    gauge::Replay replay;
    bool have_replay = lib && replay.open(lib, lib_len);
    if (have_replay) {
        printf("replay: %d drives, %d channels, %d records\n",
               replay.drive_count(), replay.channel_count(), replay.total_records());
        printf("replay: playing '%s', %.0f s at %.0fx\n",
               replay.drive_name(0).c_str(), replay.duration_s(), kSpeed);
    } else {
        printf("replay: no drive library -- falling back to a synthetic sweep\n");
    }

    gauge::VehicleState st;
    gauge::Trip trip;
    gauge::DrivingScore score;
    double synth = 45.0;
    bool synth_up = true;
    int64_t t0 = esp_timer_get_time();

    for (;;) {
        if (have_replay) {
            double logical = (esp_timer_get_time() - t0) / 1e6 * kSpeed;
            gauge::ReplaySample smp{};
            while (replay.next(logical, &smp)) {
                st.set(replay.channel_name(smp.chan), smp.value);
                trip.update(smp.t_ms / 1000.0, st.get("speed"), st.get("fuel_rate"));
                score.update(smp.t_ms / 1000.0, st.get("speed"), st.get("rpm"),
                             st.get("throttle"), st.get("fuel_rate"));
            }
            if (replay.finished()) {          // loop the drive
                replay.select(replay.selected());
                st = gauge::VehicleState{};
                trip = gauge::Trip{};
                score = gauge::DrivingScore{};
                t0 = esp_timer_get_time();
            }
        } else {
            st.set("coolant", synth);
            if (synth_up) { synth += 0.5; if (synth >= 96.0) synth_up = false; }
            else          { synth -= 0.5; if (synth <= 45.0) synth_up = true; }
        }

        bsp_display_lock(-1);   // -1 is wait-forever; 0 would be a try-lock
        gauge_ui::Model model{st, trip, score, id};
        gauge_ui::update(model);
        bsp_display_unlock();

        if (esp_timer_get_time() - last_fps_log > 2000000) {
            last_fps_log = esp_timer_get_time();
            uint32_t fps = 0;
            if (esp_lv_adapter_get_fps(disp, &fps) == ESP_OK) {
                printf("ui: %u fps, view %s, gestures %d\n", (unsigned)fps,
                       gauge_ui::current_view_name(), gauge_ui::gesture_count());
                bsp_display_lock(-1);
                gauge_ui::set_fps(fps);
                bsp_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
