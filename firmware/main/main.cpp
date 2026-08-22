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

// ---- the engine-vitals home view (section 6, view 2) ---------------------

lv_obj_t* g_arc   = nullptr;
lv_obj_t* g_value = nullptr;
lv_obj_t* g_state  = nullptr;
lv_obj_t* g_footer = nullptr;

void build_gauge(lv_obj_t* scr, const gauge::Identity& id) {
    g_arc = lv_arc_create(scr);
    lv_obj_set_size(g_arc, 430, 430);
    lv_obj_center(g_arc);
    lv_arc_set_bg_angles(g_arc, 135, 45);
    lv_arc_set_range(g_arc, 40, 110);
    lv_arc_set_value(g_arc, 40);
    lv_obj_remove_style(g_arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(0xFF9500), LV_PART_INDICATOR);

    lv_obj_t* caption = lv_label_create(scr);
    lv_label_set_text(caption, "COOLANT");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_20, 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, -86);

    g_value = lv_label_create(scr);
    lv_label_set_text(g_value, "--");
    lv_obj_set_style_text_color(g_value, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_value, &lv_font_montserrat_48, 0);
    lv_obj_align(g_value, LV_ALIGN_CENTER, 0, -18);

    g_state = lv_label_create(scr);
    lv_label_set_text(g_state, "");
    lv_obj_set_style_text_font(g_state, &lv_font_montserrat_28, 0);
    lv_obj_align(g_state, LV_ALIGN_CENTER, 0, 40);

    g_footer = lv_label_create(scr);
    lv_label_set_text(g_footer, "");
    lv_obj_set_style_text_color(g_footer, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_text_font(g_footer, &lv_font_montserrat_20, 0);
    lv_obj_align(g_footer, LV_ALIGN_CENTER, 0, 152);

    lv_obj_t* banner = lv_label_create(scr);
    lv_label_set_text(banner, id.label.c_str());
    lv_obj_set_style_text_color(banner, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(banner, &lv_font_montserrat_20, 0);
    lv_obj_align(banner, LV_ALIGN_CENTER, 0, 120);
}

void render(const gauge::VehicleState& st) {
    auto coolant = st.get("coolant");
    if (!coolant) { lv_label_set_text(g_value, "--"); lv_label_set_text(g_state, ""); return; }
    int c = static_cast<int>(*coolant);
    lv_label_set_text_fmt(g_value, "%d\xC2\xB0", c);
    lv_arc_set_value(g_arc, c);
    const char* word; uint32_t colour;
    if (c < 60)      { word = "COLD";    colour = 0x4FA3FF; }
    else if (c < 85) { word = "WARMING"; colour = 0xFFC24A; }
    else             { word = "READY";   colour = 0x5BD97A; }
    lv_label_set_text(g_state, word);
    lv_obj_set_style_text_color(g_state, lv_color_hex(colour), 0);
}

}  // namespace

extern "C" void app_main(void) {
    printf("\n=== mx5-gauge %s: boot splash + home view ===\n", gauge::core_version());
    g_fb = static_cast<uint16_t*>(heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM));
    if (!g_fb) { printf("FATAL: no PSRAM\n"); return; }

    auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    lv_display_t* disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    play_boot_clip(disp, scr);
    // Instruments are withheld, not covered: they are created only now.
    build_gauge(scr, id);
    bsp_display_unlock();

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

        char foot[64];
        if (have_replay) {
            double logical = (esp_timer_get_time() - t0) / 1e6 * kSpeed;
            snprintf(foot, sizeof foot, "%.0f/%.0f km  %d:%02d",
                     trip.dist_km, replay.duration_s() / 60.0,
                     static_cast<int>(logical) / 60, static_cast<int>(logical) % 60);
        } else {
            snprintf(foot, sizeof foot, "synthetic");
        }

        bsp_display_lock(0);
        render(st);
        lv_label_set_text(g_footer, foot);
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
