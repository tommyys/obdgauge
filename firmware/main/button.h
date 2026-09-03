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
// ---- what the hold does, and what it no longer does ----------------------
// The hold was going to restart into the replay, and it does not: demo mode is
// the four bench sweeps now -- revs, coolant, kW and the trip ring's economy
// walking red to green -- and none of them needs a reboot. So the hold takes
// effect the instant the button comes up, with no dark screen at all. Only the
// short press still has to restart, because only WiFi does.
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
    BOOT_NORMAL = 0,   // power-on, or RESET: the full splash, nothing swept
    BOOT_QUICK,        // short press: skip the splash, retry WiFi
    BOOT_DEMO,         // "RESTART DEMO": skip the splash, come up sweeping
} boot_request_t;

// Where the button is, once a frame. The two HELD states are what the gauge
// puts on the glass while the finger is still down -- that is the whole answer
// to "there is a lag before it responds": the press is acknowledged the moment
// it is seen, and the gesture it will perform is named before it happens.
typedef enum {
    BTN_IDLE = 0,      // up, nothing pending
    BTN_HELD_SHORT,    // down, still short of the hold: reload on release
    BTN_HELD_LONG,     // down, past three seconds: demo on release
    BTN_ACT_RELOAD,    // just released short
    BTN_ACT_DEMO,      // just released after a hold
} button_state_t;

// Configures GPIO0 as a pulled-up input. Safe to call before the display.
void button_init(void);

// Reads the button and says where it is. It performs nothing itself: the caller
// owns the screen and the sweeps, and a module that reaches into both would
// have to know about LVGL and the display lock to do it safely. Call once a
// frame from the app loop -- it keeps no task and allocates nothing, which on
// this board is the whole reason it is a poll and not a callback.
button_state_t button_poll(void);

// Why this boot happened. The flag lives in RTC memory, which survives
// esp_restart() but not a power cycle, so a gauge that loses power comes up
// normally rather than remembering a button press from before.
boot_request_t button_boot_request(void);

// Clears the request, so the next reset that is not a button press is a normal
// boot. Called once, immediately after the request has been read.
void button_boot_request_clear(void);

// Asks for a restart from a task that is not the app loop -- the serial
// console. It does NOT restart: the app loop takes it on its next frame, puts
// the note on the glass and restarts from there, so a typed RESTART behaves
// exactly like a pressed one and the two prove each other.
void button_queue_restart(bool demo);

// What was queued: 0 nothing, 1 a plain reload, 2 a reload into demo. Clears it.
int button_take_queued_restart(void);

// The same two restarts, for the serial console: "RESTART" and "RESTART DEMO".
// The button calls these, so the console exercises the identical path -- which
// is how the RTC flag, the skipped splash and the demo start were proven
// without a finger on the glass.
void button_request_restart(bool demo);

#ifdef __cplusplus
}
#endif
