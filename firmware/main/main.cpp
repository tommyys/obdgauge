// The boot splash (SPEC.md section 14) followed by the engine-vitals home view.
//
// Section 14's rule is that the instruments are *withheld rather than covered*:
// nothing shows until the splash finishes, which is how a cluster behaves and
// spares you the first second of half-populated channels. So the sequence here
// is WAKING (the clip) -> FADING (a push into the centre, dimming to black) ->
// RUNNING (the gauge), and the gauge objects are not even created until the dip
// is over.
//
// The clip is 60 deflated RGB565 frames in the `assets` partition, paced by the
// wall clock across BOOT_MS. Pacing by time rather than by frame index means a
// panel that cannot keep up drops frames instead of running the animation slow
// -- the splash always lasts BOOT_MS (4 s), which is what section 14 specifies,
// and the fade another BOOT_FADE_MS (1.2 s) on top.
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

#include "button.h"
#include "metrics.h"
#include "replay.h"
#include "state.h"
#include "vehicle.h"
#include "drive_source.h"
#include "live_link.h"
#include "ble_transport.h"
#include "wifi_time.h"
#include "imu.h"
#include "mount_cache.h"
#include "flight_log.h"
#include <cstdlib>
#include <ctime>
#include "drive_log.h"
#include "drives_list.h"
#include "serial_cmd.h"
#include "sweep.h"
#include "gauge_ui.h"
#include "slide.h"
#include "esp_lv_adapter.h"
#include "version.h"
// tinfl_decompress_mem_to_mem lives in the ESP32-S3 ROM (esp32s3.rom.ld), so
// the clip is decompressed at no cost in flash or code size.
#include "miniz.h"

namespace {

// state.py: BOOT_MS = 4000, BOOT_FADE_MS = 1200
constexpr int64_t kBootUs = 4000 * 1000;
constexpr int64_t kFadeUs = 1200 * 1000;

constexpr int W = BSP_LCD_H_RES;
constexpr int H = BSP_LCD_V_RES;

// Written by tools/build_boot_asset.sh, which is where the format and the
// measurements behind it are documented. Frames are individually raw-deflated
// and stored in the panel's byte order; a frames+1 table of absolute file
// offsets follows this header, so frame k is [off[k], off[k+1]).
struct AssetHeader {
    char     magic[4];   // "MX5C"
    uint16_t w, h;
    uint16_t frames;
    uint16_t fps;
    uint32_t flags;      // bit 0: pixels big-endian.  bit 1: raw-deflated
    uint32_t max_comp;   // biggest compressed frame, so one buffer fits them all
    uint32_t reserved;
} __attribute__((packed));

constexpr uint32_t kFlagBigEndian = 1;
constexpr uint32_t kFlagDeflate   = 2;

uint16_t* g_fb = nullptr;

// ---- the hand-off: zoom into the centre while dipping to black ------------

// How far in the push goes by the time the screen is black. 1.35 is a third
// again: enough that the movement is unmistakable on a 466 px round panel,
// little enough that nearest-neighbour resampling never shows a stair edge --
// the frames it magnifies are a dark car on black, with no fine detail to
// break up.
constexpr float kFadeZoom = 1.35f;

// The frame is stored, and wanted back, in the panel's byte order; the
// unpacking in between has to happen in the CPU's. Two swaps a pixel is
// cheaper than keeping a second little-endian copy of a 434 KB frame.
inline uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

// `frame` is the last clip frame and is only ever read, so every fade frame is
// resampled from the original rather than from its own predecessor -- zooming a
// zoom would soften the car a little more each time.
//
// Paced by time exactly as the clip is: a slow frame costs that frame, and the
// dip still ends when BOOT_FADE_MS says. If either buffer cannot be had, the
// old behaviour -- straight to black, then wait -- is the fallback, because a
// hard cut is worse than a splash but better than a stall.
void fade_zoom_to_black(lv_display_t* disp, const uint16_t* frame) {
    constexpr int kPix = W * H;
    // 64-byte aligned and DMA-reachable for the same reason g_fb is: the panel
    // reads it directly after a cache sync.
    uint16_t* dst = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kPix * 2, MALLOC_CAP_SPIRAM));
    // 5- and 6-bit channel ramps, rebuilt once per fade frame. Heap, not
    // locals: app_main's stack has about 200 bytes spare, and this is 192.
    uint8_t* lut = static_cast<uint8_t*>(malloc(32 + 64));
    if (!dst || !lut) {
        printf("boot: no room to fade (%d KB + 96 B) -- cutting to black\n", kPix * 2 / 1024);
        heap_caps_free(dst);
        free(lut);
        int64_t f0 = esp_timer_get_time();
        while (esp_timer_get_time() - f0 < kFadeUs) vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    uint8_t* lut5 = lut;        // red and blue
    uint8_t* lut6 = lut + 32;   // green

    const int64_t f0 = esp_timer_get_time();
    int frames = 0;
    for (;;) {
        const int64_t elapsed = esp_timer_get_time() - f0;
        if (elapsed >= kFadeUs) break;
        const float p = static_cast<float>(elapsed) / static_cast<float>(kFadeUs);

        // The zoom eases IN (p*p) and the brightness eases OUT of full
        // (smoothstep), so the push is visible for a beat before the picture
        // has gone -- fading them both linearly hides the movement in the dark.
        const float scale  = 1.0f + (kFadeZoom - 1.0f) * p * p;
        const float bright = 1.0f - p * p * (3.0f - 2.0f * p);
        for (int i = 0; i < 32; ++i) lut5[i] = static_cast<uint8_t>(i * bright + 0.5f);
        for (int i = 0; i < 64; ++i) lut6[i] = static_cast<uint8_t>(i * bright + 0.5f);

        // 16.16 fixed point. step < 1 source pixel per destination pixel is
        // what makes this a zoom IN: the frame is sampled from a window
        // narrower than the panel, centred on the middle.
        const int32_t step = static_cast<int32_t>(65536.0f / scale);
        const int32_t half = W / 2;
        for (int y = 0; y < H; ++y) {
            int32_t sy = ((y - half) * step >> 16) + half;
            if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;
            const uint16_t* srow = frame + static_cast<size_t>(sy) * W;
            uint16_t* drow = dst + static_cast<size_t>(y) * W;
            int32_t sx_f = -half * step;
            for (int x = 0; x < W; ++x, sx_f += step) {
                int32_t sx = (sx_f >> 16) + half;
                if (sx < 0) sx = 0; else if (sx >= W) sx = W - 1;
                const uint16_t v = bswap16(srow[sx]);
                drow[x] = bswap16(static_cast<uint16_t>(
                    (lut5[(v >> 11) & 31] << 11) |
                    (lut6[(v >> 5) & 63] << 5) |
                     lut5[v & 31]));
            }
        }
        gauge_ui::direct_draw_frame(disp, dst, W, H, /*pixels_big_endian=*/true);
        ++frames;
    }
    const int64_t took = esp_timer_get_time() - f0;
    printf("boot: fade %d frames in %.2f s (%.1f fps), zoom to %.2fx\n",
           frames, took / 1e6, frames * 1e6 / static_cast<double>(took ? took : 1),
           static_cast<double>(kFadeZoom));
    heap_caps_free(dst);
    free(lut);
}

