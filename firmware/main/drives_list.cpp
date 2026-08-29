// Feeds the Drives view from the flash ring.
//
// The view runs on the LVGL draw task and must never block. Every flash touch
// -- listing drives, folding a drive's records into its four numbers -- is
// done here, on a task of its own on core 0, and the view reads a cache.
//
// A scan of a 27-minute drive is 688 KB off flash. That is well under a
// second, but "well under a second" on the draw task is still a visibly
// dropped frame, and it takes the recorder's lock while it runs.
#include "drives_list.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "drive_log.h"
#include "drive_stats.h"
#include "drives.h"
#include "flight_log.h"
#include "logbuf.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

// The newest drives, which is what a list is for. The ring can hold more; the
// view shows the newest and the console still reaches every one of them.
constexpr int kMaxCached = 12;
// How often the list itself is refreshed. Drives open and close on the scale
// of minutes, so re-listing faster than this is flash traffic for nothing.
constexpr int kRelistMs = 5000;

struct Entry {
    gauge::DriveInfo  info{};
    gauge::DriveStats stats{};
    bool              ready = false;
};

// Dates lent to drives that recorded without a clock.
//
// The epoch lives in the drive's start marker on flash, and flash bits only
// go one way without an erase: a marker written as 0 cannot be rewritten to
// 1,787,000,000 in place, and erasing to fix it would take the drive with it.
// So a lent date is kept beside the ring in NVS, keyed by drive id, and
// applied when the drive is listed. The records are never touched.
constexpr const char* kDateNs = "drivedate";

void date_key(char* out, size_t n, uint32_t id) { snprintf(out, n, "%u", (unsigned)id); }

uint32_t lent_date(uint32_t id) {
    nvs_handle_t h;
    if (nvs_open(kDateNs, NVS_READONLY, &h) != ESP_OK) return 0;
    char key[16];
    date_key(key, sizeof key, id);
    uint32_t epoch = 0;
    if (nvs_get_u32(h, key, &epoch) != ESP_OK) epoch = 0;
    nvs_close(h);
    return epoch;
}

SemaphoreHandle_t g_mutex = nullptr;
Entry             g_cache[kMaxCached];
int               g_count = 0;
const char*       g_empty = "no drives recorded yet";

// Folds one drive's records. Runs on the scan task, holding the recorder's
// lock, and is handed whole sectors at a time by read_drive().
bool fold_sink(const gauge::Record* records, size_t count, void* ctx) {
    static_cast<gauge::DriveStatsFold*>(ctx)->add(records, count);
    return true;
}

bool scan_one(gauge::LogBuf* log, uint32_t id, gauge::DriveStats* out) {
    gauge::DriveStatsFold fold;
    if (!log->read_drive(id, fold_sink, &fold)) return false;
    *out = fold.result();
    return true;
}

