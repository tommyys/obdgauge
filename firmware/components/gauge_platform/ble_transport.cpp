#include "ble_transport.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace gauge_platform {
namespace {

const char* TAG = "ble";

// Nordic UART Service, the same three UUIDs mx5gauge/sources.py has been
// talking to from the Mac. NimBLE wants 128-bit UUIDs least-significant byte
// first, so these read backwards against the dashed form in sources.py.
#define NUS_BASE(x0, x1)                                                      \
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93,    \
                     0xf3, 0xa3, 0xb5, x0, x1, 0x40, 0x6e)
const ble_uuid128_t kNusWrite  = NUS_BASE(0x02, 0x00);
const ble_uuid128_t kNusNotify = NUS_BASE(0x03, 0x00);

constexpr int kMaxReply = 512;   // a supported-PID sweep reply is far under this
constexpr int kMinChunk = 20;    // pre-negotiation ATT payload, as sources.py uses

struct Link {
    // Radio brought up once per boot: NimBLE cannot be re-initialised cleanly,
    // and a retry after a failed scan must not try.
    bool radio_up = false;
    uint8_t own_addr_type = 0;

    uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
    uint16_t write_handle = 0;
    uint16_t notify_handle = 0;
    // Property-matched fallbacks, used only when neither NUS UUID turns up:
    // clones exist that expose the same two-characteristic shape under their
    // own UUIDs, and a gauge that refuses to talk to one is worse than a gauge
    // that guesses by properties.
    uint16_t any_write_handle = 0;
    uint16_t any_notify_handle = 0;

    char name[32] = {0};
    char hint[32] = {0};

    // Given when a reply reaches its '>' prompt; given by the NimBLE host task,
    // taken by the polling task.
    SemaphoreHandle_t reply = nullptr;
    // Given once the whole chain (scan -> connect -> discover -> subscribe) has
    // either finished or failed, so connect() has one thing to wait on.
    SemaphoreHandle_t ready = nullptr;
    bool ready_ok = false;

    SemaphoreHandle_t lock = nullptr;
    char buf[kMaxReply];
    int  buf_len = 0;
} g;

void signal_ready(bool ok) {
    // First outcome wins: a late disconnect must not un-fail a success, and a
    // second give() would leave the semaphore hot for the next connect().
    if (g.ready_ok || !g.ready) {
        if (ok) g.ready_ok = true;
        return;
    }
    g.ready_ok = ok;
    xSemaphoreGive(g.ready);
}

// Each distinct name once per scan, now that duplicate filtering is off and
// the same device reports several times a second.
char g_seen[12][32];
int  g_seen_n = 0;

int g_reports = 0;
char g_anon[16][18];
int  g_anon_n = 0;

void log_anon(const uint8_t* addr, int8_t rssi) {
    char key[18];
    snprintf(key, sizeof key, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    for (int i = 0; i < g_anon_n; ++i)
        if (strcmp(g_anon[i], key) == 0) return;
    if (g_anon_n < 16) {
        strncpy(g_anon[g_anon_n], key, sizeof g_anon[0] - 1);
        g_anon[g_anon_n][sizeof g_anon[0] - 1] = 0;
        ++g_anon_n;
    }
    ESP_LOGI(TAG, "  saw %s (no name) rssi %d", key, rssi);
}

void log_seen(const char* nm) {
    for (int i = 0; i < g_seen_n; ++i)
        if (strcmp(g_seen[i], nm) == 0) return;
    if (g_seen_n < 12) {
        strncpy(g_seen[g_seen_n], nm, sizeof g_seen[0] - 1);
        g_seen[g_seen_n][sizeof g_seen[0] - 1] = 0;
        ++g_seen_n;
    }
    ESP_LOGI(TAG, "  saw '%s'", nm);
}

bool contains_ci(const char* hay, int hay_len, const char* needle) {
    const int n = static_cast<int>(strlen(needle));
    if (n == 0) return true;
    for (int i = 0; i + n <= hay_len; ++i) {
        int j = 0;
        while (j < n && std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                            std::tolower(static_cast<unsigned char>(needle[j])))
            ++j;
        if (j == n) return true;
    }
    return false;
}

// ---- discovery -----------------------------------------------------------

int on_subscribe(uint16_t conn, const struct ble_gatt_error* err,
                 struct ble_gatt_attr*, void*) {
    if (conn != g.conn) return 0;
    if (err && err->status != 0) {
        ESP_LOGE(TAG, "subscribe to notifications failed: %d", err->status);
        signal_ready(false);
        return 0;
    }
    ESP_LOGI(TAG, "linked to '%s': write handle %u, notify handle %u",
             g.name, g.write_handle, g.notify_handle);
    signal_ready(true);
    return 0;
}

void subscribe() {
    if (!g.write_handle) g.write_handle = g.any_write_handle;
    if (!g.notify_handle) g.notify_handle = g.any_notify_handle;
    if (!g.write_handle || !g.notify_handle) {
        ESP_LOGE(TAG, "'%s' has no write+notify pair -- not an ELM327 adapter?",
                 g.name);
        signal_ready(false);
        return;
    }
    // The CCCD sits immediately after its characteristic value on every adapter
    // in the wild, which is why we can skip a whole descriptor-discovery round.
    const uint8_t on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(g.conn, g.notify_handle + 1, on, sizeof on,
                                 on_subscribe, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write failed to start: %d", rc);
        signal_ready(false);
    }
}

int on_chr(uint16_t conn, const struct ble_gatt_error* err,
           const struct ble_gatt_chr* chr, void*) {
    if (conn != g.conn) return 0;
    if (err && err->status == BLE_HS_EDONE) { subscribe(); return 0; }
    if (err && err->status != 0) {
        ESP_LOGE(TAG, "characteristic discovery failed: %d", err->status);
        signal_ready(false);
        return 0;
    }
    if (!chr) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &kNusWrite.u) == 0)
        g.write_handle = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &kNusNotify.u) == 0)
        g.notify_handle = chr->val_handle;

    if (!g.any_notify_handle && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY))
        g.any_notify_handle = chr->val_handle;
    if (!g.any_write_handle &&
        (chr->properties &
         (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)))
        g.any_write_handle = chr->val_handle;
    return 0;
}

