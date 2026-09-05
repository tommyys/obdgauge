#include "serial_cmd.h"
#include "button.h"
#include "sweep.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"
#include "crc32.h"
#include "drive_log.h"
#include "drives.h"
#include "drives_list.h"
#include "gauge_ui.h"
#include "imu.h"
#include "logbuf.h"
#include "wifi_time.h"

// USB-Serial-JTAG blocking reads
// ------------------------------
// This console is CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (sdkconfig.defaults),
// not a UART. ESP-IDF v5.5.2's own VFS driver for it
// (components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c) says so
// in its header comment: "Read is non-blocking ... These functions are used
// by default." Its read() confirms it: with no driver installed, a read with
// nothing in the FIFO returns fetch_size == 0, sets errno = EWOULDBLOCK, and
// returns -1 immediately -- it never waits for a byte to arrive.
//
// That is worse than a CPU-hogging spin here: it is data loss. fgets() on a
// stream that hits a read() error mid-line (not EOF, an *error*) returns
// NULL for that call per C11 7.21.7.2, and does not clear the stream's error
// indicator -- every fgets() after a human's first keystroke-sized partial
// read fails immediately, forever, unless something calls clearerr(). A
// person typing "STATS\n" one keystroke at a time (which is exactly how a
// USB-CDC terminal delivers it -- one small transfer per keystroke, not one
// packet for the whole line) would hit this on the very first read that
// finds the FIFO momentarily empty.
//
// The fix, from the same header: install the USB-Serial-JTAG driver and
// switch the VFS to it. usb_serial_jtag_vfs_use_driver()'s doc comment says
// plainly "read and write are blocking and interrupt-driven" once the driver
// backs it. That is both the correct behaviour for fgets() and exactly what
// avoids parking this task in a hot loop: with a blocking read, the task
// sleeps in the driver's ISR-signalled queue wait until a byte lands, so it
// is off the run queue entirely between keystrokes -- not spinning at
// priority 2 stealing time from the recorder (priority 3) or the radio.
namespace {

void install_blocking_console_reads() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // Already installed by the console/USB stack in some configs; either
    // outcome leaves us free to switch the VFS to the (now blocking) driver.
    const esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    // Printed because a console that takes no input is otherwise completely
    // silent about it: the board still talks, so nothing looks wrong until
    // `STATS` or pull_drives.py sits there getting no reply. On 2026-09-03
    // that cost an hour of bisecting a WiFi change that had nothing to do
    // with it.
    printf("serial: console driver %s (%s)\n",
           e == ESP_OK ? "installed" : "NOT installed", esp_err_to_name(e));
    // Line buffered, so a full "TIME 1756300000\n" reaches fgets() as one
    // unit rather than dribbling through stdio's own buffering games.
    setvbuf(stdin, nullptr, _IOLBF, 128);
}

// 3 records per base64 line group: 36 bytes -> 48 chars, no padding, so a
// line never carries a partial record and the host can decode line by line.
constexpr size_t kRecordsPerLine = 3;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_line(const uint8_t* in, size_t len) {
    char out[80];
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = kB64[(v >> 6) & 63];
        out[o++] = kB64[v & 63];
    }
    out[o] = 0;
    printf("%s\n", out);
}

struct GetCtx {
    uint32_t crc;
    uint32_t sent;
};

bool emit(const gauge::Record* r, size_t n, void* ctx) {
    GetCtx* g = static_cast<GetCtx*>(ctx);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(r);
    const size_t total = n * sizeof(gauge::Record);
    // gauge::crc32, not esp_rom_crc32_le: the two are the same algorithm
    // (crc32.h shows the IDF source), but only this one is reachable from
    // firmware/test/host, so only this one can be pinned to the standard
    // check value that the Python side is also pinned to.
    g->crc = gauge::crc32(g->crc, bytes, total);
    for (size_t off = 0; off < total; off += kRecordsPerLine * sizeof(gauge::Record)) {
        const size_t chunk = total - off < kRecordsPerLine * sizeof(gauge::Record)
                                 ? total - off
                                 : kRecordsPerLine * sizeof(gauge::Record);
        b64_line(bytes + off, chunk);
    }
    g->sent += n;
    return true;
}

