#include "drive_log.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "flight_log.h"
#include "imu.h"
#include "logbuf.h"
#include "poll.h"

namespace {

const char* TAG = "drivelog";

// A drive ends when the car goes quiet for this long. At key-off the vLinker
// sleeps and the poll loop notices within ~15 s -- measured in the car
// 2026-08-27 -- so 20 s closes the drive without splitting one at a red light.
constexpr int64_t kSilenceUs = 20LL * 1000 * 1000;
// Flush at least this often: a power cut can only cost what is unflushed.
constexpr int64_t kFlushUs = 2LL * 1000 * 1000;
// 5 Hz, not 10: the IMU is four channels against the car's twelve readings a
// second, and 10 Hz would halve the history the partition holds (design s3).
constexpr int64_t kImuPeriodUs = 200LL * 1000;
// Persist the borrowed clock this often, so a boot with no Mac has a floor.
constexpr int64_t kClockSaveUs = 300LL * 1000 * 1000;

struct QSample {
    uint16_t chan;
    float    value;
    double   t_s;
};

// The real part, behind gauge_core's seam. This is the only file that knows
// the ring is in flash at all.
class PartitionFlash : public gauge::IFlash {
public:
    bool open() {
        part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         ESP_PARTITION_SUBTYPE_ANY, "logs");
        return part_ != nullptr;
    }
    size_t sector_count() const override { return part_ ? part_->size / gauge::kSectorSize : 0; }
    bool read(size_t off, void* dst, size_t len) override {
        return part_ && esp_partition_read(part_, off, dst, len) == ESP_OK;
    }
    bool write(size_t off, const void* src, size_t len) override {
        return part_ && esp_partition_write(part_, off, src, len) == ESP_OK;
    }
    bool erase_sector(size_t index) override {
        return part_ && esp_partition_erase_range(part_, index * gauge::kSectorSize,
                                                  gauge::kSectorSize) == ESP_OK;
    }
private:
    const esp_partition_t* part_ = nullptr;
};

PartitionFlash   g_flash;
gauge::LogBuf*   g_log = nullptr;
QueueHandle_t    g_q = nullptr;
SemaphoreHandle_t g_lock = nullptr;
uint32_t         g_dropped = 0;
uint32_t         g_write_fail = 0;         // LogBuf calls that returned false
uint32_t         g_epoch_at_boot = 0;      // wall clock of uptime 0, 0 if unknown
uint32_t         g_clock_floor = 0;        // NVS wall clock from a previous run
bool             g_have_imu = false;

// Every LogBuf entry point returns bool and every one of them used to be
// discarded, so a failed flash write or erase left a hole in a drive nobody
// was watching and no way to find out afterwards. This is the only place any
// of them is called from; funnel them all through here.
bool note(bool ok, const char* what) {
    if (ok) return true;
    if (!g_write_fail) flight_log("drive log write FAILED (%s)", what);
    ++g_write_fail;
    return false;
}

uint32_t wall_now() {
    if (!g_epoch_at_boot) return 0;
    return g_epoch_at_boot + (uint32_t)(esp_timer_get_time() / 1000000);
}

