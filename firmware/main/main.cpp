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
#include <set>
#include <string>
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
#include "live_link.h"
#include "gauge_ui.h"
#include "slide.h"
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

    // Straight to the panel, past LVGL. An lv_canvas here cost 154 ms a frame
    // -- a full LVGL redraw of a full-screen PSRAM image, every frame -- so the
    // splash showed 17 of 31 frames, at an uneven 6.5 fps. The carousel slide
    // was already pushing full-width frames through
    // esp_lv_adapter_dummy_draw_blit; this is that same path, and it now costs
    // 19 ms a frame.
    if (!gauge_ui::direct_draw_begin(disp)) {
        printf("boot: direct draw unavailable -- skipping splash\n");
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    int shown = 0, last = -1;
    int64_t read_us = 0, swap_us = 0, sync_us = 0, blit_us = 0;
    bool ok = true;
    for (;;) {
        int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed >= kBootUs) break;
        // Which frame *should* be on screen at this instant.
        int idx = static_cast<int>(elapsed * hdr.frames / kBootUs);
        if (idx >= hdr.frames) idx = hdr.frames - 1;
        if (idx != last) {
            const int64_t r0 = esp_timer_get_time();
            esp_partition_read(part, sizeof(AssetHeader) + static_cast<size_t>(idx) * frame_bytes,
                               g_fb, frame_bytes);
            read_us += esp_timer_get_time() - r0;
            // Swaps g_fb in place, which is why every frame is re-read from
            // flash rather than any of them being re-blitted.
            if (!gauge_ui::direct_draw_frame(disp, g_fb, W, H,
                                            &swap_us, &sync_us, &blit_us))
                ok = false;
            last = idx;
            ++shown;
        }
    }
    const int64_t total_us = esp_timer_get_time() - t0;
    // Per-stage, because "the splash is choppy" was answerable only by knowing
    // which of the four stages owns the frame time. Whatever the next attempt
    // to speed this up is, this line is what tells it whether it worked.
    printf("boot: showed %d of %u frames in %.2f s (%.1f fps)%s\n", shown, hdr.frames,
           total_us / 1e6, shown * 1e6 / static_cast<double>(total_us ? total_us : 1),
           ok ? "" : " -- BLIT OR SYNC FAILED");
    if (shown)
        printf("boot: per frame read %lldms swap %lldms sync %lldms blit %lldms\n",
               read_us / shown / 1000, swap_us / shown / 1000,
               sync_us / shown / 1000, blit_us / shown / 1000);

    // FADING: dip to black. The clip ends on a lit car filling the screen, and
    // cutting straight from that to a dial would read as a glitch rather than a
    // hand-off (section 14). Handing the panel back triggers a full LVGL
    // refresh, which is what actually paints the black.
    gauge_ui::direct_draw_end(disp);
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
    // 64-byte aligned because the frame is cache-synced before the panel's DMA
    // reads it, exactly as the slide's buffers are.
    g_fb = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, W * H * 2, MALLOC_CAP_SPIRAM));
    if (!g_fb) { printf("FATAL: no PSRAM\n"); return; }

    auto id = gauge::identify("JM0NDA1R0R2345678", "", "MX-5");
    // Single-buffered, because that is all this panel offers: the adapter only
    // permits tear-avoid NONE or TE_SYNC on a SPI interface, so the double and
    // triple buffering that would overlap render with DMA is unavailable.
    // A full-screen change therefore costs the transfer plus a full render.
    // The transfer is 19 ms measured (466x466x2 over QSPI at the 80 MHz this
    // project's vendored BSP sets); the render is the larger half and is what
    // holds the live views to the 10-22 fps the log reports.
    lv_display_t* disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    play_boot_clip(disp, scr);
    // The clip's 434 KB framebuffer is dead the moment the splash ends, and the
    // carousel slide wants three buffers of exactly that size. Hand it back
    // before the UI asks.
    heap_caps_free(g_fb);
    g_fb = nullptr;
    // Instruments are withheld, not covered: they are created only now.
    gauge_ui::init(scr, id);
    bsp_display_unlock();
    // gauge_ui::slide_selftest() pushes a known pattern through the blit path.
    // It established two things that no log could: a full-frame blit returns
    // ESP_ERR_NO_MEM (see kBlitRows), and the panel wants RGB565 byte-swapped
    // -- the bands came back BLUE RED GREEN WHITE. Not called on a normal boot,
    // but kept, because it answers questions the serial log cannot.

    esp_lv_adapter_fps_stats_enable(disp, true);
    lv_indev_t* indev = bsp_display_get_input_dev();
    int64_t last_fps_log = esp_timer_get_time();
    printf("ui: %d views, starting on %s\n",
           gauge_ui::view_count(), gauge_ui::current_view_name());

    // Looking for the car is deliberately NOT started here. Bringing the BLE
    // controller up costs a burst of DMA-capable internal RAM, and the gauge's
    // first couple of seconds are when LVGL is drawing whole screens at once --
    // its own most memory-hungry moment. Measured on the board: starting the
    // radio at this point made the panel fail draws
    // ("spi transmit (queue) color failed") for the first 13 seconds, which is
    // a visibly torn gauge; started after the views have settled, there are
    // none. Nothing is lost by waiting -- the car is not going anywhere, and
    // the replay covers the gap.
    constexpr int64_t kLiveStartUs = 5 * 1000 * 1000;
    bool live_started = false;
    bool live_mode = false;

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

    // Which channels this drive actually carries. The views use it to decide
    // whether they have anything to show at all -- a car that never reports
    // fuel rate gets "NO FUEL RATE" on the economy view rather than a screen of
    // dashes (SPEC.md section 4). Built once: the drive's channel table is
    // fixed for the whole file.
    //
    // Left as a null pointer when there is no replay, which the views read as
    // "not identified yet" and so show everything. That is the right answer for
    // the synthetic sweep below, which is a bench signal rather than a car.
    std::set<std::string> channels;
    if (have_replay) {
        for (int i = 0; i < replay.channel_count(); ++i)
            channels.insert(replay.channel_name(static_cast<uint16_t>(i)));
        printf("replay: %d channels available to the views\n", (int)channels.size());
    }
    const std::set<std::string>* supported = have_replay ? &channels : nullptr;

    gauge::VehicleState st;
    gauge::Trip trip;
    gauge::DrivingScore score;
    double synth = 45.0;
    bool synth_up = true;
    int64_t t0 = esp_timer_get_time();

    for (;;) {
        if (!live_started && esp_timer_get_time() - t0 > kLiveStartUs) {
            live_started = true;
            live::start("vlinker");
        }

        // The car wins over the replay the moment it has answered its
        // supported-PID sweep, and the switch is one-way: a dropped link
        // reconnects rather than falling back, because a replayed drive
        // running under a driver who is actually driving is worse than a
        // frozen one.
        if (live::ready() && !live_mode) {
            live_mode = true;
            have_replay = false;
            supported = &live::keys();
            if (!live::vin().empty())
                id = gauge::identify(live::vin(), "", "MX-5");
            st = gauge::VehicleState{};
            trip = gauge::Trip{};
            score = gauge::DrivingScore{};
            printf("live: %s -- switching the views off replay\n", live::status());
        }

        if (live_mode) {
            live::Sample smp{};
            while (live::next(&smp)) {
                st.set(smp.key, smp.value);
                // Wall-clock seconds, unlike the replay's own timeline: this
                // drive is happening now, so trip distance and the harshness
                // thresholds are measuring real elapsed time (SPEC.md s4).
                trip.update(smp.t_s, st.get("speed"), st.get("fuel_rate"));
                score.update(smp.t_s, st.get("speed"), st.get("rpm"),
                             st.get("throttle"), st.get("fuel_rate"));
            }
        } else if (have_replay) {
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
        gauge_ui::Model model{st, trip, score, id, supported};
        gauge_ui::update(model);
        bsp_display_unlock();

        if (esp_timer_get_time() - last_fps_log > 2000000) {
            last_fps_log = esp_timer_get_time();
            uint32_t fps = 0;
            if (esp_lv_adapter_get_fps(disp, &fps) == ESP_OK) {
                // Print the indev's own state too. If it reads PRESSED with no
                // finger on the glass, the input device is wedged below LVGL's
                // event layer, which is a different bug from a wrap problem.
                const char* st_s = "?";
                lv_point_t pt{};
                if (indev) {
                    st_s = (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED)
                               ? "PRESSED" : "released";
                    lv_indev_get_point(indev, &pt);
                }
                // LVGL's pool is reported because running it out does not
                // degrade -- it HANGS. Objects and the temporary buffers the
                // renderer needs come from the same fixed pool, and the builtin
                // allocator's out-of-memory path is an assert that spins
                // forever, so the board simply goes silent and needs a replug.
                // That is what every swipe did when the eight views' objects
                // left too little for lv_snapshot's arc masks. If this figure
                // creeps back toward 100%, raise CONFIG_LV_MEM_SIZE_KILOBYTES
                // before adding more objects.
                lv_mem_monitor_t mm;
                lv_mem_monitor(&mm);
                // DMA-capable internal RAM, reported because LVGL's own pool
                // being healthy says nothing about it: the panel's SPI
                // transfers can only come from here, and when BLE was allowed
                // to take its buffers from the same place the driver failed
                // draws with ESP_ERR_NO_MEM while lv pool sat at 33%.
                size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA |
                                                          MALLOC_CAP_INTERNAL);
                printf("ui: %u fps, view %s, gest %d, press %d, rel %d, indev %s @%d,%d, "
                       "lv pool %u%% used (%u free), dma free %u\n",
                       (unsigned)fps, gauge_ui::current_view_name(),
                       gauge_ui::gesture_count(), gauge_ui::press_count(),
                       gauge_ui::release_count(), st_s, (int)pt.x, (int)pt.y,
                       (unsigned)mm.used_pct, (unsigned)mm.free_size,
                       (unsigned)dma_free);
                // Repeated rather than printed once at the swipe: a serial
                // capture that opens after the swipe was losing it every time.
                printf("     %s\n", gauge_ui::slide_note());
                bsp_display_lock(-1);
                gauge_ui::set_fps(fps);
                bsp_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
