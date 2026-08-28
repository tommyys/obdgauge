#include "serial_cmd.h"
#include "sweep.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "bsp/esp-bsp.h"
#include "crc32.h"
#include "drive_log.h"
#include "gauge_ui.h"
#include "logbuf.h"

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
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
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

void task(void*) {
    install_blocking_console_reads();
    char line[64];
    for (;;) {
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
        } else if (!strncmp(line, "SWEEP", 5)) {
            // "SWEEP" for a minute, "SWEEP 20" for twenty seconds.
            const double secs = (line[5] == ' ') ? strtod(line + 6, nullptr) : 60.0;
            sweep_start(secs > 0 ? secs : 60.0, 1000.0, 8000.0);
            printf("OK sweep 1000-8000 rpm for %.0fs\n", secs > 0 ? secs : 60.0);
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

extern "C" void serial_cmd_init(void) {
    // Priority 2: below the recorder, well below the UI. Nothing waits on it.
    // 12288 bytes, not 8192: cmd_get's own frame holds read_drive's 4,080-byte
    // sector buffer, and emit()'s sink calls printf, which is 1-1.5 KB of
    // stack in ESP-IDF on its own. (The DriveInfo table that grew from 32 to
    // 64 entries with the truncation fix is static, so it is not on this
    // stack -- but 8 KB was thin before that and has never once run.)
    xTaskCreatePinnedToCore(task, "serialcmd", 12288, nullptr, 2, nullptr, 0);
}
