// Phase 0 Task 13: touch and IMU bring-up on the ESP32-S3-Touch-AMOLED-1.75C.
//
// Everything is reported on the panel as well as over serial, because the
// serial console on this board is awkward: idf_monitor needs a TTY, and any
// esptool call re-enters download mode.
//
// The IMU axis check is the part that matters. In the simulator "harsh" is a
// speed-delta proxy -- laggy and coarse (SPEC.md section 4). On the board the
// QMI8658 replaces it, but only if longitudinal and lateral are the right way
// round: get them backwards and the driving score is silently wrong, and no
// host test can catch it. Gravity is the reference.
#include <cstdio>
#include <cstring>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "imu.h"
#include "version.h"

namespace {

constexpr int kLine = 30;

lv_obj_t* g_i2c   = nullptr;
lv_obj_t* g_imu   = nullptr;
lv_obj_t* g_acc   = nullptr;
lv_obj_t* g_grav  = nullptr;
lv_obj_t* g_touch = nullptr;
lv_obj_t* g_dot   = nullptr;

lv_obj_t* row(lv_obj_t* parent, int index, uint32_t colour, const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_width(l, 330);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -110 + index * kLine);
    lv_label_set_text(l, "");
    return l;
}

void build_ui() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "PHASE 0 BRING-UP");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -150);

    char buf[64];
    lv_obj_t* panel = row(scr, 0, 0x808080, &lv_font_montserrat_20);
    snprintf(buf, sizeof buf, "PANEL  %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_label_set_text(panel, buf);

    lv_obj_t* ps = row(scr, 1, 0x808080, &lv_font_montserrat_20);
    snprintf(buf, sizeof buf, "PSRAM  %u KB",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(ps, buf);

    g_i2c   = row(scr, 2, 0x808080, &lv_font_montserrat_20);
    g_imu   = row(scr, 3, 0x5BD97A, &lv_font_montserrat_20);
    g_acc   = row(scr, 4, 0xFFFFFF, &lv_font_montserrat_20);
    g_grav  = row(scr, 5, 0x4FA3FF, &lv_font_montserrat_20);
    g_touch = row(scr, 6, 0xFFC24A, &lv_font_montserrat_20);

    // A dot that follows your finger: the clearest possible proof that touch
    // coordinates line up with what is on screen.
    g_dot = lv_obj_create(scr);
    lv_obj_set_size(g_dot, 34, 34);
    lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_border_width(g_dot, 0, 0);
    lv_obj_add_flag(g_dot, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

extern "C" void app_main(void) {
    printf("\n=== mx5-gauge Task 13 bring-up (core %s) ===\n", gauge::core_version());

    ESP_ERROR_CHECK(bsp_i2c_init());
    uint8_t addrs[16];
    int n_addr = imu_i2c_scan(addrs, sizeof addrs);
    char scan[80] = "I2C    ";
    for (int i = 0; i < n_addr; ++i) {
        char one[8];
        snprintf(one, sizeof one, "%02X ", addrs[i]);
        strncat(scan, one, sizeof scan - strlen(scan) - 1);
    }
    if (n_addr == 0) strncat(scan, "none!", sizeof scan - strlen(scan) - 1);
    printf("%s\n", scan);

    bool imu_ok = imu_init();
    printf("IMU: %s", imu_ok ? "QMI8658 " : "not found\n");
    if (imu_ok) printf("@ 0x%02X whoami=0x%02X\n", imu_address(), imu_whoami());

    bsp_display_start();
    bsp_display_backlight_on();
    lv_indev_t* indev = bsp_display_get_input_dev();

    bsp_display_lock(0);
    build_ui();
    lv_label_set_text(g_i2c, scan);
    char b[64];
    if (imu_ok) snprintf(b, sizeof b, "IMU    QMI8658 @0x%02X", imu_address());
    else        snprintf(b, sizeof b, "IMU    NOT FOUND");
    lv_label_set_text(g_imu, b);
    if (!imu_ok) lv_obj_set_style_text_color(g_imu, lv_color_hex(0xFF3B30), 0);
    bsp_display_unlock();

    int touches = 0;
    int last_x = -1, last_y = -1;
    int tick = 0;
    for (;;) {
        imu_sample_t s{};
        bool got = imu_ok && imu_read(&s);

        char acc[64], grav[64], tch[64];
        if (got) {
            snprintf(acc, sizeof acc, "ACC  %+.2f %+.2f %+.2f g", s.ax, s.ay, s.az);
            // Name the axis gravity is on, and roughly how the board is held.
            float ax = s.ax < 0 ? -s.ax : s.ax;
            float ay = s.ay < 0 ? -s.ay : s.ay;
            float az = s.az < 0 ? -s.az : s.az;
            const char* axis = (az >= ax && az >= ay) ? "Z" : (ay >= ax ? "Y" : "X");
            float mag = (az >= ax && az >= ay) ? s.az : (ay >= ax ? s.ay : s.ax);
            snprintf(grav, sizeof grav, "GRAV   %s %+.2f g", axis, mag);
        } else {
            snprintf(acc, sizeof acc, "ACC    --");
            snprintf(grav, sizeof grav, "GRAV   --");
        }

        bsp_display_lock(0);
        lv_point_t p{};
        bool pressed = false;
        if (indev) {
            pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
            lv_indev_get_point(indev, &p);
        }
        if (pressed) {
            if (last_x < 0) ++touches;
            last_x = p.x;
            last_y = p.y;
            lv_obj_clear_flag(g_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(g_dot, p.x - 17, p.y - 17);
        } else {
            last_x = -1;
            lv_obj_add_flag(g_dot, LV_OBJ_FLAG_HIDDEN);
        }
        if (pressed) snprintf(tch, sizeof tch, "TOUCH  %d,%d  (n=%d)", (int)p.x, (int)p.y, touches);
        else         snprintf(tch, sizeof tch, "TOUCH  --      (n=%d)", touches);

        lv_label_set_text(g_acc, acc);
        lv_label_set_text(g_grav, grav);
        lv_label_set_text(g_touch, tch);
        bsp_display_unlock();

        // Reprint periodically: the console cannot be attached at boot here.
        if (++tick % 40 == 0) printf("%s | %s | %s\n", acc, grav, tch);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