// What is actually on the board's I2C bus.
//
// The BSP header says the bus is shared by "touch, audio, IMU, RTC, and
// power-management devices", but that comment covers a family of boards and
// Waveshare's page for this one does not list a clock chip at all. A drive
// recorded with no clock is stamped `drive-unknown-N` for ever (SPEC.md s15),
// so whether a PCF85063 is sitting there unused is worth one command to
// settle rather than an argument. Addresses only -- this identifies parts, it
// does not talk to them.
void cmd_i2c() {
    // Three passes, and only an address that answered in all three counts.
    // One pass on this board reports 12-18 devices and a different set every
    // time: the bus is shared with the touch controller and the IMU, which
    // are being polled while the scan runs, and a probe that collides with
    // their traffic reads as an ACK from an address with nothing on it. The
    // real parts answer every pass; the ghosts never repeat.
    uint8_t seen[3][32];
    int count[3] = {0, 0, 0};
    for (int pass = 0; pass < 3; ++pass) {
        count[pass] = imu_i2c_scan(seen[pass], (int)(sizeof seen[pass]));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    int n = 0;
    for (int i = 0; i < count[0]; ++i) {
        const uint8_t a = seen[0][i];
        bool in_all = true;
        for (int pass = 1; pass < 3 && in_all; ++pass) {
            bool hit = false;
            for (int j = 0; j < count[pass]; ++j) hit = hit || seen[pass][j] == a;
            in_all = hit;
        }
        if (!in_all) continue;
        ++n;
        const char* part = "?";
        switch (a) {
            case 0x18: case 0x19: part = "ES8311 audio codec"; break;
            case 0x34:            part = "AXP2101 power management"; break;
            case 0x40: case 0x41: part = "ES7210 echo cancellation"; break;
            case 0x51:            part = "PCF85063 REAL-TIME CLOCK"; break;
            case 0x5a:            part = "CST9217 touch"; break;
            case 0x6a: case 0x6b: part = "QMI8658 IMU"; break;
        }
        printf("I2C 0x%02x %s\n", a, part);
    }
    printf("OK %d devices\n", n);
}

void cmd_stats() {
    drive_log_stats_t s{};
    if (!drive_log_stats(&s)) { printf("ERR no recorder\n"); return; }
    printf("STATS sectors=%u used=%u bytes=%u starts=%u records=%u dropped=%u "
           "writefail=%u epoch=%u floor=%u table=%u\n",
           (unsigned)s.sectors, (unsigned)s.sectors_used, (unsigned)s.bytes_used,
           (unsigned)s.drive_starts, (unsigned)s.records, (unsigned)s.dropped,
           (unsigned)s.write_fail, (unsigned)s.epoch_s, (unsigned)s.clock_floor_s,
           (unsigned)s.table_version);
    printf("OK\n");
}

// before_id 0 is the newest page; anything else is the page of drives OLDER
// than that drive. The window is still kListCapacity wide -- paging is what
// makes the drives past it reachable instead of merely admitted to.
void cmd_list(uint32_t before_id) {
    gauge::LogBuf* log = drive_log_buf();
    if (!log) { printf("ERR no recorder\n"); return; }
    if (!drive_log_lock(5000)) { printf("ERR busy\n"); return; }
    if (before_id && !log->has_drive(before_id)) {
        drive_log_unlock();
        printf("ERR no drive %u\n", (unsigned)before_id);
        return;
    }
    // Sized to what list() can actually hold. It used to be 32 against a
    // 64-entry table, so a commute's worth of drives past the cap were
    // invisible to LIST, refused by GET, and eventually dropped unpulled --
    // while "OK 32 drives" read like a complete answer.
    static gauge::DriveInfo info[gauge::kListCapacity];
    for (auto& d : info) d = gauge::DriveInfo{};
    bool truncated = false;
    const size_t n = log->list(info, gauge::kListCapacity, &truncated, before_id);
    drive_log_unlock();
    for (size_t i = 0; i < n; ++i)
        printf("DRIVE id=%u epoch=%u records=%u ms=%u complete=%d table=%u\n",
               (unsigned)info[i].id, (unsigned)info[i].epoch_s,
               (unsigned)info[i].records, (unsigned)info[i].duration_ms,
               info[i].complete ? 1 : 0, (unsigned)info[i].table_version);
    printf("OK %u drives truncated=%d\n", (unsigned)n, truncated ? 1 : 0);
}

void cmd_get(uint32_t id) {
    gauge::LogBuf* log = drive_log_buf();
    if (!log) { printf("ERR no recorder\n"); return; }
    if (!drive_log_lock(5000)) { printf("ERR busy\n"); return; }
    // Resolved by id alone, NOT by looking for it in list()'s reply. list()
    // can only ever report the newest kListCapacity drives, so a GET that
    // went through it refused every drive older than that window -- and the
    // pull tool skips drives already in logs/, so once the newest 64 were
    // pulled no further run could ever reach the older ones: they sat on
    // flash until the ring dropped them, unpulled. has_drive() scans for the
    // one drive and applies the same offerable rule, with no window at all.
    gauge::DriveInfo info{};
    if (!log->has_drive(id, &info)) {
        drive_log_unlock();
        printf("ERR no drive %u\n", (unsigned)id);
        return;
    }
    const uint32_t records = info.records;
    const uint16_t version = info.table_version;
    if (version != gauge::kChanTableVersion) {
        // Channel ids are positions in this firmware's table. Handing these
        // records to a reader that would label them with a different table's
        // names is mislabelled data, which is worse than no data (design s2).
        drive_log_unlock();
        printf("ERR drive %u was written by channel table v%u, this firmware is v%u\n",
               (unsigned)id, (unsigned)version, (unsigned)gauge::kChanTableVersion);
        return;
    }
    printf("BEGIN %u %u\n", (unsigned)id, (unsigned)records);
    GetCtx g{0, 0};
    const bool ok = log->read_drive(id, emit, &g);
    drive_log_unlock();
    printf("END crc32=%08x\n", (unsigned)g.crc);
    printf(ok && g.sent == records ? "OK\n" : "ERR short read\n");
}

// Smallest stack headroom seen, in bytes. Sampled after each command rather
// than before, because the peak this stack is sized for is inside cmd_get --
// read_drive's 4,080-byte sector buffer plus printf.
volatile uint32_t g_stack_low = 0xFFFFFFFFu;

// Set by serial_cmd_enable(). The task is created early to claim its stack
// while the internal heap is whole, but must not answer a command before the
// recorder and the views it reaches into exist.
volatile bool g_enabled = false;

void task(void*) {
    install_blocking_console_reads();
    while (!g_enabled) vTaskDelay(pdMS_TO_TICKS(50));
    char line[64];
    for (;;) {
        {
            const uint32_t head = uxTaskGetStackHighWaterMark(nullptr) *
                                  sizeof(StackType_t);
            if (head < g_stack_low) g_stack_low = head;
        }
        // Blocking now (see install_blocking_console_reads above): this task
        // sleeps here between commands rather than polling stdin. But
        // usb_serial_jtag_read() has its own unconditional early return --
        // if usb_serial_jtag_is_connected() is false (a sleeping Mac, a
        // jiggled cable, a USB re-enumeration) it returns -1 with errno left
        // untouched, even in driver/blocking mode. That single -1 is enough
        // to latch stdin's sticky error flag (__SERR): once set, newlib's
        // __srefill short-circuits on it and every fgets() after this one
        // fails immediately without calling read() again -- the same dead
        // console this task's blocking-read fix exists to prevent, just
        // reached by a transient USB drop instead of the VFS default. A
        // physical disconnect must not be allowed to latch a software error
        // the wire will recover from as soon as the host reappears.
        if (!fgets(line, sizeof line, stdin)) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!strncmp(line, "TIME ", 5)) {
            drive_log_set_epoch((uint32_t)strtoul(line + 5, nullptr, 10));
            printf("OK clock set\n");
        } else if (!strncmp(line, "DATE ", 5)) {
            // DATE <drive id> <epoch> -- for a drive recorded before anything
            // ever set the board's clock. It cannot be fixed on flash (the
            // marker's zeroed bits cannot be written back up), so it is kept
            // beside the ring instead.
            char* end = nullptr;
            const uint32_t id = (uint32_t)strtoul(line + 5, &end, 10);
            const uint32_t epoch = end ? (uint32_t)strtoul(end, nullptr, 10) : 0;
            if (!id) printf("ERR say 'DATE <id> <epoch>'\n");
            else if (drives_list_set_date(id, epoch)) printf("OK drive %u dated %u\n",
                                                            (unsigned)id, (unsigned)epoch);
            else printf("ERR could not store the date\n");
        } else if (!strncmp(line, "HIDE ", 5) || !strncmp(line, "SHOW ", 5)) {
            // HIDE <id> takes a drive off the DRIVES view; SHOW <id> puts it
            // back. Neither touches the records: LIST and GET still see it.
            const bool hide = line[0] == 'H';
            const uint32_t id = (uint32_t)strtoul(line + 5, nullptr, 10);
            if (!id) printf("ERR say '%s <id>'\n", hide ? "HIDE" : "SHOW");
            else if (drives_list_hide(id, hide))
                printf("OK drive %u %s\n", (unsigned)id, hide ? "hidden" : "shown");
            else printf("ERR could not store it\n");
        } else if (!strcmp(line, "DRIVES")) {
            // What the Drives view is showing, in text. The panel cannot be
            // read from a tool call, so without this the only way to check the
            // view is to be in front of the gauge.
            drives_list_dump();
        } else if (!strncmp(line, "WIFI KEEP", 9)) {
            // There was going to be a flag here. The board settled it
            // instead. Measured 2026-09-03: the station holds 30,252 bytes of
            // internal RAM while it is up, and the display then cannot get
            // its own 14,912-byte contiguous buffer -- bsp_display_start()
            // asserts and the gauge boot-loops. Leaving WiFi up does not cost
            // frames here; it stops the panel starting at all. The command
            // stays so that asking gets an answer rather than silence.
            printf("ERR wifi cannot stay up on this board -- it holds 30,252 "
                   "bytes of internal RAM while up, and the display cannot "
                   "start without it. See SPEC.md s16.\n");
        } else if (!strcmp(line, "WIFI")) {
            printf("WIFI %s\n", gauge_platform::wifi_time::status());
            printf("WIFI busy=%s epoch=%u\n",
                   gauge_platform::wifi_time::busy() ? "yes" : "no",
                   (unsigned)gauge_platform::wifi_time::epoch());
            printf("OK\n");
        } else if (!strcmp(line, "I2C")) {
            cmd_i2c();
        } else if (!strcmp(line, "STATS")) {
            cmd_stats();
        } else if (!strcmp(line, "LIST")) {
            cmd_list(0);
        } else if (!strncmp(line, "LIST BEFORE ", 12)) {
            cmd_list((uint32_t)strtoul(line + 12, nullptr, 10));
        } else if (!strncmp(line, "GET ", 4)) {
            cmd_get((uint32_t)strtoul(line + 4, nullptr, 10));
        } else if (!strcmp(line, "ERASE CONFIRM")) {
            gauge::LogBuf* log = drive_log_buf();
            if (!log) { printf("ERR no recorder\n"); }
            else if (!drive_log_lock(30000)) { printf("ERR busy\n"); }
            else { const bool ok = log->erase_all(); drive_log_unlock();
                   // The Drives view builds its list once and then leaves the
                   // flash alone, so an erase is invisible to it without this
                   // -- it would go on offering drives that no longer exist.
                   if (ok) drives_list_refresh();
                   printf(ok ? "OK erased\n" : "ERR erase failed\n"); }
        } else if (!strncmp(line, "EASE", 4)) {
            // The needles chase their readings rather than jumping to them.
            // How hard that is to see depends on the car's reporting rate, so
            // the setting is live: "EASE 0" restores the old jump, "EASE 120"
            // is the default, and the two can be compared on the glass in the
            // same minute without a reflash.
            if (line[4] == ' ')
                gauge_ui::set_ease_tau_ms((uint32_t)strtoul(line + 5, nullptr, 10));
            printf("OK ease %ums\n", (unsigned)gauge_ui::ease_tau_ms());
        } else if (!strncmp(line, "FLICK", 5)) {
            // A vertical flick on the Drives list, with no finger on the
            // glass -- the same queue a real gesture uses. "FLICK" moves down
            // the list, "FLICK -" moves back up. Prints where the list ended
            // up, so a step can be checked from a tool call.
            const int dir = (line[5] == ' ' && line[6] == '-') ? +1 : -1;
            const int before = gauge_ui::drives_scroll_y();
            gauge_ui::queue_drives_step(dir * gauge_ui::drives_step_rows());
            // The app loop acts on it; give it a few frames to land.
            vTaskDelay(pdMS_TO_TICKS(300));
            printf("OK flick %d, scroll y %d -> %d\n", dir, before,
                   gauge_ui::drives_scroll_y());
        } else if (!strncmp(line, "SWIPE", 5)) {
            // A real view change, slide and all, with no finger on the glass.
            // The slide is where this firmware's timing and memory problems
            // show up, and measuring it should not need a person in the room.
            const int dir = (line[5] == ' ' && line[6] == '-') ? -1 : 1;
            // Queued, so this runs where a real gesture's slide runs -- in the
            // app loop. Calling advance_view() straight from this task was
            // measuring a path no finger ever takes.
            gauge_ui::queue_view_step(dir);
            printf("OK swipe %d\n", dir);
        } else if (!strncmp(line, "CHROME", 6)) {
            // BENCH: "CHROME 0" hides both, 1 scrims only, 2 bezel only, 3 both.
            const int m = (line[6] == ' ') ? (int)strtol(line + 7, nullptr, 10) : 3;
            bsp_display_lock(-1);
            gauge_ui::chrome_show((m & 1) != 0, (m & 2) != 0);
            bsp_display_unlock();
            printf("OK chrome scrims=%d bezel=%d\n", (m & 1) != 0, (m & 2) != 0);
        } else if (!strncmp(line, "SCROLL", 6)) {
            // Drags the Drives list up and down for a few seconds, so the
            // frame rate during a scroll can be read off the ui log without a
            // thumb on the glass. "SCROLL" for 8 s, "SCROLL 20" for twenty.
            const double secs = (line[6] == ' ') ? strtod(line + 7, nullptr) : 8.0;
            const int64_t until = esp_timer_get_time() + (int64_t)(secs * 1e6);
            int dy = -8, moved = 0, steps = 0;
            bool ok = true;
            while (ok && esp_timer_get_time() < until) {
                bsp_display_lock(-1);
                ok = gauge_ui::drives_scroll_by(dy);
                bsp_display_unlock();
                moved += dy;
                ++steps;
                // Turn round before the list runs out of travel, so this keeps
                // dirtying the screen rather than pushing against the end.
                if (moved < -320 || moved > 0) { dy = -dy; }
                vTaskDelay(pdMS_TO_TICKS(16));
            }
            printf(ok ? "OK scroll %d steps\n" : "ERR not on the drives view\n", steps);
        } else if (!strncmp(line, "SWEEP", 5)) {
            // "SWEEP" for a minute, "SWEEP 20" for twenty seconds.
            const double secs = (line[5] == ' ') ? strtod(line + 6, nullptr) : 60.0;
            sweep_start(secs > 0 ? secs : 60.0, 1000.0, 8000.0);
            printf("OK sweep 1000-8000 rpm for %.0fs\n", secs > 0 ? secs : 60.0);
        } else if (!strncmp(line, "TEMP", 4)) {
            // "TEMP" for a minute, "TEMP 20" for twenty seconds. Walks coolant
            // from 30 to 120 C and back, which is the only way to see the
            // engine view's scale fill and change colour without a drive.
            //
            // Starts below the scale's 40 C floor on purpose: the bottom of
            // that dial is where a cold car sits, and "one segment lit, not
            // none" is the part of it worth looking at.
            const double secs = (line[4] == ' ') ? strtod(line + 5, nullptr) : 60.0;
            sweep_temp_start(secs > 0 ? secs : 60.0, 30.0, 120.0);
            printf("OK temp sweep 30-120 C for %.0fs\n", secs > 0 ? secs : 60.0);
        } else if (!strncmp(line, "KW", 2)) {
            // "KW" for a minute, "KW 20" for twenty seconds. Walks power from
            // 0 to 150 kW and back, which is the only way to see the power
            // dial's scale fill and brighten without a road.
            //
            // 150 rather than the car's own ceiling: the profile's figure is
            // what the dial is scaled to, so sweeping exactly to it never shows
            // the top segment lit for long. A little past it does.
            const double secs = (line[2] == ' ') ? strtod(line + 3, nullptr) : 60.0;
            sweep_kw_start(secs > 0 ? secs : 60.0, 0.0, 150.0);
            printf("OK kw sweep 0-150 kW for %.0fs\n", secs > 0 ? secs : 60.0);
        } else if (!strncmp(line, "RESTART", 7)) {
            // "RESTART" retries wifi, "RESTART DEMO" comes up replaying. The
            // same two calls the button makes, so this console command and the
            // button prove each other -- which is how the RTC flag, the
            // skipped splash and the demo start were tested without a finger
            // on the glass.
            const bool demo = strstr(line, "DEMO") != nullptr;
            printf("OK restarting%s\n", demo ? " into demo mode" : "");
            // Queued rather than done here, so it goes out through the app
            // loop exactly as a press does -- note on the glass first, then
            // the reset. Restarting from this task would skip the screen.
            button_queue_restart(demo);
        } else if (!strcmp(line, "DEMO")) {
            // Starts the built-in replay. Off at power-up on purpose: see
            // demo_request() in sweep.h.
            demo_request();
            printf("OK demo replay started\n");
        } else if (!strncmp(line, "ECON", 4)) {
            // "ECON" for a minute, "ECON 20" for twenty seconds. Walks the
            // trip ring from 4 to 16 km/L and back, which is the only way to
            // see its colour ramp without driving to both ends of it.
            const double secs = (line[4] == ' ') ? strtod(line + 5, nullptr) : 60.0;
            sweep_econ_start(secs > 0 ? secs : 60.0, 4.0, 16.0);
            printf("OK econ 4-16 km/L for %.0fs\n", secs > 0 ? secs : 60.0);
        } else if (!strncmp(line, "BAND", 4)) {
            if (line[4] == ' ') gauge_ui::set_band_enabled(line[5] != '0');
            printf("OK band\n");
        } else if (!strncmp(line, "DIALS", 5)) {
            // The 434 px rim is the one object big enough that moving it
            // repaints most of the panel. Turning it off says how much of a
            // frame it is actually worth, on the glass, rather than by
            // argument -- compare the fps in the ui: line either way.
            if (line[5] == ' ') gauge_ui::set_dial_enabled(line[6] != '0');
            printf("OK dials\n");
        } else if (!strcmp(line, "ERASE")) {
            printf("ERR say 'ERASE CONFIRM'\n");
        } else if (line[0]) {
            printf("ERR unknown command\n");
        }
        fflush(stdout);
    }
}

}  // namespace

