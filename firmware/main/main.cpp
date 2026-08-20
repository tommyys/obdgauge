// Phase 0 Task 13: measure the cost of the SPEC.md section 11 rpm backdrop at
// 466x466, so Phase 2's view design is built on a number instead of a guess.
//
// Section 11 defines it precisely: transparent to ~24% radius, ramping to
// near-opaque red at the rim; f = rpm/rpm_red, tint starting at 22% of redline,
// raised to the power 1.35. Those constants are reproduced here so the thing
// being measured is the real backdrop, not an approximation of it.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "vehicle.h"

namespace {

constexpr int   W = BSP_LCD_H_RES;
constexpr int   H = BSP_LCD_V_RES;
constexpr int   CX = W / 2;
constexpr int   CY = H / 2;
constexpr int   R  = W / 2;
constexpr float kCoreRadius = 0.24f;   // transparent below this fraction of R
constexpr float kTintStart  = 0.22f;   // fraction of redline before any tint
constexpr float kGamma      = 1.35f;
constexpr int   kShift      = 8;       // r2 >> 8 indexes the LUT
constexpr int   kLutN       = ((R * R) >> kShift) + 2;

uint16_t* g_fb   = nullptr;            // 466x466 RGB565 canvas in PSRAM
uint16_t  g_lut[kLutN];
int       g_annulus_r2 = 0;            // r2 below which nothing is drawn

// Alpha for a given rpm, exactly as section 11 specifies.
float backdrop_alpha(float rpm, float rpm_red) {
    float f = rpm / rpm_red;
    if (f <= kTintStart) return 0.0f;
    float x = (f - kTintStart) / (1.0f - kTintStart);
    if (x > 1.0f) x = 1.0f;
    return powf(x, kGamma) * 0.92f;    // 0.92 = "near-opaque" at redline
}

// Rebuild the radius->colour table. This is the only per-rpm work; the inner
// pixel loop is then a shift and a table lookup.
void build_lut(float alpha) {
    for (int i = 0; i < kLutN; ++i) {
        int   r2 = i << kShift;
        float r  = sqrtf(static_cast<float>(r2)) / R;
        float v  = 0.0f;
        if (r > kCoreRadius && r <= 1.0f) {
            float t = (r - kCoreRadius) / (1.0f - kCoreRadius);
            v = alpha * t * t;          // vignette ramps toward the rim
        }
        int red = static_cast<int>(v * 31.0f + 0.5f);
        if (red > 31) red = 31;
        g_lut[i] = static_cast<uint16_t>(red << 11);   // RGB565, red only
    }
}

// Fill the whole disc. `from_r2` lets the annulus variant skip the core.
void fill(int from_r2) {
    for (int y = 0; y < H; ++y) {
        int dy = y - CY;
        int dy2 = dy * dy;
        uint16_t* row = g_fb + y * W;
        for (int x = 0; x < W; ++x) {
            int dx = x - CX;
            int r2 = dx * dx + dy2;
            if (r2 < from_r2) continue;
            int idx = r2 >> kShift;
            row[x] = (idx < kLutN) ? g_lut[idx] : 0;
        }
    }
}

struct Result {
    const char* name;
    float fps;
    float gen_ms;      // mean time generating pixels
    float frame_ms;    // mean total time including flush
    int   regens;
};

// A realistic sweep: 800 -> 7000 rpm over 4 s is a hard pull, and the colour
// moves fastest there, so this is the worst case rather than an average one.
float rpm_at(int64_t t_us) {
    float phase = fmodf(t_us / 1e6f, 8.0f);
    float u = phase < 4.0f ? phase / 4.0f : (8.0f - phase) / 4.0f;
    return 800.0f + u * (7000.0f - 800.0f);
}

Result measure(const char* name, lv_display_t* disp, lv_obj_t* canvas,
               int mode, float rpm_red, int seconds) {
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + seconds * 1000000LL;
    int frames = 0, regens = 0;
    int64_t gen_us = 0;
    int last_bucket = -1;
    while (esp_timer_get_time() < deadline) {
        int64_t now = esp_timer_get_time();
        float rpm = rpm_at(now - t0);

        bool regen = true;
        if (mode == 1) {                         // bucketed: 100 rpm steps
            int bucket = static_cast<int>(rpm / 500.0f);
            regen = (bucket != last_bucket);
            last_bucket = bucket;
        } else if (mode == 3) {                  // flush-only ceiling
            regen = false;
        }

        if (regen) {
            int64_t g0 = esp_timer_get_time();
            build_lut(backdrop_alpha(rpm, rpm_red));
            fill(mode == 2 ? g_annulus_r2 : 0);
            gen_us += esp_timer_get_time() - g0;
            ++regens;
        }
        lv_obj_invalidate(canvas);
        lv_refr_now(disp);
        ++frames;
    }
    float elapsed = (esp_timer_get_time() - t0) / 1e6f;
    Result r{};
    r.name = name;
    r.fps = frames / elapsed;
    r.gen_ms = regens ? (gen_us / 1000.0f) / regens : 0.0f;
    r.frame_ms = (elapsed * 1000.0f) / frames;
    r.regens = static_cast<int>(regens / elapsed);
    return r;
}

}  // namespace

