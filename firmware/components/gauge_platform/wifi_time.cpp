#include "wifi_time.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace gauge_platform {
namespace wifi_time {
namespace {

constexpr const char* TAG = "wifi_time";

// The two outcomes a join can have. A disconnect is a failure of that network
// rather than something to retry here: the plan owns retrying, and the driver
// re-associating on its own would spend another network's budget.
constexpr int kGotIp  = BIT0;
constexpr int kFailed = BIT1;

struct {
    Config cfg;
    const gauge::WifiNetwork* nets = nullptr;
    std::size_t n = 0;

    volatile bool started = false;
    volatile bool busy = false;
    // Whether the radio -- and the contiguous internal RAM it holds -- is
    // still ours. `busy` is not this: the task can be alive with the radio
    // down, and the boot only cares about the memory. See wait().
    volatile bool radio_is_up = false;
    uint32_t epoch = 0;
    char status[128] = "wifi: not started";

    esp_netif_t* netif = nullptr;
    EventGroupHandle_t evt = nullptr;
    esp_event_handler_instance_t h_any = nullptr;
    esp_event_handler_instance_t h_ip = nullptr;
} g;

void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.status, sizeof g.status, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", g.status);
    if (g.cfg.log) g.cfg.log(g.status);
}

// The number that matters is not how much internal RAM is free but how big
// its largest hole is: the BT controller needs one contiguous 30,720 bytes
// and the carousel's blit band another 29,824. A healthy total with a
// shattered heap is exactly the state that broke the BLE link on 2026-08-28,
// so both are logged at every point where WiFi changes the picture.
void note_heap(const char* when) {
    note("wifi: %s, internal free %u largest %u", when,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Why the last attempt was refused. Kept because "it would not accept us"
// covers both a wrong password and a hotspot that is simply switched off,
// which are entirely different things to go and fix. 201 is NO_AP_FOUND --
// nothing of that name on the air -- and 15 is a bad password.
volatile uint8_t g_reason = 0;

const char* reason_text(uint8_t r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND:        return "not on the air";
        case WIFI_REASON_AUTH_FAIL:          return "wrong password";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "wrong password";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "handshake timed out";
        case WIFI_REASON_ASSOC_FAIL:         return "refused the association";
        case WIFI_REASON_CONNECTION_FAIL:    return "connection failed";
        default:                             return "refused us";
    }
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (!g.evt) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        g_reason = d ? d->reason : 0;
        xEventGroupSetBits(g.evt, kFailed);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g.evt, kGotIp);
    }
}

bool radio_up() {
    // Both of these are process-wide and may already exist -- the console and
    // the BSP create them in some configs -- so an ALREADY_INIT/INVALID_STATE
    // is success, not a failure to bail on.
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        note("wifi: netif init failed (%s)", esp_err_to_name(e));
        return false;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        note("wifi: event loop failed (%s)", esp_err_to_name(e));
        return false;
    }

    g.netif = esp_netif_create_default_wifi_sta();
    if (!g.netif) { note("wifi: no station netif"); return false; }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&ic);
    if (e != ESP_OK) {
        // The likely reason is memory, and the likely memory is the block the
        // panel and the BT controller are already holding. One attempt, then
        // out: retrying an allocation that failed for want of a contiguous
        // block is what fragments the pool further.
        note("wifi: init failed (%s)", esp_err_to_name(e));
        esp_netif_destroy_default_wifi(g.netif);
        g.netif = nullptr;
        return false;
    }

    // RAM, not flash: the credentials are compiled in, so there is nothing
    // worth persisting, and this keeps WiFi out of the NVS partition the
    // flight log and the recorder rely on.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event,
                                        nullptr, &g.h_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event,
                                        nullptr, &g.h_ip);
    esp_wifi_set_mode(WIFI_MODE_STA);
    e = esp_wifi_start();
    if (e != ESP_OK) { note("wifi: start failed (%s)", esp_err_to_name(e)); return false; }
    // Modem sleep between beacons. The gauge is on car power so this is not
    // about battery: it hands radio time back, which is the resource the OBD
    // link cares about if the keep-alive flag is ever left on.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    g.radio_is_up = true;
    return true;
}

void radio_down() {
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (g.h_any) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g.h_any);
        g.h_any = nullptr;
    }
    if (g.h_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g.h_ip);
        g.h_ip = nullptr;
    }
    esp_wifi_deinit();
    if (g.netif) {
        esp_netif_destroy_default_wifi(g.netif);
        g.netif = nullptr;
    }
    // Last, and only here: the boot is watching this flag to know the memory
    // is back before it starts the display.
    g.radio_is_up = false;
}