// ---- GAP -----------------------------------------------------------------

int gap_event(struct ble_gap_event* event, void* arg);

void begin_scan() {
    struct ble_gap_disc_params p = {};
    p.passive = 0;                 // active: the name is in the scan response
    // Duplicate filtering OFF, and this is the whole reason the car test found
    // nothing. A device's name usually arrives in the SCAN RESPONSE, not the
    // advertisement -- and the controller's duplicate filter treats that
    // response as a repeat of the advertisement it just reported, and drops
    // it. So every device whose name is not in the first packet was invisible:
    // the scan saw them and discarded exactly the field we match on. The cost
    // of turning it off is repeat events, which log_seen() below absorbs.
    p.filter_duplicates = 0;
    // 30 ms of listening in every 100 ms, rather than NimBLE's default of
    // listening continuously. A continuous scan keeps the radio and its
    // interrupts busy against a UI that is already only managing 10-20 fps,
    // and costs frames for nothing: an adapter advertises many times a second,
    // so a 30% duty cycle still finds it within a second or two.
    p.itvl = 160;                  // 160 * 0.625 ms = 100 ms
    p.window = 48;                 //  48 * 0.625 ms =  30 ms
    int rc = ble_gap_disc(g.own_addr_type, BLE_HS_FOREVER, &p, gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan failed to start: %d", rc);
        signal_ready(false);
        return;
    }
    g_seen_n = 0;
    g_anon_n = 0;
    g_reports = 0;
    ESP_LOGI(TAG, "scanning for an adapter matching '%s'", g.hint);
}

int gap_event(struct ble_gap_event* event, void*) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // Every report, named or not, with its address and signal strength.
        // "no adapter found" and "the radio is hearing nothing" look identical
        // when only named devices are logged, and that ambiguity is what made
        // the first car test unreadable -- a scan that reports zero named
        // devices may be working perfectly in a quiet place, or may be deaf.
        ++g_reports;
        struct ble_hs_adv_fields f;
        bool parsed = ble_hs_adv_parse_fields(&f, event->disc.data,
                                              event->disc.length_data) == 0;
        if (!parsed || !f.name || f.name_len == 0) {
            log_anon(event->disc.addr.val, event->disc.rssi);
            return 0;
        }
        char nm[32];
        int n = f.name_len < (int)sizeof nm - 1 ? f.name_len : (int)sizeof nm - 1;
        memcpy(nm, f.name, n);
        nm[n] = 0;
        // Every named device is logged, not just the match. The Mac side does
        // the same (README "the error lists every visible Bluetooth device"):
        // when the adapter is asleep or renamed, this list is the only way to
        // tell "nothing is advertising" from "it is there under another name".
        log_seen(nm);
        if (!contains_ci(nm, n, g.hint)) return 0;

        strncpy(g.name, nm, sizeof g.name - 1);
        ble_gap_disc_cancel();
        int rc = ble_gap_connect(g.own_addr_type, &event->disc.addr, 10000,
                                 nullptr, gap_event, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect to '%s' failed to start: %d", nm, rc);
            signal_ready(false);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect failed: %d -- scanning again",
                     event->connect.status);
            g.conn = BLE_HS_CONN_HANDLE_NONE;
            begin_scan();
            return 0;
        }
        g.conn = event->connect.conn_handle;
        // Ask for the bigger MTU before any command: a supported-PID reply that
        // spans packets still parses, but one that does not is one less thing
        // to have gone wrong on the first live link.
        ble_gattc_exchange_mtu(g.conn, nullptr, nullptr);
        // Discovered over the whole handle range rather than inside the NUS
        // service: clones that move the characteristics out of it are common,
        // and the UUID match above still finds them wherever they sit.
        if (ble_gattc_disc_all_chrs(g.conn, 1, 0xffff, on_chr, nullptr) != 0) {
            ESP_LOGE(TAG, "characteristic discovery failed to start");
            signal_ready(false);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "link to '%s' dropped: reason %d", g.name,
                 event->disconnect.reason);
        g.conn = BLE_HS_CONN_HANDLE_NONE;
        g.write_handle = g.notify_handle = 0;
        // Wake a reader that is mid-command so the poll loop sees the drop
        // immediately instead of after its 4 s timeout.
        if (g.reply) xSemaphoreGive(g.reply);
        signal_ready(false);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != g.conn) return 0;
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        char tmp[128];
        if (len > sizeof tmp) len = sizeof tmp;
        if (os_mbuf_copydata(event->notify_rx.om, 0, len, tmp) != 0) return 0;
        bool prompt = false;
        xSemaphoreTake(g.lock, portMAX_DELAY);
        for (uint16_t i = 0; i < len; ++i) {
            if (g.buf_len < kMaxReply) g.buf[g.buf_len++] = tmp[i];
            if (tmp[i] == '>') prompt = true;
        }
        xSemaphoreGive(g.lock);
        // '>' is the ELM327's prompt: the reply is complete, nothing more is
        // coming, and waiting on a byte count instead would stall forever.
        if (prompt) xSemaphoreGive(g.reply);
        return 0;
    }
    default:
        return 0;
    }
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &g.own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        signal_ready(false);
        return;
    }
    begin_scan();
}

