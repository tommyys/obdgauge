#include "flight_log.h"
#include "serial_cmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

// 1500, halved from 3000 on 2026-08-28. Two of these live in internal .bss
// (the current run and the previous one), and internal RAM is what the BT
// controller needs a contiguous 30 KB of -- see BleTransport::radio_init().
// A boot-to-live run writes about 10 lines of ~40 bytes, so 1500 still holds
// roughly 35 lines; the cap has never been reached in a real run. If a future
// run does fill it, the oldest lines are what is lost, and the boot reason
// plus the failure at the end are what matter.
constexpr size_t kCap = 1500;      // fits NVS comfortably, ~35 lines
const char* kNs = "flight";
const char* kKey = "log";

char g_buf[kCap];
char g_prev[kCap];
bool g_have_prev = false;
size_t g_len = 0;
nvs_handle_t g_nvs = 0;
bool g_open = false;
// The recorder is called from BOTH the UI loop and the BLE polling task, and
// neither the shared buffer nor an NVS handle may be used from two tasks at
// once. Without this the board went silent the first time a car reading and a
// link event landed together.
SemaphoreHandle_t g_lock = nullptr;

const char* reset_reason_name() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_INT_WDT:  return "INT WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "usb";
    default:               return "other";
    }
}

}  // namespace

extern "C" void flight_log_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK || nvs_open(kNs, NVS_READWRITE, &g_nvs) != ESP_OK) {
        printf("flight: no NVS -- this run will not be recorded\n");
        return;
    }
    g_open = true;
    g_lock = xSemaphoreCreateMutex();

    // Read back whatever the last run managed to write before losing power.
    size_t n = kCap - 1;
    if (nvs_get_blob(g_nvs, kKey, g_prev, &n) == ESP_OK && n > 0) {
        g_prev[n] = 0;
        g_have_prev = true;
        flight_log_replay();
    } else {
        printf("flight: no previous session recorded\n");
    }

    g_len = 0;
    g_buf[0] = 0;
    flight_log("boot, reset was %s", reset_reason_name());
}

extern "C" void flight_log_replay(void) {
    if (!g_have_prev) { printf("flight: no previous session recorded\n"); return; }
    printf("\n===== previous session (recorded with no console attached) =====\n"
           "%s"
           "===== end of previous session =====\n\n", g_prev);
}

extern "C" void flight_log(const char* fmt, ...) {
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    char line[160];
    const int64_t ms = esp_timer_get_time() / 1000;
    int p = snprintf(line, sizeof line, "[%3lld.%03llds] ", ms / 1000, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + p, sizeof line - p, fmt, ap);
    va_end(ap);
    const size_t need = strlen(line) + 1;

    // Full: drop whole lines from the front rather than truncating the newest.
    // A recorder that loses the end loses the moment it was recording for.
    while (g_len + need >= kCap) {
        char* nl = strchr(g_buf, '\n');
        if (!nl) { g_len = 0; g_buf[0] = 0; break; }
        const size_t drop = static_cast<size_t>(nl - g_buf) + 1;
        memmove(g_buf, g_buf + drop, g_len - drop + 1);
        g_len -= drop;
    }
    memcpy(g_buf + g_len, line, need - 1);
    g_len += need - 1;
    g_buf[g_len++] = '\n';
    g_buf[g_len] = 0;

    // Not into a GET dump -- the entry is still recorded to NVS above, only
    // the console copy is dropped. See serial_cmd_console_busy().
    if (!serial_cmd_console_busy()) printf("flight: %s\n", line);
    if (!g_open) { if (g_lock) xSemaphoreGive(g_lock); return; }
    // Committed on every line. Flash wear is a non-issue at a few dozen lines
    // a session, and the alternative is losing exactly the event that
    // preceded the power cut.
    nvs_set_blob(g_nvs, kKey, g_buf, g_len);
    nvs_commit(g_nvs);
    if (g_lock) xSemaphoreGive(g_lock);
}