// ---- the boot clip -------------------------------------------------------

bool play_boot_clip(lv_display_t* disp, lv_obj_t* scr) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!part) { printf("boot: no assets partition\n"); return false; }

    AssetHeader hdr{};
    if (esp_partition_read(part, 0, &hdr, sizeof hdr) != ESP_OK ||
        memcmp(hdr.magic, "MX5C", 4) != 0) {
        // "MX5B" was the uncompressed format this replaced. Naming it is worth
        // a line: the symptom of flashing a stale asset is a missing splash,
        // which otherwise looks like a firmware fault rather than a rebuild.
        printf("boot: no MX5C clip in assets (found '%.4s') -- skipping splash\n",
               hdr.magic);
        return false;
    }
    if (hdr.w != W || hdr.h != H) {
        printf("boot: clip is %ux%u, panel is %dx%d -- skipping\n", hdr.w, hdr.h, W, H);
        return false;
    }
    if ((hdr.flags & (kFlagBigEndian | kFlagDeflate)) !=
        (kFlagBigEndian | kFlagDeflate)) {
        printf("boot: clip flags %08x unsupported -- skipping\n", (unsigned)hdr.flags);
        return false;
    }
    const size_t frame_bytes = static_cast<size_t>(hdr.w) * hdr.h * 2;
    printf("boot: %u frames %ux%u @%u fps, biggest %u B compressed\n",
           hdr.frames, hdr.w, hdr.h, hdr.fps, (unsigned)hdr.max_comp);

    // Everything here is heap, never a local: app_main runs on an 8 KB stack
    // with about 200 bytes to spare, and a frame-sized array here would
    // overflow it into a boot loop (it has, once, from a 592-byte local).
    const size_t table_bytes = sizeof(uint32_t) * (hdr.frames + 1);
    uint32_t* offs = static_cast<uint32_t*>(malloc(table_bytes));
    // The compressed source is read into PSRAM and only ever read by the CPU,
    // so unlike the frame buffer it needs no alignment and no DMA capability.
    uint8_t*  src  = static_cast<uint8_t*>(heap_caps_malloc(hdr.max_comp, MALLOC_CAP_SPIRAM));

    // The decompressor's own state is 10.6 KB -- three Huffman tables -- and it
    // is why `tinfl_decompress_mem_to_mem` cannot be used from here: that
    // wrapper puts the whole struct on the CALLER's stack, so it smashed
    // app_main's 8 KB and the gauge panicked in the next SPI call, with a
    // corrupted semaphore handle and a backtrace pointing at the blit rather
    // than at anything to do with decoding. The streaming entry point takes the
    // state as a pointer, which is the only reason this fits.
    //
    // Internal RAM by preference: this is touched for every output byte, and in
    // PSRAM it would give back the time the compression just won. PSRAM is the
    // fallback rather than a failure, because a slower splash beats no splash.
    tinfl_decompressor* inf = static_cast<tinfl_decompressor*>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_INTERNAL));
    const bool inf_internal = inf != nullptr;
    if (!inf)
        inf = static_cast<tinfl_decompressor*>(
            heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));

    if (!offs || !src || !inf ||
        esp_partition_read(part, sizeof(AssetHeader), offs, table_bytes) != ESP_OK) {
        printf("boot: cannot hold the frame table (%u B), a frame (%u B) or the "
               "decompressor (%u B) -- skipping\n",
               (unsigned)table_bytes, (unsigned)hdr.max_comp,
               (unsigned)sizeof(tinfl_decompressor));
        free(offs);
        heap_caps_free(src);
        heap_caps_free(inf);
        return false;
    }
    printf("boot: decompressor %u B in %s RAM\n",
           (unsigned)sizeof(tinfl_decompressor), inf_internal ? "internal" : "PS");

    // Straight to the panel, past LVGL. An lv_canvas here cost 154 ms a frame
    // -- a full LVGL redraw of a full-screen PSRAM image, every frame -- so the
    // splash showed 17 of 31 frames, at an uneven 6.5 fps. The carousel slide
    // was already pushing full-width frames through
    // esp_lv_adapter_dummy_draw_blit; this is that same path, and it now costs
    // 28 ms a frame.
    if (!gauge_ui::direct_draw_begin(disp)) {
        printf("boot: direct draw unavailable -- skipping splash\n");
        free(offs);
        heap_caps_free(src);
        heap_caps_free(inf);
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    int shown = 0, dropped = 0, last = -1;
    int64_t read_us = 0, inflate_us = 0, swap_us = 0, sync_us = 0, blit_us = 0;
    bool ok = true;
    for (;;) {
        int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed >= kBootUs) break;
        // Which frame *should* be on screen at this instant. Time-paced, not
        // index-paced, so a slow frame costs that frame rather than delaying
        // every frame after it: the splash always ends when BOOT_MS says.
        int idx = static_cast<int>(elapsed * hdr.frames / kBootUs);
        if (idx >= hdr.frames) idx = hdr.frames - 1;
        if (idx != last) {
            if (last >= 0) dropped += idx - last - 1;
            const size_t comp = offs[idx + 1] - offs[idx];
            const int64_t r0 = esp_timer_get_time();
            esp_partition_read(part, offs[idx], src, comp);
            const int64_t r1 = esp_timer_get_time();
            // 67 KB in, 434 KB out, 31 ms -- against 47 ms just to read the
            // 434 KB raw. Decompressing is the faster way to get a frame off
            // this flash, as well as the smaller one.
            //
            // One call per frame: the whole compressed frame is already in
            // `src` and the whole output fits, so NON_WRAPPING_OUTPUT_BUF lets
            // tinfl copy matches straight out of what it has written rather
            // than keeping a 32 KB dictionary of its own. No HAS_MORE_INPUT,
            // because there is none -- that is what makes DONE mean finished.
            tinfl_init(inf);
            size_t in_bytes = comp, out_bytes = frame_bytes;
            const tinfl_status st = tinfl_decompress(
                inf, src, &in_bytes,
                reinterpret_cast<uint8_t*>(g_fb), reinterpret_cast<uint8_t*>(g_fb),
                &out_bytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            const size_t got = (st == TINFL_STATUS_DONE) ? out_bytes : 0;
            const int64_t r2 = esp_timer_get_time();
            read_us    += r1 - r0;
            inflate_us += r2 - r1;
            if (got != frame_bytes) {
                // A corrupt frame is worth saying out loud rather than showing:
                // g_fb still holds the previous frame, so skipping it reads as a
                // stutter instead of as a screenful of noise.
                printf("boot: frame %d inflated to %u of %u bytes\n",
                       idx, (unsigned)got, (unsigned)frame_bytes);
                ok = false;
            } else if (!gauge_ui::direct_draw_frame(disp, g_fb, W, H,
                                                    /*pixels_big_endian=*/true,
                                                    &swap_us, &sync_us, &blit_us)) {
                ok = false;
            } else {
                ++shown;
            }
            last = idx;
        }
    }
    const int64_t total_us = esp_timer_get_time() - t0;
    // Handed back before the fade, not after: the decompressor is 10.6 KB of
    // internal RAM and the fade wants none of it -- only PSRAM and 96 bytes.
    free(offs);
    heap_caps_free(src);
    heap_caps_free(inf);
    // Per-stage, because "the splash is choppy" was answerable only by knowing
    // which of the stages owns the frame time. Whatever the next attempt to
    // speed this up is, this line is what tells it whether it worked.
    printf("boot: showed %d of %u frames (%d skipped) in %.2f s (%.1f fps)%s\n",
           shown, hdr.frames, dropped,
           total_us / 1e6, shown * 1e6 / static_cast<double>(total_us ? total_us : 1),
           ok ? "" : " -- A FRAME FAILED");
    if (shown)
        printf("boot: per frame read %lldms inflate %lldms swap %lldms sync %lldms blit %lldms\n",
               read_us / shown / 1000, inflate_us / shown / 1000, swap_us / shown / 1000,
               sync_us / shown / 1000, blit_us / shown / 1000);

    // FADING (section 14). The clip ends on a lit car filling the screen, and
    // cutting straight from that to a dial would read as a glitch rather than a
    // hand-off. Until 2026-09-03 this "fade" was a cut to black followed by a
    // wait, which is why it needed a dip at all; it now pushes in towards the
    // car's own centre while it darkens, so the hand-off reads as the car
    // receding into the instrument rather than as a screen being switched off.
    fade_zoom_to_black(disp, g_fb);

    // Handing the panel back triggers a full LVGL refresh, which is what paints
    // the black the instruments are then built on top of.
    gauge_ui::direct_draw_end(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_invalidate(scr);
    lv_refr_now(disp);
    return true;
}

}  // namespace

