// The one button the firmware can read, and the two things it does.
//
// This board's BSP declares no buttons at all (BSP_CAPS_BUTTONS 0) and names no
// button pin, so what is here is the ESP32-S3's own BOOT button on GPIO0 --
// free on this board, since the panel, touch, I2C and audio are all elsewhere.
// The other button is RESET, which is wired to the chip's enable pin: it is a
// hardware line and no firmware can see it.
//
// ---- why both gestures restart the gauge ----------------------------------
// A short press is asked to "retry WiFi", and on this board that can only mean
// a reboot. The station needs one contiguous 30,252-byte block of internal RAM
// and there are about 2,000 bytes free once the display is up; worse, bringing
// it up while the panel is running is what boot-looped the gauge on 2026-09-03
// (the display could not get its own 14,912-byte buffer). WiFi runs before the
// display and is gone before it starts, and there is exactly one window it fits
// in -- see the note above wifi_time::start() in main.cpp. So the button does
// the only thing that can work: it restarts, and the boot order retries WiFi
// properly on the way up.
//
// ---- and why they act on RELEASE -----------------------------------------
// GPIO0 held low THROUGH a reset is what puts an ESP32-S3 into USB download
// mode. A restart fired at the three-second mark, with the finger still down,
// would come up in the bootloader with a black screen and look like a gauge
// that had died. Both gestures are therefore decided when the button comes
// back up, at which point the pin is high and the reset is safe.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the gauge should do differently on the boot it is in, decided by the
// press that caused it. Read once at startup, then cleared.
typedef enum {
    BOOT_NORMAL = 0,   // power-on, or RESET: the full splash, no replay
    BOOT_QUICK,        // short press: skip the splash, retry WiFi
    BOOT_DEMO,         // three-second hold: skip the splash, start the replay
} boot_request_t;

// Configures GPIO0 as a pulled-up input. Safe to call before the display.
void button_init(void);

// Reads the button and, on release, acts: a short press restarts the gauge, a
// hold of three seconds or more restarts it into demo mode. Call once a frame
// from the app loop -- it keeps no task and allocates nothing, which on this
// board is the whole reason it is a poll and not a callback.
void button_poll(void);

// Why this boot happened. The flag lives in RTC memory, which survives
// esp_restart() but not a power cycle, so a gauge that loses power comes up
// normally rather than remembering a button press from before.
boot_request_t button_boot_request(void);

// Clears the request, so the next reset that is not a button press is a normal
// boot. Called once, immediately after the request has been read.
void button_boot_request_clear(void);

// The same two restarts, for the serial console: "RESTART" and "RESTART DEMO".
// The button calls these, so the console exercises the identical path -- which
// is how the RTC flag, the skipped splash and the demo start were proven
// without a finger on the glass.
void button_request_restart(bool demo);

#ifdef __cplusplus
}
#endif