bool join(const gauge::WifiNetwork& net, int timeout_ms) {
    wifi_config_t wc = {};
    std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof wc.sta.ssid, "%s",
                  net.ssid ? net.ssid : "");
    std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof wc.sta.password,
                  "%s", net.pass ? net.pass : "");
    // An open network is allowed, but only if the entry actually has no
    // password: without this the driver would silently accept an open
    // impostor advertising your hotspot's name.
    wc.sta.threshold.authmode =
        (net.pass && net.pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    g_reason = 0;
    xEventGroupClearBits(g.evt, kGotIp | kFailed);
    const esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) { note("wifi: connect '%s' refused (%s)", wc.sta.ssid, esp_err_to_name(e)); return false; }

    const EventBits_t bits = xEventGroupWaitBits(
        g.evt, kGotIp | kFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & kGotIp) return true;
    // Leave nothing associating in the background: the next network's attempt
    // must start from a known state.
    esp_wifi_disconnect();
    if (bits & kFailed)
        note("wifi: '%s' %s (reason %u)", wc.sta.ssid, reason_text(g_reason),
             (unsigned)g_reason);
    else
        note("wifi: '%s' found but gave us no address in time", wc.sta.ssid);
    return false;
}

// What the radio can actually see, printed once when nothing on the list was
// found.
//
// "not on the air" is true but unhelpful, and on 2026-09-04 it sent us looking
// for a firmware fault that was not there. This board's radio is 2.4 GHz ONLY,
// and the Mac sitting next to it was on 5 GHz -- so "the network is right
// there" and "the gauge can see it" are different claims, and only this one
// settles which. It also answers the other question that came up: whether the
// BT controller, which is already up and holding the shared 2.4 GHz front end
// by the time this runs, is stopping the scan from seeing anything at all.
//
// Cheap and bounded: one passive sweep, at most eight results, and only on the
// boot where the clock was not set anyway.
void log_visible_networks() {
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    // Blocking, because there is nothing else to do with this boot's radio
    // window once every network on the list has failed.
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        note("wifi: could not scan to see what IS on the air");
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        note("wifi: scan saw NO 2.4 GHz networks at all -- this radio is "
             "2.4 GHz only, and the BT controller shares its front end");
        return;
    }
    uint16_t want = n > 8 ? 8 : n;
    wifi_ap_record_t recs[8] = {};
    if (esp_wifi_scan_get_ap_records(&want, recs) != ESP_OK) return;
    char line[240];
    int at = std::snprintf(line, sizeof line, "wifi: %u on the air:", (unsigned)n);
    for (uint16_t i = 0; i < want && at > 0 && at < (int)sizeof line; ++i)
        at += std::snprintf(line + at, sizeof line - at, " %s(%d,ch%u)",
                            reinterpret_cast<const char*>(recs[i].ssid),
                            (int)recs[i].rssi, (unsigned)recs[i].primary);
    note("%s", line);
}

// Ask, sanity-check, and report. Returns 0 when there is nothing to trust.
uint32_t ask_time(int timeout_ms) {
    // Two servers rather than one, and a real allowance. Measured on the
    // board 2026-09-03: joined to home WiFi at -73 dBm, a single
    // pool.ntp.org did not answer inside three seconds -- the DNS lookup and
    // the exchange both have to fit, on a link the gauge does not control.
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    if (esp_netif_sntp_init(&sc) != ESP_OK) { note("wifi: sntp would not start"); return 0; }
    const esp_err_t e = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    esp_netif_sntp_deinit();
    if (e != ESP_OK) { note("wifi: no time from pool.ntp.org"); return 0; }

    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    const uint32_t ep = static_cast<uint32_t>(tv.tv_sec);
    if (!gauge::wifi_epoch_believable(ep)) {
        // SNTP has already moved the system clock by this point; what is
        // refused is letting it near the drive stamps, which are permanent.
        note("wifi: refused an unbelievable clock (%u)", (unsigned)ep);
        return 0;
    }
    return ep;
}

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// How much longer than its own budget the boot will wait for the radio to
// hand its memory back. Long enough for the tail of a timed-out attempt --
// esp_wifi_stop() and deinit -- and short enough that a wedged driver delays
// the gauge rather than hanging it.
constexpr int kRadioReleaseGraceMs = 3000;

