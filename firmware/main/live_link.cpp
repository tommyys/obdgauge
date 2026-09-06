#include "live_link.h"
#include "serial_cmd.h"

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
#include "esp_heap_caps.h"
#include "flight_log.h"

namespace live {
namespace {

QueueHandle_t      g_q = nullptr;
TaskHandle_t       g_task = nullptr;   // parked by reserve(), woken by start()
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
    // Held back while a GET dump streams -- see serial_cmd_console_busy().
    // g_status still updates, so the LIVE view is unaffected; only the console
    // copy is dropped, and a dropped line costs three records of a pull.
    if (!serial_cmd_console_busy()) printf("live: %s\n", g_status);
    flight_log("%s", g_status);
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
    // Parked until start() says go. The stack is already claimed -- that is the
    // whole point of being created this early -- but nothing touches the radio
    // until the views have settled. See reserve() in live_link.h.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    double backoff = 1.0;
    for (;;) {
        gauge_platform::BleTransport bt;
        set_status("scanning for '%s'", g_hint);
        if (bt.connect(g_hint, 20000)) {
            gauge::Elm327 elm(bt);
            set_status("connected to %s, handshaking", bt.peer_name());
            if (elm.init()) {
                g_vin = elm.read_vin();
                // Retried, because an empty supported-PID sweep is usually the
                // car not being awake yet rather than a car with nothing to
                // say -- ATSP0 has to negotiate a protocol, and with the
                // ignition just turned on the first sweep can land before the
                // bus does. Dropping the link over that costs a full rescan
                // and a reconnect for something that answers on the next try.
                std::set<uint8_t> supported;
                for (int attempt = 1; attempt <= 5 && bt.connected(); ++attempt) {
                    supported = elm.discover();
                    if (!supported.empty()) break;
                    set_status("no PIDs yet (try %d/5) -- is the ignition on?",
                               attempt);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                std::set<std::string> keys = gauge::keys_for(supported);
                keys.insert("volts");
                g_keys = keys;
                g_ready = true;
                set_status("live: %d PIDs, vin %s", (int)supported.size(),
                           g_vin.empty() ? "n/a" : g_vin.c_str());
                // The car's actual capability, once, in full. Task 14 step 4
                // asks for this to be checked against what the simulator
                // reports for the same car, and a count cannot be compared.
                std::string list;
                for (const auto& k : g_keys) { list += k; list += ' '; }
                printf("live: channels: %s\n", list.c_str());

                // Every PID the car supports, not just the display set: the
                // recorder wants all of it, and build_poll_cycle keeps
                // kPollFast in front of each one so the needle does not
                // notice (design s3).
                std::vector<uint8_t> cycle =
                    gauge::build_poll_cycle(supported, /*log_all=*/true);
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
            // Always hand the adapter back before retrying. See
            // BleTransport::disconnect().
            bt.disconnect();
        }
        // Deliberately left ready(): once a car has been seen, the views stay
        // on live data across a reconnect rather than snapping back to replay
        // and re-running the drive under the driver.
        vTaskDelay(pdMS_TO_TICKS((int)(backoff * 1000)));
        backoff = backoff * 1.6 > 10.0 ? 10.0 : backoff * 1.6;
    }
}


// Both halves of getting the task running, so the failure path is in one place.
bool spawn() {
    if (!g_q) g_q = xQueueCreate(64, sizeof(Sample));
    if (!g_q) {
        printf("live: QUEUE FAILED -- no OBD link this run\n");
        flight_log("live: queue FAILED -- no OBD link this run");
        return false;
    }
    if (g_task) return true;
    // Off the UI core: the poll loop is mostly blocked on the car, but the
    // NimBLE host it wakes should not be competing with LVGL's render.
    //
    // The result is CHECKED. It was not, and on this board an unchecked
    // xTaskCreate is how a feature disappears in silence -- it hid the loss of
    // the whole OBD link for a day, and the same thing had already starved
    // serial_cmd's stack when WiFi was linked in.
    const BaseType_t ok =
        xTaskCreatePinnedToCore(task, "live", 6144, nullptr, 4, &g_task, 0);
    if (ok != pdPASS || !g_task) {
        g_task = nullptr;
        printf("live: TASK CREATE FAILED (%d) -- the car will never be found\n",
               (int)ok);
        flight_log("live: task create FAILED -- no OBD link this run");
        return false;
    }
    return true;
}

}  // namespace

void reserve() {
    // Both heap figures, because they disagree in the way that matters here:
    // the task stack comes from internal RAM, the panel's SPI buffers from the
    // DMA-capable subset of it, and a total that looks sufficient says nothing
    // until the largest block is next to it.
    printf("live: reserving the task -- internal free %u largest %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (spawn()) flight_log("live: task reserved");
}

void start(const char* name_hint) {
    strncpy(g_hint, name_hint && *name_hint ? name_hint : "vlinker",
            sizeof g_hint - 1);
    // Late creation is the fallback, not the path: if reserve() was never
    // called or could not get the memory, this is the old behaviour, and it
    // now says so when it fails instead of going quiet.
    if (!g_task && !spawn()) return;
    xTaskNotifyGive(g_task);
}


bool ready() { return g_ready.load(); }

bool next(Sample* out) {
    return g_q && xQueueReceive(g_q, out, 0) == pdTRUE;
}

const std::set<std::string>& keys() { return g_keys; }
const std::string& vin() { return g_vin; }
const char* status() { return g_status; }

}  // namespace live
