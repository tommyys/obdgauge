#include "live_link.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_transport.h"
#include "elm327.h"
#include "poll.h"

namespace live {
namespace {

QueueHandle_t      g_q = nullptr;
std::atomic<bool>  g_ready{false};
std::set<std::string> g_keys;
std::string        g_vin;
char               g_status[64] = "idle";
char               g_hint[32] = "vlinker";

void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    printf("live: %s\n", g_status);
}

double now_s() { return esp_timer_get_time() / 1e6; }

void push(const std::string& key, double value) {
    Sample s{};
    strncpy(s.key, key.c_str(), sizeof s.key - 1);
    s.value = static_cast<float>(value);
    s.t_s = now_s();
    // Dropped rather than blocked on: the radio must never stall waiting for
    // the UI, and a reading the UI is too busy to take is stale by the time it
    // would arrive anyway.
    xQueueSend(g_q, &s, 0);
}

void task(void*) {
    double backoff = 1.0;
    for (;;) {
        gauge_platform::BleTransport bt;
        set_status("scanning for '%s'", g_hint);
        if (bt.connect(g_hint, 20000)) {
            gauge::Elm327 elm(bt);
            set_status("connected to %s, handshaking", bt.peer_name());
            if (elm.init()) {
                g_vin = elm.read_vin();
                std::set<uint8_t> supported = elm.discover();
                std::set<std::string> keys = gauge::keys_for(supported);
                keys.insert("volts");
                g_keys = keys;
                g_ready = true;
                set_status("live: %d PIDs, vin %s", (int)supported.size(),
                           g_vin.empty() ? "n/a" : g_vin.c_str());

                // The display set, not the full sweep: a logging run wants
                // every PID, a gauge wants rpm and speed to stay current.
                std::vector<uint8_t> cycle =
                    gauge::build_poll_cycle(supported, /*log_all=*/false);
                backoff = 1.0;                  // a good connect resets backoff
                size_t i = 0;
                int misses = 0;
                double last_v = 0.0;
                while (bt.connected() && !cycle.empty()) {
                    uint8_t pid = cycle[i++ % cycle.size()];
                    auto data = elm.request(pid);
                    auto got = data ? gauge::decode(pid, *data)
                                    : std::optional<gauge::Reading>{};
                    if (got) {
                        push(got->key, got->value);
                        misses = 0;
                    } else if (++misses >= 12) {
                        // BLE still claims connected but the adapter has
                        // stopped answering -- a state it does get into. Only a
                        // fresh link clears it, so drop out and reconnect.
                        set_status("adapter stopped answering -- resetting");
                        break;
                    }
                    if (now_s() - last_v > 5.0) {
                        last_v = now_s();
                        if (auto v = elm.read_voltage()) push("volts", *v);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                set_status("'%s' would not handshake", bt.peer_name());
            }
        }
        // Deliberately left ready(): once a car has been seen, the views stay
        // on live data across a reconnect rather than snapping back to replay
        // and re-running the drive under the driver.
        vTaskDelay(pdMS_TO_TICKS((int)(backoff * 1000)));
        backoff = backoff * 1.6 > 10.0 ? 10.0 : backoff * 1.6;
    }
}

}  // namespace

void start(const char* name_hint) {
    if (g_q) return;
    strncpy(g_hint, name_hint && *name_hint ? name_hint : "vlinker",
            sizeof g_hint - 1);
    g_q = xQueueCreate(64, sizeof(Sample));
    // Off the UI core: the poll loop is mostly blocked on the car, but the
    // NimBLE host it wakes should not be competing with LVGL's render.
    xTaskCreatePinnedToCore(task, "live", 6144, nullptr, 4, nullptr, 0);
}

bool ready() { return g_ready.load(); }

bool next(Sample* out) {
    return g_q && xQueueReceive(g_q, out, 0) == pdTRUE;
}

const std::set<std::string>& keys() { return g_keys; }
const std::string& vin() { return g_vin; }
const char* status() { return g_status; }

}  // namespace live