void on_reset(int reason) {
    ESP_LOGE(TAG, "controller reset: %d", reason);
    g.conn = BLE_HS_CONN_HANDLE_NONE;
    signal_ready(false);
}

void host_task(void*) {
    nimble_port_run();            // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

}  // namespace

bool BleTransport::connect(const char* name_hint, int timeout_ms) {
    if (!g.lock) {
        g.lock = xSemaphoreCreateMutex();
        g.reply = xSemaphoreCreateBinary();
        g.ready = xSemaphoreCreateBinary();
    }
    strncpy(g.hint, name_hint ? name_hint : "", sizeof g.hint - 1);
    g.ready_ok = false;
    xSemaphoreTake(g.ready, 0);   // drain a stale outcome from a past attempt

    if (!g.radio_up) {
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        if (nimble_port_init() != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE would not start");
            return false;
        }
        nimble_port_freertos_init(host_task);
        g.radio_up = true;        // one init per boot; a retry re-scans instead
    } else if (ble_hs_synced()) {
        begin_scan();
    }

    if (xSemaphoreTake(g.ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "no adapter matching '%s' after %d ms -- %d reports, "
                      "%d named, %d unnamed",
                 g.hint, timeout_ms, g_reports, g_seen_n, g_anon_n);
        ble_gap_disc_cancel();
        return false;
    }
    return g.ready_ok && connected();
}

bool BleTransport::connected() const {
    return g.conn != BLE_HS_CONN_HANDLE_NONE && g.write_handle && g.notify_handle;
}

void BleTransport::disconnect() {
    if (g.conn == BLE_HS_CONN_HANDLE_NONE) return;
    // Without this the link stays open after we give up on the car, and a
    // connected peripheral does not advertise -- so every later scan found
    // nothing and we could never get back to the adapter we were still
    // holding. Observed in the car on the first live link.
    ble_gap_terminate(g.conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int i = 0; i < 50 && g.conn != BLE_HS_CONN_HANDLE_NONE; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
}

const char* BleTransport::peer_name() const { return g.name; }

bool BleTransport::write(const std::string& text) {
    if (!connected()) return false;
    // A command starts a fresh reply. Dropping whatever is in the buffer here
    // rather than after reading it means a timed-out command's late reply
    // cannot be mistaken for the next command's answer -- which is how an
    // ELM327 link silently reports the wrong PID's value.
    xSemaphoreTake(g.lock, portMAX_DELAY);
    g.buf_len = 0;
    xSemaphoreGive(g.lock);
    xSemaphoreTake(g.reply, 0);

    const std::string payload = text + "\r";
    int chunk = ble_att_mtu(g.conn) - 3;
    if (chunk < kMinChunk) chunk = kMinChunk;
    for (size_t i = 0; i < payload.size(); i += chunk) {
        const size_t n = std::min<size_t>(chunk, payload.size() - i);
        int rc = ble_gattc_write_no_rsp_flat(g.conn, g.write_handle,
                                            payload.data() + i, n);
        if (rc != 0) {
            ESP_LOGE(TAG, "write of '%s' failed: %d", text.c_str(), rc);
            return false;
        }
    }
    return true;
}

std::string BleTransport::read(int timeout_ms) {
    if (xSemaphoreTake(g.reply, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return "";
    xSemaphoreTake(g.lock, portMAX_DELAY);
    std::string out(g.buf, g.buf_len);
    g.buf_len = 0;
    xSemaphoreGive(g.lock);
    return out;
}

void BleTransport::delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

}  // namespace gauge_platform
