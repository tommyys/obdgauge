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
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "drive_log.h"
#include "drive_stats.h"
#include "clock.h"
#include "drives.h"
#include "flight_log.h"
#include "logbuf.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

namespace {

// The newest drives, which is what a list is for. The ring can hold more; the
// view shows the newest and the console still reaches every one of them.
constexpr int kMaxCached = 12;
// How often the list itself is refreshed WHILE THE DRIVES VIEW IS ON SCREEN.
// Drives open and close on the scale of minutes, so re-listing faster than this
// is flash traffic for nothing.
constexpr int kRelistMs = 5000;
// And how long after the view was last drawn the board still counts as being
// watched. One frame is 16 ms; this is generous enough to survive a slow frame
// and short enough that leaving the view stops the flash traffic at once.
constexpr int kWatchGraceMs = 500;
// A gap longer than this means the view was somewhere else and has just come
// back, so the list is re-read immediately rather than up to kRelistMs later.
constexpr int kWatchReturnMs = 1500;

// ---- and why it also waits for the ring to move -------------------------
// A re-list is not a cheap read. LogBuf::list() summarises every drive it
// holds, and summarising means reading every record of it: measured on this
// board at 1.44 to 1.64 SECONDS for six drives, of which the NVS lookups for
// lent dates and hidden flags were 2 ms. There is no version of that which is
// quick enough to do on a timer.
//
// But it is also not necessary on a timer. The ring only changes when
// something is written to it, and LogBuf already counts that: record_count()
// rises on every flush, drive_starts() on every drive opened, and
// current_drive() names the one being written. If none of the three has moved
// since the last re-list, the answer cannot have changed and the read is
// skipped -- so browsing the list while parked costs nothing at all, and
// arriving at the view is instant instead of a second and a half of stall.
//
// While a drive IS recording the count keeps rising, so the re-list keeps
// happening on its kRelistMs cadence. That is on purpose: the open drive's own
// numbers grow, and they are the ones somebody watching the list wants right.
struct RingGen {
    size_t   records = 0;
    size_t   starts  = 0;
    uint32_t open    = 0;
    bool operator!=(const RingGen& o) const {
        return records != o.records || starts != o.starts || open != o.open;
    }
};
RingGen ring_gen(gauge::LogBuf* log) {
    return RingGen{log->record_count(), log->drive_starts(), log->current_drive()};
}

// ---- why the list waits to be looked at ---------------------------------
// relist() reads the drive ring off flash, and on an ESP32 a flash read turns
// the cache off for its duration. LVGL's code and its buffers live behind that
// cache, so every re-list stalls the drawing of whatever view happens to be up.
//
// Measured on the board: with this running every five seconds regardless of
// view, the tacho held 66 fps most of the time and fell to about 50 twice in
// every ten seconds -- roughly thirty dropped frames per dip. With the re-list
// gated on the Drives view being the one on screen, thirty consecutive samples
// came back at 66 fps with no dip at all.
//
// So the rule is: the only view that reads flash is the view that shows what is
// on it. Nothing else needs the list -- the console's own dump prints the cache
// this task already filled, and a drive that starts while the tacho is up will
// be found the moment somebody looks at the list.

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
// Drives the owner has taken off the list.
//
// A drive cannot be erased from the ring on its own: the sectors are a ring,
// and erasing one in the middle takes whatever else is living in it. So a
// hidden drive is still on flash and still pullable over the console -- it is
// simply not offered on the glass. The two 0.9 and 1.2 minute idles of
// 2026-08-29 are what this is for: the gauge finding the adapter before
// setting off is not a journey, and two of those per drive would bury the
// list within a week.
constexpr const char* kHideNs = "drivehide";

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

bool is_hidden(uint32_t id) {
    nvs_handle_t h;
    if (nvs_open(kHideNs, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    date_key(key, sizeof key, id);
    uint8_t hidden = 0;
    if (nvs_get_u8(h, key, &hidden) != ESP_OK) hidden = 0;
    nvs_close(h);
    return hidden != 0;
}

// Written by the LVGL task (src_watching) and read by scan_task. A plain tick
// count either way: a torn read costs one late re-list, and both tasks would
// have to be interleaved inside a 32-bit store for even that.
volatile TickType_t g_watched_at   = 0;
volatile bool       g_watch_return = false;

// The ring as it stood when the cache was last built. Touched only by
// scan_task, except for the force below.
RingGen g_listed{};
bool    g_listed_valid = false;
// Set when something OTHER than a write makes the cache wrong: a lent date or
// a hidden flag, neither of which the ring's own counters can see.
volatile bool g_force_relist = false;

// Set by drives_list_init(). The task is created early to claim its 8 KB of
// internal RAM while the heap is whole, but must not read flash before the
// recorder is mounted and the view exists to be fed.
volatile bool g_enabled = false;

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
    int hidden = 0;
    for (size_t i = 0; i < n && count < kMaxCached; ++i) {
        if (is_hidden(found[i].id)) { ++hidden; continue; }
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
    static bool said_once = false;
    if (changed || !said_once) {
        said_once = true;
        flight_log("drives: list has %d of %u held, %d hidden", count, (unsigned)n, hidden);
    }
}

void scan_task(void*) {
    while (!g_enabled) vTaskDelay(pdMS_TO_TICKS(50));
    TickType_t last_list = 0;
    for (;;) {
        gauge::LogBuf* log = drive_log_buf();
        if (!log) {
            g_empty = "recorder not available";
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        // Once at startup, so the count and the folding pass have something to
        // work from; then only while somebody is looking at the list.
        const TickType_t seen = g_watched_at;
        const bool watched =
            seen != 0 && (now - seen) <= pdMS_TO_TICKS(kWatchGraceMs);
        bool due = last_list == 0;
        if (watched && g_watch_return) { due = true; g_watch_return = false; }
        if (watched && (now - last_list) >= pdMS_TO_TICKS(kRelistMs)) due = true;
        // The gate. Everything above only says when it would be ALLOWED to
        // read; this says whether there is anything new to read. The forced
        // flag wins, because a lent date is a change the ring cannot report.
        if (due && g_listed_valid && !g_force_relist && !(ring_gen(log) != g_listed))
            due = false;

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

        if (due) {
            relist(log);
            g_listed = ring_gen(log);
            g_listed_valid = true;
            g_force_relist = false;
            last_list = now;
        }

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
                // The stack headroom rides along: 4 KB is not generous, and a
                // fold is this task's deepest moment.
                flight_log("drives: drive %u folded, %.2f km, %.0f rpm, %.0f km/h, "
                           "%u recs, stack spare %u",
                           (unsigned)want_id, st.distance_km, st.peak_rpm, st.peak_kph,
                           (unsigned)st.records,
                           (unsigned)(uxTaskGetStackHighWaterMark(nullptr) *
                                      sizeof(StackType_t)));
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

// Called on every frame the Drives view is drawn, and never otherwise. Notes
// the time, and notices a return from another view so the list is re-read on
// arrival rather than up to kRelistMs afterwards.
void src_watching() {
    const TickType_t now = xTaskGetTickCount();
    const TickType_t prev = g_watched_at;
    if (prev == 0 || (now - prev) > pdMS_TO_TICKS(kWatchReturnMs))
        g_watch_return = true;
    g_watched_at = now;
}

const gauge_ui::DrivesSource kSource = { src_count, src_row, src_empty,
                                         src_watching };

}  // namespace

bool drives_list_set_date(uint32_t id, uint32_t epoch_s) {
    nvs_handle_t h;
    if (nvs_open(kDateNs, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[16];
    date_key(key, sizeof key, id);
    const bool ok = nvs_set_u32(h, key, epoch_s) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        // Take effect now rather than at the next re-list.
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        for (int i = 0; i < g_count; ++i)
            if (g_cache[i].info.id == id) g_cache[i].info.epoch_s = epoch_s;
        xSemaphoreGive(g_mutex);
        // And let the cache be re-derived from flash and NVS when it next may:
        // the ring's own counters cannot see a lent date, so without this the
        // gate in scan_task would skip the read for ever. See RingGen.
        g_force_relist = true;
    }
    return ok;
}

bool drives_list_hide(uint32_t id, bool hidden) {
    nvs_handle_t h;
    if (nvs_open(kHideNs, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[16];
    date_key(key, sizeof key, id);
    const bool ok = (hidden ? nvs_set_u8(h, key, 1) : nvs_erase_key(h, key)) == ESP_OK;
    nvs_commit(h);
    nvs_close(h);
    if (ok) {
        // A drive being UN-hidden cannot be put back by patching the cache --
        // it is not in it. Only a re-list can find it again, and the ring's
        // counters cannot see a hidden flag move, so say so explicitly.
        g_force_relist = true;
        // Off the list now, not at the next re-list.
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        int out = 0;
        for (int i = 0; i < g_count; ++i) {
            if (hidden && g_cache[i].info.id == id) continue;
            g_cache[out++] = g_cache[i];
        }
        g_count = out;
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
        // The same local time the view puts on the row, so a timezone fault
        // shows up here rather than only on the glass.
        char when[32] = "date unknown";
        if (e.info.epoch_s) {
            const time_t t = (time_t)e.info.epoch_s;
            struct tm tm_v;
            localtime_r(&t, &tm_v);
            strftime(when, sizeof when, "%d %b %H:%M", &tm_v);
        }
        printf("ROW %d id=%u epoch=%u when='%s' complete=%d table=%u ready=%d "
               "km=%.2f rpm=%.0f kph=%.0f ms=%u\n",
               i, (unsigned)e.info.id, (unsigned)e.info.epoch_s, when, e.info.complete ? 1 : 0,
               (unsigned)e.info.table_version, e.ready ? 1 : 0,
               e.stats.distance_km, e.stats.peak_rpm, e.stats.peak_kph,
               (unsigned)(e.ready ? e.stats.duration_ms : e.info.duration_ms));
    }
    xSemaphoreGive(g_mutex);
    printf("OK %d rows\n", n);
}

// The header clock's window onto the board's wall clock. It lives here rather
// than in its own file because the running clock comes out of the same
// drive_log_stats_t the drives list already reads.
//
// Read-only now. The Clock view that used to write through this seam was
// removed on 2026-09-03: the gauge sets its own clock over WiFi at boot
// (SPEC.md s16), and `TIME <epoch>` from the console still goes straight to
// drive_log_set_epoch().
const gauge_ui::ClockSource kClockSource = {
    // Lock-free: the header asks for the time every frame, and
    // drive_log_stats() waits on the flash writer's mutex.
    []() -> uint32_t { return drive_log_now(); },
};

void drives_list_init(void) {
    g_mutex = xSemaphoreCreateMutex();
    gauge_ui::drives_set_source(&kSource);
    gauge_ui::clock_set_source(&kClockSource);
    g_enabled = true;
    // Priority 1: below the recorder (3) and the console (2). Nothing waits on
    // it, and every millisecond it spends is a millisecond of flash reads.
    // Core 0, with the rest of the flash work -- the UI loop owns core 1.
    // 4096, down from 8192. The 8 KB was for read_drive()'s 4,080-byte sector
    // buffer living in this task's frame; that buffer is static inside
    // read_drive() now (see logbuf.cpp -- every caller holds the recorder's
    // lock). 4 KB is what fits: measured on the board, the largest free block
    // once the views are up is 4,608 bytes.
    //
    // Created HERE, after the display, and NOT with the early claims. Moving
    // it early on 2026-09-04 did fix this view -- and took 8 KB of 8-bit
    // internal RAM that LVGL's own 8 KB task needed, so the gauge panicked at
    // esp_lv_adapter_start() and rebooted. The two cannot both have it. The
    // result is checked below, and loudly, because a silent failure here reads
    // exactly like "no drives were ever recorded".
    // INTERNAL|8BIT, not INTERNAL. A task stack must be byte-addressable, and
    // plain MALLOC_CAP_INTERNAL also counts the 32-bit-only region -- it read
    // 84,431 free with a largest block of 34,816 on the boot where LVGL's own
    // 8 KB task creation failed for want of memory. The wrong pool.
    const size_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t big = heap_caps_get_largest_free_block(caps);
    const size_t free_now = heap_caps_get_free_size(caps);
    if (xTaskCreatePinnedToCore(scan_task, "drivescan", 4096, nullptr, 1, nullptr, 0)
            != pdPASS) {
        printf("drives: FAILED to start the scan task -- the Drives view will "
               "stay empty. Internal RAM: %u free, largest block %u, "
               "needed 4096\n",
               (unsigned)free_now, (unsigned)big);
        flight_log("drives: scan task FAILED, internal free %u largest %u",
                   (unsigned)free_now, (unsigned)big);
    } else {
        flight_log("drives: scan task reserved, internal free %u largest %u",
                   (unsigned)free_now, (unsigned)big);
    }
}