void task(void*) {
    gauge::WifiPlan plan(g.n, g.cfg.plan);
    bool up = false;

    for (;;) {
        const int idx = plan.next(now_ms());
        if (idx == gauge::WifiPlan::kDone) break;
        if (idx == gauge::WifiPlan::kWait) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        if (!up) {
            note_heap("bringing the radio up");
            if (!radio_up()) break;
            up = true;
            note_heap("radio up");
        }

        const int budget = plan.attempt_timeout_ms(now_ms());
        const gauge::WifiNetwork& net = g.nets[idx];
        if (join(net, budget)) {
            const uint32_t ep = ask_time(g.cfg.sntp_timeout_ms);
            if (ep) {
                plan.succeeded(now_ms());
                g.epoch = ep;
                if (g.cfg.on_time) g.cfg.on_time(ep);
                char when[32] = "?";
                const time_t t = static_cast<time_t>(ep);
                struct tm tm_v;
                if (localtime_r(&t, &tm_v)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm_v);
                note("wifi: clock set to %s from '%s'", when, net.ssid);
            } else {
                plan.failed(now_ms());
            }
        } else {
            plan.failed(now_ms());
        }

        if (up && !plan.radio_wanted(now_ms())) {
            // Last chance to look around, while the radio is still ours and
            // only when nothing on the list worked. This is inside the loop
            // because the radio goes down HERE, not at the tail of the
            // function -- put there, it never ran once.
            //
            // Skipped once the boot budget is spent, which is the only case
            // where anything is waiting on this. app_main cannot start the
            // display until the radio hands its memory back, and a blocking
            // scan takes one to two seconds MORE on the boot that has already
            // used its whole allowance. Before this, the boot walked out
            // mid-scan and the panel's own allocation then failed -- an
            // assert in bsp_display_lcd_init() and a reboot, on the boots
            // where WiFi failed slowly. The retry pass minutes later gets a
            // fresh budget and does log the scan; nothing is waiting then.
            if (!plan.synced() && plan.attempt_timeout_ms(now_ms()) > 0)
                log_visible_networks();
            radio_down();
            up = false;
            note_heap("radio released");
        }
    }

    if (up) {
        if (g.cfg.plan.keep_up) {
            note("wifi: staying up (keep-alive flag set)");
        } else {
            radio_down();
            note_heap("radio released");
        }
    }
    if (!plan.synced() && g.n)
        note("wifi: no clock this boot -- drives stay 'date unknown'");

    g.busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

void start(const gauge::WifiNetwork* nets, std::size_t n, const Config& cfg) {
    if (g.started) return;
    g.started = true;
    g.cfg = cfg;
    g.nets = nets;
    g.n = n;
    if (n == 0) {
        // Nothing to do, and busy() stays false so main.cpp's gate on the OBD
        // scan is a no-op on a clone with no wifi_creds.h.
        note("wifi: no networks compiled in (see main/wifi_creds.h.example)");
        return;
    }
    g.evt = xEventGroupCreate();
    if (!g.evt) { note("wifi: no event group"); return; }
    g.busy = true;
    // Priority 2: above nothing that draws and below the recorder (3), which
    // must never wait on a radio. The task blocks in the driver's event group
    // almost all of its life, so it costs the run queue nothing.
    if (xTaskCreate(task, "wifi_time", 5120, nullptr, 2, nullptr) != pdPASS) {
        note("wifi: task would not start");
        g.busy = false;
    }
}

void wait(int max_ms) {
    const int64_t deadline = now_ms() + max_ms;
    while (g.busy && now_ms() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    if (g.busy) note("wifi: gave up waiting after %d ms", max_ms);
    // And then for the radio itself. The caller's reason for waiting is not
    // the clock, it is the memory: the display cannot be started while the
    // radio holds its contiguous internal RAM (main.cpp says so where it
    // calls this). Waiting on `busy` alone let the boot continue mid-attempt
    // with the radio still up -- 72,395 bytes free, largest block 31,744,
    // against 137,047 and 63,488 once released -- and the panel's allocation
    // then failed hard: assert in bsp_display_lcd_init(), reboot, retry.
    //
    // A short grace on top, not another full budget: with the diagnostic
    // scan skipped above, the release is the tail of an attempt that has
    // already timed out, so this is milliseconds in practice.
    if (g.cfg.plan.keep_up) return;
    const int64_t radio_deadline = now_ms() + kRadioReleaseGraceMs;
    while (g.radio_is_up && now_ms() < radio_deadline) vTaskDelay(pdMS_TO_TICKS(20));
    if (g.radio_is_up)
        note("wifi: radio STILL UP after %d ms of grace -- the display may "
             "not find the memory it needs", kRadioReleaseGraceMs);
}

bool busy() { return g.busy; }
uint32_t epoch() { return g.epoch; }
const char* status() { return g.status; }

}  // namespace wifi_time
}  // namespace gauge_platform