extern "C" void app_main(void) {
    printf("\n=== section 11 backdrop cost at %dx%d ===\n", W, H);
    printf("frame buffer: %d bytes RGB565\n", W * H * 2);

    g_fb = static_cast<uint16_t*>(heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM));
    if (!g_fb) { printf("FATAL: no PSRAM for the framebuffer\n"); return; }
    float core = kCoreRadius * R;
    g_annulus_r2 = static_cast<int>(core * core);

    auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    printf("redline from profile: %.0f rpm\n", id.rpm_red);

    lv_display_t* disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // First, with NO canvas in the tree: a solid-colour full-screen refresh.
    // This is the true panel+LVGL floor. If it is far below the canvas figure,
    // the canvas blit is the bottleneck rather than the QSPI transfer.
    Result solid{};
    {
        int64_t t0 = esp_timer_get_time();
        int frames = 0;
        while (esp_timer_get_time() - t0 < 5000000LL) {
            lv_obj_set_style_bg_color(scr, lv_color_hex((frames & 1) ? 0x080000 : 0x000008), 0);
            lv_obj_invalidate(scr);
            lv_refr_now(disp);
            ++frames;
        }
        float el = (esp_timer_get_time() - t0) / 1e6f;
        solid.name = "solid fill, no canvas";
        solid.fps = frames / el;
        solid.frame_ms = (el * 1000.0f) / frames;
    }
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t* canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, g_fb, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);
    memset(g_fb, 0, W * H * 2);

    Result res[4];
    res[0] = measure("flush only (ceiling)", disp, canvas, 3, id.rpm_red, 5);
    res[1] = measure("naive: full regen",    disp, canvas, 0, id.rpm_red, 5);
    res[2] = measure("bucketed 500 rpm",     disp, canvas, 1, id.rpm_red, 5);
    res[3] = measure("annulus only",         disp, canvas, 2, id.rpm_red, 5);
    bsp_display_unlock();

    printf("\n%-22s %8s %10s %10s %8s\n", "strategy", "fps", "gen ms", "frame ms", "regen/s");
    printf("%-22s %8.1f %10.2f %10.2f %8d\n", solid.name, solid.fps, 0.0f, solid.frame_ms, 0);
    for (auto& r : res) {
        printf("%-22s %8.1f %10.2f %10.2f %8d\n", r.name, r.fps, r.gen_ms, r.frame_ms, r.regens);
    }
    printf("=== end ===\n");

    // Leave the numbers on screen too.
    bsp_display_lock(0);
    lv_obj_delete(canvas);
    lv_obj_t* t = lv_label_create(scr);
    lv_label_set_text(t, "BACKDROP COST");
    lv_obj_set_style_text_color(t, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -150);
    for (int i = 0; i < 4; ++i) {
        char b[80];
        snprintf(b, sizeof b, "%.0f fps  %.1fms  %s", res[i].fps, res[i].gen_ms, res[i].name);
        lv_obj_t* l = lv_label_create(scr);
        lv_label_set_text(l, b);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_width(l, 380);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, -70 + i * 46);
    }
    bsp_display_unlock();

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
