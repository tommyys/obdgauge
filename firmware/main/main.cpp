// Phase 0 bring-up: the engine-vitals home view (SPEC.md section 6, view 2)
// on the real panel, driven by gauge_core.
//
// Coolant is the hero, not oil, because that is what the car actually reports
// (SPEC.md section 4). The centre stays pure black, which is the same
// legibility reasoning that picked glow=rim for the rpm backdrop in section 11.
#include <cstdio>
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "metrics.h"
#include "parse.h"
#include "pid.h"
#include "poll.h"
#include "state.h"
#include "vehicle.h"
#include "version.h"

namespace {

lv_obj_t* g_arc      = nullptr;
lv_obj_t* g_value    = nullptr;
lv_obj_t* g_state    = nullptr;
lv_obj_t* g_banner   = nullptr;

// Coolant dial range. 40-110 C covers the 72->95 warm-up the logs show
// (SPEC.md section 4) with headroom at both ends.
constexpr int kCoolantLo = 40;
constexpr int kCoolantHi = 110;

void build_ui(const gauge::Identity& id) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    g_arc = lv_arc_create(scr);
    lv_obj_set_size(g_arc, 430, 430);
    lv_obj_center(g_arc);
    lv_arc_set_bg_angles(g_arc, 135, 45);          // 270 degrees, gap at the bottom
    lv_arc_set_range(g_arc, kCoolantLo, kCoolantHi);
    lv_arc_set_value(g_arc, kCoolantLo);
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

    // The make/model banner from the universal layer (SPEC.md section 10).
    g_banner = lv_label_create(scr);
    lv_label_set_text(g_banner, id.label.c_str());
    lv_obj_set_style_text_color(g_banner, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(g_banner, &lv_font_montserrat_20, 0);
    lv_obj_align(g_banner, LV_ALIGN_CENTER, 0, 120);
}

// Views degrade honestly: '--' for a channel the car is not reporting, never a
// plausible-looking zero (SPEC.md section 4).
void render(const gauge::VehicleState& st) {
    auto coolant = st.get("coolant");
    if (!coolant) {
        lv_label_set_text(g_value, "--");
        lv_label_set_text(g_state, "");
        return;
    }
    int c = static_cast<int>(*coolant);
    lv_label_set_text_fmt(g_value, "%d\xC2\xB0", c);
    lv_arc_set_value(g_arc, c);

    const char* word;
    uint32_t colour;
    if (c < 60)       { word = "COLD";    colour = 0x4FA3FF; }
    else if (c < 85)  { word = "WARMING"; colour = 0xFFC24A; }
    else              { word = "READY";   colour = 0x5BD97A; }
    lv_label_set_text(g_state, word);
    lv_obj_set_style_text_color(g_state, lv_color_hex(colour), 0);
}

void core_selfcheck() {
    int pass = 0, fail = 0;
    auto expect = [&](const char* what, bool ok, const char* got) {
        if (ok) ++pass; else ++fail;
        printf("  %-34s %s  %s\n", what, ok ? "ok  " : "FAIL", got);
    };
    char buf[48];
    printf("\n=== mx5-gauge core %s on ESP32-S3 ===\n", gauge::core_version());
    auto rpm = gauge::dec_rpm(gauge::Bytes{0x1A, 0xF8});
    snprintf(buf, sizeof buf, "%.1f", rpm ? *rpm : -1.0);
    expect("dec_rpm(1A F8) == 1726.0", rpm && *rpm == 1726.0, buf);
    auto t = gauge::dec_temp(gauge::Bytes{0x5A});
    snprintf(buf, sizeof buf, "%d", t ? *t : -999);
    expect("dec_temp(5A) == 50", t && *t == 50, buf);
    auto cyc = gauge::build_poll_cycle({0x0C, 0x0D, 0x05});
    snprintf(buf, sizeof buf, "%d", static_cast<int>(cyc.size()));
    expect("build_poll_cycle len == 3", cyc.size() == 3, buf);
    gauge::VehicleState st;
    st.set("coolant", 88.0);
    st.set("coolant", 900.0);
    auto c = st.get("coolant");
    snprintf(buf, sizeof buf, "%.1f rej=%d", c ? *c : -1.0, st.rejected());
    expect("implausible reading rejected", c && *c == 88.0 && st.rejected() == 1, buf);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(buf, sizeof buf, "%u bytes", static_cast<unsigned>(psram));
    expect("psram >= 8MB", psram >= 8u * 1024 * 1024, buf);
    printf("=== %d passed, %d failed ===\n", pass, fail);
}

}  // namespace

extern "C" void app_main(void) {
    core_selfcheck();

    auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    printf("panel: %dx%d   banner: %s\n", BSP_LCD_H_RES, BSP_LCD_V_RES, id.label.c_str());

    bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    build_ui(id);
    bsp_display_unlock();

    // Replay the warm-up the logs actually show: 72 -> 95 C, then hold.
    // Feeding it through VehicleState means the plausibility gate is in the
    // path, exactly as it will be with the car attached.
    gauge::VehicleState st;
    double c = 45.0;
    bool up = true;
    for (;;) {
        st.set("coolant", c);
        bsp_display_lock(0);
        render(st);
        bsp_display_unlock();
        if (up) { c += 0.5; if (c >= 96.0) up = false; }
        else    { c -= 0.5; if (c <= 45.0) up = true; }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