void relist(gauge::LogBuf* log) {
    // Static, not stack: list()'s 64-entry accumulator and the rebuilt cache
    // are 2.4 KB between them, and this task's frame also has to hold
    // read_drive()'s 4,080-byte sector buffer. Only the scan task runs this.
    static gauge::DriveInfo found[gauge::kListCapacity];
    static Entry next[kMaxCached];
    const size_t n = log->list(found, gauge::kListCapacity);

    // Keep what was already folded: ids are stable, so a re-list must not
    // throw away scans and make the view say "reading..." every five seconds.
    for (Entry& e : next) e = Entry{};
    int count = 0;
    for (size_t i = 0; i < n && count < kMaxCached; ++i) {
        next[count].info = found[i];
        // A drive that recorded with no clock can be given one afterwards.
        // Only ever fills a gap: a date the drive recorded for itself is the
        // better one and is never overwritten.
        if (!next[count].info.epoch_s) next[count].info.epoch_s = lent_date(found[i].id);
        for (int j = 0; j < g_count; ++j) {
            if (g_cache[j].info.id != found[i].id || !g_cache[j].ready) continue;
            // A drive still being WRITTEN grows, so its numbers are re-folded
            // rather than kept. `complete` is not that test: a drive cut short
            // by a power cut is incomplete for ever, and re-folding it meant
            // re-reading 688 KB of flash every five seconds, for ever, for an
            // answer that could not change. Only the open drive is re-folded.
            if (g_cache[j].info.id == log->current_drive()) break;
            next[count].stats = g_cache[j].stats;
            next[count].ready = true;
            break;
        }
        ++count;
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    memcpy(g_cache, next, sizeof g_cache);
    const bool changed = g_count != count;
    g_count = count;
    xSemaphoreGive(g_mutex);
    // Only when it moves: this runs every five seconds for the life of the
    // gauge. list() reporting fewer drives than the ring holds is the failure
    // that would leave the view empty with the records still on flash.
    if (changed) flight_log("drives: list has %d of %u held", count, (unsigned)n);
}

void scan_task(void*) {
    TickType_t last_list = 0;
    for (;;) {
        gauge::LogBuf* log = drive_log_buf();
        if (!log) {
            g_empty = "recorder not available";
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        const bool due = last_list == 0 || (now - last_list) >= pdMS_TO_TICKS(kRelistMs);

        // One unfolded drive per pass, newest first, so the top of the list
        // fills in first -- that is the row being looked at.
        uint32_t want_id = 0;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        for (int i = 0; i < g_count; ++i) {
            if (g_cache[i].ready) continue;
            if (g_cache[i].info.table_version != gauge::kChanTableVersion) continue;
            want_id = g_cache[i].info.id;
            break;
        }
        xSemaphoreGive(g_mutex);

        if (!due && !want_id) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        // 30 s: the same wait serial_cmd uses. A GET streaming a whole drive
        // out of the console holds this lock for as long as it takes.
        if (!drive_log_lock(30000)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        if (due) { relist(log); last_list = now; }

        if (want_id) {
            gauge::DriveStats st;
            const bool ok = scan_one(log, want_id, &st);
            drive_log_unlock();
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            for (int i = 0; i < g_count; ++i) {
                if (g_cache[i].info.id != want_id) continue;
                if (ok) { g_cache[i].stats = st; g_cache[i].ready = true; }
                else    { g_cache[i].info.table_version = 0xFFFF; }   // says "cannot read"
                break;
            }
            xSemaphoreGive(g_mutex);
            if (ok) {
                // One line per drive folded. It is the only way to check the
                // four numbers on the glass against the same drive pulled to
                // CSV -- a panel cannot be read from a tool call.
                flight_log("drives: drive %u folded, %.2f km, %.0f rpm, %.0f km/h, %u recs",
                           (unsigned)want_id, st.distance_km, st.peak_rpm, st.peak_kph,
                           (unsigned)st.records);
            } else {
                flight_log("drives: drive %u could not be read", (unsigned)want_id);
            }
        } else {
            drive_log_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// --- what the view sees --------------------------------------------------

int src_count() {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const int n = g_count;
    xSemaphoreGive(g_mutex);
    return n;
}

bool src_row(int index, gauge_ui::DriveRowInfo* out) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const bool ok = index >= 0 && index < g_count;
    if (ok) {
        const Entry& e = g_cache[index];
        out->id       = e.info.id;
        out->epoch_s  = e.info.epoch_s;
        out->complete = e.info.complete;
        out->table_ok = e.info.table_version == gauge::kChanTableVersion;
        out->ready    = e.ready;
        out->stats    = e.stats;
        // A drive still being written has no end marker and its duration comes
        // from the records folded so far, not from the DriveInfo header.
        if (!e.ready) out->stats.duration_ms = e.info.duration_ms;
    }
    xSemaphoreGive(g_mutex);
    return ok;
}

const char* src_empty() { return g_empty; }

const gauge_ui::DrivesSource kSource = { src_count, src_row, src_empty };

}  // namespace

bool drives_list_set_date(uint32_t id, uint32_t epoch_s) {
    nvs_handle_t h;
    if (nvs_open(kDateNs, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[16];
    date_key(key, sizeof key, id);
    const bool ok = nvs_set_u32(h, key, epoch_s) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        // Take effect now rather than at the next five-second re-list.
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        for (int i = 0; i < g_count; ++i)
            if (g_cache[i].info.id == id) g_cache[i].info.epoch_s = epoch_s;
        xSemaphoreGive(g_mutex);
    }
    return ok;
}

void drives_list_dump(void) {
    if (!g_mutex) { printf("ERR drives view not started\n"); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const int n = g_count;
    for (int i = 0; i < n; ++i) {
        const Entry& e = g_cache[i];
        printf("ROW %d id=%u epoch=%u complete=%d table=%u ready=%d "
               "km=%.2f rpm=%.0f kph=%.0f ms=%u\n",
               i, (unsigned)e.info.id, (unsigned)e.info.epoch_s, e.info.complete ? 1 : 0,
               (unsigned)e.info.table_version, e.ready ? 1 : 0,
               e.stats.distance_km, e.stats.peak_rpm, e.stats.peak_kph,
               (unsigned)(e.ready ? e.stats.duration_ms : e.info.duration_ms));
    }
    xSemaphoreGive(g_mutex);
    printf("OK %d rows\n", n);
}

void drives_list_init(void) {
    g_mutex = xSemaphoreCreateMutex();
    gauge_ui::drives_set_source(&kSource);
    // Priority 1: below the recorder (3) and the console (2). Nothing waits on
    // it, and every millisecond it spends is a millisecond of flash reads.
    // Core 0, with the rest of the flash work -- the UI loop owns core 1.
    // 8192, not 4096: read_drive() holds a whole 4,080-byte flash sector in this
    // task's frame while it streams a drive, the same reason serial_cmd's task
    // is 12 KB. At 4 KB this overflowed and rebooted the gauge on the first
    // scan -- before any drive was ever listed.
    xTaskCreatePinnedToCore(scan_task, "drivescan", 8192, nullptr, 1, nullptr, 0);
}