// The networks to try for the time, in order. Compiled in rather than typed
// on the gauge: a WPA password on a 466 px round panel is a worse experience
// than editing one header, and the file never enters git.
//
// __has_include rather than a hard include so that a fresh clone -- which has
// no wifi_creds.h -- still builds and boots. It simply has no networks, and
// wifi_time::start() does nothing with an empty list.
#if defined(__has_include)
#  if __has_include("wifi_creds.h")
#    include "wifi_creds.h"
#  endif
#endif

namespace {
#ifdef GAUGE_WIFI_NETWORKS
const gauge::WifiNetwork kWifiNets[] = { GAUGE_WIFI_NETWORKS };
constexpr std::size_t kWifiCount = sizeof kWifiNets / sizeof kWifiNets[0];
#else
const gauge::WifiNetwork* kWifiNets = nullptr;
constexpr std::size_t kWifiCount = 0;
#endif

// wifi_time may not depend on main, so the two things it needs from the
// gauge -- somewhere to put the clock, somewhere to put its log -- are handed
// over as functions.
void wifi_got_time(uint32_t epoch_s) { drive_log_set_epoch(epoch_s); }
void wifi_logged(const char* line)   { flight_log("%s", line); }
}  // namespace

// Demo mode: every dial swept at once, for half an hour.
//
// It was the recorded replay, and the replay is still there behind the DEMO
// console command. This is a better demo of the gauge ITSELF: a real drive only
// ever visits the revs, temperatures and power it actually used, so the ends of
// every scale -- the redline, a hot engine, full power -- and the trip ring
// walking red to green are exactly what a replayed drive never shows you.
//
// Half an hour rather than for ever, and cancelled the moment the car takes
// over (sweep_stop_all): a swept dial overrides the real reading, and a gauge
// lying about the engine is the one thing demo mode is not allowed to become.
constexpr double kDemoSecs = 1800.0;