void save_clock() {
    const uint32_t now = wall_now();
    if (!now) return;
    nvs_handle_t h;
    if (nvs_open("drivelog", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "clock", now);
    nvs_commit(h);
    nvs_close(h);
}

// Smallest stack headroom this task has ever had, in bytes, so its stack is
// sized from a measurement. It matters because this stack cannot move to
// PSRAM: the task writes flash, and a flash write disables the cache that
// PSRAM is reached through. Internal RAM is also what the BT controller needs
// a contiguous 30 KB of, so every KB here is a KB the radio may not get.
volatile uint32_t g_stack_low = 0xFFFFFFFFu;

void task(void*) {
    // Refreshed ONLY by a reading that came from the car. The IMU must never
    // touch this: it used to be fed back through the same queue, which meant
    // the recorder refreshed its own liveness timer 20 times a second and the
    // 20 s silence test below could never fire -- no drive ever closed, every
    // drive merged into drive 1, and a parked gauge on constant USB power
    // overwrote the whole ring overnight.
    int64_t last_sample = 0;              // 0 = no drive open
    int64_t last_flush = esp_timer_get_time();
    int64_t last_imu = 0;
    int64_t last_clock_save = esp_timer_get_time();
    double  drive_t0 = 0.0;
    uint32_t drive_records = 0;    // this drive only -- record_count() is cumulative since mount

    for (;;) {
        const uint32_t head = uxTaskGetStackHighWaterMark(nullptr) *
                              sizeof(StackType_t);
        if (head < g_stack_low) g_stack_low = head;

        const int64_t now = esp_timer_get_time();

        // Close a drive the car has stopped feeding.
        if (last_sample && now - last_sample > kSilenceUs) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            // end_drive() zeroes current_drive() before returning, so the id
            // to log has to be captured first, not read off the object after.
            const uint32_t closed_id = g_log->current_drive();
            note(g_log->end_drive(), "end_drive");
            xSemaphoreGive(g_lock);
            flight_log("drive %u closed, %u records", (unsigned)closed_id,
                       (unsigned)drive_records);
            last_sample = 0;
            drive_records = 0;
        }

        QSample s{};
        while (xQueueReceive(g_q, &s, 0) == pdTRUE) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            // `!current_drive()` is the ERASE CONFIRM case: erase_all()
            // remounts, which drops the open drive, and without this every
            // append would fail until the silence timer eventually closed a
            // drive that no longer exists. Re-open on the very next reading
            // instead of limping for 20 s.
            if (!last_sample || !g_log->current_drive()) {
                drive_t0 = s.t_s;
                drive_records = 0;
                note(g_log->begin_drive(wall_now()), "begin_drive");
                flight_log("drive %u opened, clock %s",
                           (unsigned)g_log->current_drive(),
                           wall_now() ? "known" : "UNKNOWN");
            }
            gauge::Record r{};
            const double dt = s.t_s - drive_t0;
            r.t_ms = dt > 0 ? (uint32_t)(dt * 1000.0) : 0;
            r.chan = s.chan;
            r.value = s.value;
            note(g_log->append(r), "append");
            ++drive_records;
            xSemaphoreGive(g_lock);
            // The car spoke. This -- and only this -- keeps the drive alive.
            last_sample = now;
        }

        // The IMU moves here from the UI loop, where it was read once a
        // second only to be printed. It is appended straight to the ring
        // rather than pushed back through g_q: this task is the only thing
        // allowed to touch LogBuf, it is already holding the lock in the same
        // breath, and the round trip through the queue was what fed the
        // liveness timer and stopped drives from ever ending.
        if (last_sample && g_have_imu && now - last_imu > kImuPeriodUs) {
            last_imu = now;
            imu_sample_t im{};
            if (imu_read(&im)) {
                // Ids are looked up once: log_chan_id walks a string table.
                static const uint16_t kImuChan[4] = {
                    gauge::log_chan_id("imu_ax"), gauge::log_chan_id("imu_ay"),
                    gauge::log_chan_id("imu_az"), gauge::log_chan_id("imu_gz"),
                };
                const float v[4] = {im.ax, im.ay, im.az, im.gz};
                const double dt = now / 1e6 - drive_t0;
                const uint32_t t_ms = dt > 0 ? (uint32_t)(dt * 1000.0) : 0;
                xSemaphoreTake(g_lock, portMAX_DELAY);
                // A drive may have closed between the check and the lock;
                // append() would refuse, and that refusal is not a fault.
                if (g_log->current_drive()) {
                    for (int i = 0; i < 4; ++i) {
                        if (kImuChan[i] == gauge::kChanUnknown) continue;
                        gauge::Record r{t_ms, kImuChan[i], 0, v[i]};
                        note(g_log->append(r), "append imu");
                        ++drive_records;
                    }
                }
                xSemaphoreGive(g_lock);
            }
        }

        if (now - last_flush > kFlushUs) {
            last_flush = now;
            xSemaphoreTake(g_lock, portMAX_DELAY);
            note(g_log->flush(), "flush");
            xSemaphoreGive(g_lock);
        }
        if (now - last_clock_save > kClockSaveUs) {
            last_clock_save = now;
            save_clock();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

extern "C" void drive_log_init(void) {
    if (!g_flash.open()) {
        ESP_LOGW(TAG, "no 'logs' partition -- drives will not be recorded");
        flight_log("no logs partition, NOT recording");
        return;
    }
    static gauge::LogBuf log(g_flash);
    if (!log.mount()) {
        ESP_LOGE(TAG, "logs partition will not mount");
        flight_log("logs partition will not mount, NOT recording");
        return;
    }
    g_log = &log;
    g_lock = xSemaphoreCreateMutex();
    g_q = xQueueCreate(128, sizeof(QSample));
    g_have_imu = imu_address() != 0;

    // The floor for a boot with no Mac: the drive happened after this.
    nvs_handle_t h;
    uint32_t floor_s = 0;
    if (nvs_open("drivelog", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "clock", &floor_s);
        nvs_close(h);
    }
    g_clock_floor = floor_s;
    ESP_LOGI(TAG, "%u sectors (%u used), %u drive starts, %u records, clock floor %u",
             (unsigned)g_flash.sector_count(), (unsigned)log.sectors_used(),
             (unsigned)log.drive_starts(), (unsigned)log.record_count(),
             (unsigned)floor_s);
    flight_log("drive log up: %u drive starts", (unsigned)log.drive_starts());

    // Priority 3 -- below the UI and below live_link's 4. Core 0, beside the
    // radio, so LVGL's render on core 1 never waits behind a flash erase.
    //
    // 3072, down from 8192. Measured on the board 2026-08-28: this task's
    // deepest ever use is 936 bytes (8192 with 7256 spare, reported as
    // "stack spare drivelog" on the ui: line). It has no recursion and no
    // large locals -- the buffer it appends through is LogBuf's 384-byte
    // batch_, not a stack frame. The 5 KB this returns is internal RAM, which
    // is the only memory the BT controller can use and the only memory the
    // panel's DMA can use. Watch that ui: figure if this task grows a local.
    xTaskCreatePinnedToCore(task, "drivelog", 3072, nullptr, 3, nullptr, 0);
}

extern "C" uint32_t drive_log_stack_headroom(void) { return g_stack_low; }

extern "C" void drive_log_sample(const char* key, float value, double t_s) {
    if (!g_q) return;
    const uint16_t chan = gauge::log_chan_id(key);
    if (chan == gauge::kChanUnknown) return;
    QSample s{chan, value, t_s};
    // Dropped rather than blocked on, exactly as live_link drops readings the
    // UI cannot take: the draw loop must never wait on flash.
    if (xQueueSend(g_q, &s, 0) != pdTRUE) ++g_dropped;
}

extern "C" void drive_log_set_epoch(uint32_t epoch_s) {
    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    g_epoch_at_boot = epoch_s > up ? epoch_s - up : 0;
    save_clock();
}

extern "C" bool drive_log_stats(drive_log_stats_t* out) {
    if (!out || !g_log) return false;
    // drive_log.h says the LogBuf may only be touched under this lock, and
    // this function used to read it without. The writer task can be part-way
    // through a flush that moves record_count_ and sectors_used_.
    if (!drive_log_lock(5000)) return false;
    *out = drive_log_stats_t{};
    out->drive_starts = (uint32_t)g_log->drive_starts();
    out->records = (uint32_t)g_log->record_count();
    out->sectors = (uint32_t)g_flash.sector_count();
    out->sectors_used = (uint32_t)g_log->sectors_used();
    out->bytes_used = (uint32_t)(g_log->sectors_used() * gauge::kSectorSize);
    out->dropped = g_dropped;
    out->write_fail = g_write_fail;
    out->epoch_s = wall_now();
    out->clock_floor_s = g_clock_floor;
    out->table_version = gauge::kChanTableVersion;
    drive_log_unlock();
    return true;
}

gauge::LogBuf* drive_log_buf(void) { return g_log; }
bool drive_log_lock(int ms) {
    return g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void drive_log_unlock(void) { if (g_lock) xSemaphoreGive(g_lock); }