extern "C" uint32_t serial_cmd_stack_headroom(void) { return g_stack_low; }

extern "C" void serial_cmd_enable(void) { g_enabled = true; }

extern "C" void serial_cmd_init(void) {
    // Priority 2: below the recorder, well below the UI. Nothing waits on it.
    // 8192, down from 12288: the 12 KB was sized for read_drive's 4,080-byte
    // sector buffer sitting in cmd_get's frame, and that buffer is static
    // inside read_drive() now (see logbuf.cpp -- every caller holds the
    // recorder's lock). What is left is emit()'s printf, 1-1.5 KB of stack in
    // ESP-IDF on its own. The 4 KB this returns is the byte-addressable
    // internal RAM the whole board is short of. Watch "stack spare serialcmd"
    // on the ui: line -- it has been reporting over 4,800 bytes spare.
    // The result is checked, and loudly. A silent failure here reads exactly
    // like a wedged board: the gauge runs, draws and logs perfectly, and
    // simply never answers a command again -- `STATS` and pull_drives.py hang
    // with no error anywhere. That is what linking the WiFi and lwip stacks
    // in did on 2026-09-03: their static footprint left less internal RAM
    // than this 12 KB stack needed, and nothing said so.
    const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT);
    if (xTaskCreatePinnedToCore(task, "serialcmd", 6144, nullptr, 2, nullptr, 0)
            != pdPASS) {
        printf("serial: FAILED to start the console task -- no commands will "
               "work. DRAM: %u free, largest block %u, needed 6144\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT),
               (unsigned)big);
    }
}