void start_demo_sweeps() {
    sweep_start(kDemoSecs, 1000.0, 8000.0);
    sweep_temp_start(kDemoSecs, 30.0, 120.0);
    sweep_kw_start(kDemoSecs, 0.0, 150.0);
    sweep_econ_start(kDemoSecs, 4.0, 16.0);
    printf("demo: sweeping rpm, coolant, kW and economy for %.0f s\n", kDemoSecs);
}

extern "C" void app_main(void) {
    printf("\n=== mx5-gauge %s: boot splash + home view ===\n", gauge::core_version());
    // First, so that a run which dies during display bring-up still says so,
    // and so the previous run's record is printed before anything can hang.
    flight_log_init();
    // 64-byte aligned because the frame is cache-synced before the panel's DMA
    // reads it, exactly as the slide's buffers are.
    g_fb = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, W * H * 2, MALLOC_CAP_SPIRAM));
    if (!g_fb) { printf("FATAL: no PSRAM\n"); return; }

    // Why this boot happened, read before anything acts on it and cleared at
    // once: a request survives esp_restart() and must be spent exactly once,
    // or every reset after a button press would replay it.
    button_init();
    const boot_request_t boot_req = button_boot_request();
    button_boot_request_clear();
    if (boot_req != BOOT_NORMAL)
        printf("boot: by button -- %s\n",
               boot_req == BOOT_DEMO ? "the full splash, then sweeping every dial"
                                     : "skipping the splash, retrying wifi");

    // The board has no timezone until it is given one, so strftime() renders
    // every drive in UTC -- the 2026-08-29 drive listed as 03:24 for a drive
    // that started at 11:24. The clock the Mac lends over TIME is epoch
    // seconds and is unaffected; this is only how it is shown. POSIX gets the
    // sign backwards on purpose: -8 means eight hours EAST of UTC.
    setenv("TZ", "MYT-8", 1);
    tzset();

    // Before the display and before the recorder, because the BT controller
    // needs one contiguous 30,720-byte block of internal RAM and this is the
    // last moment the internal heap is whole. Started later it failed with
    // 33 KB free and a 21.5 KB largest hole, then asserted and rebooted the
    // gauge -- see BleTransport::radio_init(). Scanning still starts at 10 s;
    // this only claims the memory.
    const bool radio_up = gauge_platform::BleTransport::radio_init();
    flight_log("radio %s", radio_up ? "up" : "FAILED");



    // The clock, over WiFi. Third, and both neighbours are load-bearing.
    //
    // AFTER the BT controller, and not merely for memory. Both radios share
    // the PHY, and esp_phy_disable() only closes the RF and drops the shared
    // modem clock when the caller is the *last* user of it. With WiFi going
    // first and alone, its own teardown took that clock down -- and the USB
    // console's input died with it. The board still printed, so it looked
    // healthy, but `STATS` and `pull_drives.py` got no reply at all. Measured
    // 2026-09-03 against a stashed baseline. With NimBLE already holding the
    // PHY, WiFi's teardown leaves the clock alone and the console survives.
    //
    // BEFORE the blit band and the display, because bringing the station up
    // takes the internal heap from 130,831 bytes free / 63,488 largest down to
    // 35,511 / 23,552. Started during display bring-up -- on the reasoning
    // that the five seconds the panel takes are dead time -- the display could
    // not get its 14,912-byte secondary buffer and asserted, and the gauge
    // boot-looped. Display bring-up is not idle time; it is when the display
    // *allocates*.
    //
    // So there is exactly one window WiFi fits in, and this is it. It gives
    // every byte back afterwards -- wifi_time logs the largest free block on
    // the way in and out, so a regression here is visible rather than
    // mysterious.
    //
    // The cost is honest: the screen stays dark for the length of the sync, on
    // top of the seconds the panel already takes. Measured on the hotspot:
    // 3.6 s. That is why the budget is tight and why a network that does not
    // answer is abandoned rather than waited on.
    static gauge_platform::wifi_time::Config wcfg;
    wcfg.on_time = wifi_got_time;
    wcfg.log = wifi_logged;
    gauge_platform::wifi_time::start(kWifiNets, kWifiCount, wcfg);
    // Its own task, not app_main's 8 KB frame, but app_main waits for it: the
    // radio must be down before the display asks for memory. The cap is the
    // plan's own budget plus a second of slack, so a wedged driver delays the
    // gauge rather than hanging it.
    gauge_platform::wifi_time::wait(wcfg.plan.boot_budget_ms + 1000);

    // Second, and for the same reason: the carousel's blit buffer is another
    // 29,824 bytes that must be DMA-capable internal RAM. Asked for on the
    // first swipe it was refused -- 32,831 bytes free, largest hole 23,552 --
    // and the swipe then slid nothing. Both big internal claims are made here,
    // while the heap is whole, and everything after fits around them.
    const bool band_ok = gauge_ui::reserve_slide_band(W);
    flight_log("slide band %s", band_ok ? "reserved" : "FAILED");

    // Every remaining long-lived internal claim, here, while the heap is whole.
    // This is the same rule the BT controller and the blit band above already
    // follow, and the two task stacks below were simply left out of it.
    //
    // Biggest first, because what runs out on this board is not free bytes but
    // contiguous ones. The console's 12 KB went in after the display and got
    // away with it only until the live task's 6 KB landed in the middle of the
    // hole it had been using: 15,027 bytes free, largest block 7,680, and the
    // console never started -- which reads exactly like a wedged board.
    //
    // serial_cmd.h has said "call EARLY, before the display" since 2026-09-03.
    // It was called after it anyway. This is that instruction, followed.
    serial_cmd_init();
    // And the live task's 6 KB. Created at its old point -- five seconds into
    // the app loop, after the display and every view had theirs -- there were
    // 3,523 bytes free against the 6,144 it needs, so xTaskCreate failed,
    // nobody checked, and the gauge spent a day at 66 fps never once looking
    // for the car. Parked here; it does not touch the radio until
    // live::start() wakes it in the loop below.
    live::reserve();
    // And the drives scan task's 8 KB, for the same reason. At its old point,
    // after gauge_ui::init(), the largest internal block left was 7,680 -- so
    // it never started, and the Drives view showed nothing while eleven
    // recorded drives sat on the flash. drives_list_init() below still does
    // the wiring, once the recorder and the views exist.
    drives_list_reserve();

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
    // Skipped for a wifi retry only.
    //
    // A short press is someone standing over the gauge waiting to see whether
    // the clock came back, and the splash is 5.2 s of the ~16 s that takes. A
    // DEMO restart is the opposite: the animation is the best thing the gauge
    // does and demo mode exists to show the gauge to somebody, so it plays in
    // full. A power-on plays it too.
    if (boot_req != BOOT_QUICK) play_boot_clip(disp, scr);
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

    // The QMI8658 is on the board and the driving score needs it (SPEC.md B3):
    // "harsh" is a speed-delta proxy today, which lags and cannot tell braking
    // from cornering. Which axis is which can only be settled against real
    // gravity and real cornering, so it is logged rather than guessed.
    const bool have_imu = imu_init();
    printf("imu: %s (addr 0x%02x, whoami 0x%02x)\n",
           have_imu ? "ready" : "NOT FOUND", imu_address(), imu_whoami());
    flight_log("display up, ui ready, imu %s", have_imu ? "ready" : "MISSING");
    drive_log_init();
    // After the recorder: the Drives view reads what it wrote, and its scan
    // task asks drive_log_buf() for the mounted ring. The task's stack was
    // claimed before the display (see drives_list_reserve above); this opens
    // the gate that lets it read.
    drives_list_init();
    // The task itself was created before the display (see above); this is only
    // the gate that lets it start reading. Commands must not run until the
    // views, the recorder and the drives list are all up, or a GET could be
    // answered out of a half-built library.
    serial_cmd_enable();

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

    // OFF at power-up. The gauge used to come up replaying a drive from
    // August, needles moving, temperatures climbing, a car's name under it --
    // a car that was not there. On a desk that is a demo; in the car it is a
    // gauge lying about the engine for the thirty seconds before the adapter
    // answers. It is a bench feature now: `DEMO` on the console starts it.
    size_t lib_len = 0;
    const uint8_t* lib = drive_library_map(&lib_len);
    gauge::Replay replay;
    const bool replay_ok = lib && replay.open(lib, lib_len);
    bool have_replay = false;
    if (replay_ok) {
        printf("replay: %d drives, %d channels, %d records -- idle, say DEMO to play\n",
               replay.drive_count(), replay.channel_count(), replay.total_records());
    } else {
        printf("replay: no drive library\n");
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
    if (replay_ok) {
        for (int i = 0; i < replay.channel_count(); ++i)
            channels.insert(replay.channel_name(static_cast<uint16_t>(i)));
    }
    // Null until something is actually feeding the views, which the views read
    // as "not identified yet" and so show everything rather than a wall of
    // not-available screens while the gauge is still looking for the car.
    const std::set<std::string>* supported = nullptr;

    // Static, not stack. These three live for the whole run, and DrivingScore
    // is 592 bytes on its own now that it carries the g solver -- adding that
    // to app_main's frame overflowed the 8 KB main task stack and put the
    // board in a boot loop ("A stack overflow in task main has been
    // detected", 2026-08-29). Raising the stack would have hidden it; the
    // objects were never stack-shaped in the first place. Reassignment at the
    // end of a drive still works exactly as it did.
    static gauge::VehicleState st;
    static gauge::Trip trip;
    static gauge::DrivingScore score;
    // Which way the gauge points, if a previous drive worked it out. Without
    // this the g view says LEARNING for the first six minutes of every drive.
    mount_cache_load(score.g);
    // If it came back from NVS there is nothing new to solve or to save, and
    // saying "solved and saved" about a value that was merely restored is a
    // lie the console has already told once.
    bool mount_saved = score.g.ready();
    // The headroom that overflow ate, printed so the next thing to grow a
    // frame here shows up as a number rather than as a boot loop.
    printf("main: %u bytes of stack headroom\n",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    // Asked for by "RESTART DEMO" on the console: a clean boot that comes up
    // sweeping. The button's own hold does not come through here -- it needs no
    // restart, so it starts the sweeps where it stands.
    if (boot_req == BOOT_DEMO) start_demo_sweeps();

    int64_t t0 = esp_timer_get_time();
    int64_t last_frame_us = t0;
    double  last_imu_t = -1.0;      // so the first published sample is taken
    // How long the note stays up after a hold. It is opaque, so it is a flash
    // of confirmation and not a screen the gauge sits behind.
    int64_t note_until_us = 0;
    // 0 none, 1 a plain reload, 2 a reload into demo. Set by a press or by the
    // console, spent at the bottom of the same frame.
    int     reload_pending = 0;

    for (;;) {
        // Once a frame, which is every 15-20 ms: fast enough to time a press
        // and cheap enough that it costs nothing. It keeps no task and makes no
        // allocation, which on this board is why it is a poll.
        const button_state_t btn = button_poll();
        if (btn == BTN_ACT_DEMO) {
            // A restart, and deliberately so: demo mode opens with the startup
            // animation, and the clip cannot be replayed where it stands. It
            // draws from a 434 KB PSRAM framebuffer that is handed to the
            // carousel's three slide buffers the instant the splash ends -- so
            // the only way to see it again is to come up again.
            reload_pending = 2;
        } else if (btn == BTN_ACT_RELOAD) {
            reload_pending = 1;
        }
        if (const int q = button_take_queued_restart()) reload_pending = q;

        if (replay_ok && !have_replay && !live_mode && demo_wanted()) {
            have_replay = true;
            supported = &channels;
            replay.select(replay.selected());
            t0 = esp_timer_get_time();
            printf("replay: playing '%s', %.0f s at %.0fx\n",
                   replay.drive_name(0).c_str(), replay.duration_s(), kSpeed);
        }

        // ... and not while WiFi still has the radio. One 2.4 GHz front end
        // is shared between them, and the internal RAM behind it is the pool
        // that broke the BLE link on 2026-08-28 when it ran short. WiFi's own
        // budget is 8 s against this 5 s trigger, so the car can wait a few
        // seconds longer on a boot where the clock is being set; it is not
        // going anywhere, and the replay covers the gap.
        if (!live_started && esp_timer_get_time() - t0 > kLiveStartUs &&
            !gauge_platform::wifi_time::busy()) {
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
            // The mounting angle survives. It is a property of the bracket,
            // not of the drive, so switching from replay to the real car must
            // not throw away what has been learned and blank the g view.
            {
                const auto axes = score.g.export_axes();
                score = gauge::DrivingScore{};
                if (axes) score.g.restore_axes(*axes);
            }
            // And the dials off any bench sweep. A demo left running for half
            // an hour would otherwise keep overriding the real engine.
            sweep_stop_all();
            printf("live: %s -- switching the views off replay\n", live::status());
        }

        // The accelerometer, fed on its own clock rather than folded into the
        // OBD updates. It is read at 20 Hz on the recorder task -- four times
        // any OBD channel -- because the driving score measures jerk, and a
        // hard stop sampled twice is not a rate of anything.
        //
        // Never while a replay is playing. A replay carries its own recorded
        // accelerometer on its own timeline, and feeding the part's readings
        // in as well would hand the solver two clocks and a stationary car.
        if (!have_replay) {
            imu_sample_t im{};
            double imu_t = 0.0;
            if (drive_log_imu(&im, &imu_t) && imu_t != last_imu_t) {
                last_imu_t = imu_t;
                score.imu(imu_t, im.ax, im.ay, im.az);
            }
        }

        // Saved the moment the axes are first solved, and only then. NVS is
        // flash: a write per frame would wear it out, and there is nothing
        // new to write until the solver has an answer it did not have before.
        if (!mount_saved && !have_replay && score.g.ready()) {
            mount_saved = true;
            mount_cache_save(score.g);
            printf("mount: solved and saved\n");
        }

        if (live_mode) {
            live::Sample smp{};
            while (live::next(&smp)) {
                st.set(smp.key, smp.value);
                drive_log_sample(smp.key, smp.value, smp.t_s);
                // Wall-clock seconds, unlike the replay's own timeline: this
                // drive is happening now, so trip distance and the harshness
                // thresholds are measuring real elapsed time (SPEC.md s4).
                trip.update(smp.t_s, st.get("speed"), st.get("fuel_rate"));
                score.update(smp.t_s, st.get("speed"), st.get("rpm"),
                             st.get("throttle"), st.get("fuel_rate"),
                             st.get("coolant"));
            }
        } else if (have_replay) {
            double logical = (esp_timer_get_time() - t0) / 1e6 * kSpeed;
            gauge::ReplaySample smp{};
            while (replay.next(logical, &smp)) {
                const std::string key = replay.channel_name(smp.chan);
                st.set(key, smp.value);
                // A recorded drive brings its own accelerometer. Mirrors
                // state.IMU_KEYS in the simulator.
                if (key == "imu_ax" || key == "imu_ay" || key == "imu_az")
                    score.imu(smp.t_ms / 1000.0, st.get("imu_ax"),
                              st.get("imu_ay"), st.get("imu_az"));
                trip.update(smp.t_ms / 1000.0, st.get("speed"), st.get("fuel_rate"));
                score.update(smp.t_ms / 1000.0, st.get("speed"), st.get("rpm"),
                             st.get("throttle"), st.get("fuel_rate"),
                             st.get("coolant"));
            }
            if (replay.finished()) {          // loop the drive
                replay.select(replay.selected());
                st = gauge::VehicleState{};
                trip = gauge::Trip{};
                // Deliberately NOT carrying the axes over. A replay's axes
                // are the axes of whatever car recorded it, on whatever
                // bracket it had; keeping them across a loop would quietly
                // score the next pass against the last one's mounting.
                score = gauge::DrivingScore{};
                t0 = esp_timer_get_time();
            }
        }
        // There is no third case any more. A synthetic coolant used to ramp
        // 45-96 C here whenever nothing else was feeding the gauge, which is
        // exactly the state the gauge is in while it looks for the adapter:
        // the engine view sat there sweeping a temperature for an engine it
        // had not met. Nothing feeding it now means nothing moving on it.

        // A bench sweep overrides whatever the dial was being fed. Applied
        // after the replay and the car, not instead of them: everything else
        // on the view carries on reading the real drive, so a sweep changes
        // the needle and nothing else.
        double swept = 0.0;
        if (sweep_rpm(&swept)) st.set("rpm", swept);
        // Coolant, the same way and for the same reason: the engine view's
        // scale fills across 40-120 C, which no bench engine will do.
        if (sweep_temp(&swept)) st.set("coolant", swept);
        // And kW. Like the rpm sweep, this goes into the state the view reads,
        // so the drive's peak reads the sweep's high while it runs -- SWEEP has
        // always done that to peak rpm. ECON is the one that needs a stand-in,
        // because it would otherwise write into a recorded drive's totals.
        if (sweep_kw(&swept)) st.set("power_kw", swept);

        // What the last trip round this loop actually took, not what the delay
        // below asked for: the instruments ease toward their readings and the
        // ease is time-based, so a frame that ran long has to be told so.
        const int64_t now_us = esp_timer_get_time();
        const double dt_s = (now_us - last_frame_us) / 1e6;
        last_frame_us = now_us;

        // A swept economy is shown through a stand-in Trip rather than by
        // writing into the real one: ECON is a bench command for looking at
        // the ring's colour ramp, and it must not be able to corrupt the
        // totals of a drive that is being recorded while it runs.
        gauge::Trip demo_trip;
        double swept_econ = 0;
        const bool econ_demo = sweep_econ(&swept_econ);
        if (econ_demo) {
            // econ_km_per_l() is derived, so it is driven by giving the
            // stand-in a distance and a litre: km/L is then the distance.
            demo_trip.dist_km   = swept_econ;
            demo_trip.fuel_l    = 1.0;
            demo_trip.elapsed_s = trip.elapsed_s;
            demo_trip.moving_s  = trip.moving_s;
        }

        bsp_display_lock(-1);   // -1 is wait-forever; 0 would be a try-lock
        gauge_ui::Model model{st, econ_demo ? demo_trip : trip, score, id, supported, dt_s};
        gauge_ui::update(model);
        // The button's answer, on the glass, in the same frame the press was
        // seen. Naming the gesture while the finger is still down is what
        // replaced "there is a lag before it responds": a press is
        // acknowledged instantly, and a hold says what it is about to do
        // before it does it.
        if (reload_pending == 2)        gauge_ui::note_show("LAUNCHING\nDEMO MODE");
        else if (reload_pending)        gauge_ui::note_show("RELOADING\nretrying wifi");
        else if (btn == BTN_HELD_LONG)  gauge_ui::note_show("RELEASE\nFOR DEMO");
        else if (btn == BTN_HELD_SHORT) gauge_ui::note_show("RELEASE\nTO RELOAD");
        else if (esp_timer_get_time() < note_until_us)
                                        gauge_ui::note_show("DEMO\nsweeping every dial");
        else                            gauge_ui::note_hide();
        bsp_display_unlock();

        // Outside the lock, and only now: the note above has to be RENDERED
        // before the gauge disappears, and rendering is the LVGL task's job --
        // which cannot have the display lock while this loop is holding it.
        // button_request_restart() waits for that frame before it resets.
        if (reload_pending) button_request_restart(reload_pending == 2);

        // Reprinted for the first minute, because a console attached after the
        // boot print has missed it -- see flight_log_replay().
        static int replays = 0;
        if (replays < 4 && esp_timer_get_time() - t0 > (replays + 1) * 15000000) {
            ++replays;
            flight_log_replay();
        }

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
                // The largest single block, printed next to the total because
                // the two disagree in the way that matters: the BT controller
                // wants one contiguous 30,720-byte piece, and on 2026-08-28 it
                // failed with 33,059 bytes free. A total that looks sufficient
                // says nothing until this figure is next to it.
                size_t dma_big = heap_caps_get_largest_free_block(
                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                printf("ui: %u fps, view %s, gest %d, press %d, rel %d, indev %s @%d,%d, "
                       "lv pool %u%% used (%u free), dma free %u (largest %u), "
                       "stack spare drivelog %u serialcmd %u\n",
                       (unsigned)fps, gauge_ui::current_view_name(),
                       gauge_ui::gesture_count(), gauge_ui::press_count(),
                       gauge_ui::release_count(), st_s, (int)pt.x, (int)pt.y,
                       (unsigned)mm.used_pct, (unsigned)mm.free_size,
                       (unsigned)dma_free, (unsigned)dma_big,
                       (unsigned)drive_log_stack_headroom(),
                       (unsigned)serial_cmd_stack_headroom());
                // Repeated rather than printed once at the swipe: a serial
                // capture that opens after the swipe was losing it every time.
                printf("     %s\n", gauge_ui::slide_note());

                // What the car is actually saying. Until this line existed the
                // only evidence the live link worked was a PID count -- which
                // proves the car answered, not that we decoded it into
                // anything sane.
                if (live_mode) {
                    static int64_t last_rec = 0;
                    auto o = [&](const char* k) {
                        auto x = st.get(k);
                        return x ? *x : -1.0;
                    };
                    // Held back until the first reading actually lands. The
                    // switch to live happens the moment the car answers its
                    // PID sweep, which is before any value has arrived -- and
                    // a recorded line of "-1 -1 -1" reads as a dead link when
                    // it only means "one second early".
                    if (o("rpm") >= 0 &&
                        esp_timer_get_time() - last_rec > 30000000) {
                        last_rec = esp_timer_get_time();
                        flight_log("car rpm %.0f speed %.0f coolant %.0f volts %.1f, %u fps",
                                   o("rpm"), o("speed"), o("coolant"), o("volts"),
                                   (unsigned)fps);
                    }
                    auto v = [&](const char* k) {
                        auto o = st.get(k);
                        return o ? *o : -1.0;
                    };
                    printf("     car: rpm %.0f speed %.0f coolant %.0f intake %.0f "
                           "throttle %.0f load %.0f volts %.1f fuel %.2f (-1 = absent)\n",
                           v("rpm"), v("speed"), v("coolant"), v("intake"),
                           v("throttle"), v("load"), v("volts"), v("fuel_rate"));
                }
                bsp_display_lock(-1);
                // A sliding bar while the gauge is still looking for the car,
                // the car's name once it has found it. The frame rate stays in
                // this log line and is no longer written on the glass.
                gauge_ui::set_scanning(!live_mode);
                bsp_display_unlock();
            }
        }
        // 33 ms here capped the whole gauge at 30 updates a second, and the
        // needle only moves when this loop tells it to -- so the ease above had
        // 30 frames a second to work with at best, and fewer whenever a frame
        // ran long. 16 ms is one LVGL refresh period (CONFIG_LV_DEF_REFR_PERIOD
        // is 15), which is as often as the display can accept a change anyway.
        // Nothing else in this loop got more expensive: with no new readings
        // and no needle movement, LVGL finds nothing invalidated and draws
        // nothing.
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
