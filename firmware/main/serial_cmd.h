// A line reader on the USB console, so tools/pull_drives.py can take the
// recorded drives off the board.
//
// This shares the port with `idf.py monitor` -- they cannot both be open. The
// puller says so when the port is busy rather than hanging.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the reader task. Call EARLY, with the other big internal-RAM
// claims (the BT controller's 30,720 bytes and the blit band's 29,824) and
// before the display: the task's stack is 12 KB of internal RAM, and once
// LVGL and the WiFi stack have taken their share there is no contiguous block
// that size left. Measured 2026-09-03 -- called after the display it failed
// with 18,663 bytes free and a 9,728-byte largest hole, and the gauge then ran
// perfectly while answering no command ever again.
//
// The task waits for serial_cmd_enable() before serving anything, so claiming
// the memory early does not mean answering commands before the recorder and
// the views exist.
void serial_cmd_init(void);

// Lets the reader start serving commands. Call once everything the commands
// touch is up.
void serial_cmd_enable(void);

// Smallest bytes ever left unused on the console task's stack. 0xFFFFFFFF
// before it has run. Its true peak only happens during a GET, so a figure
// taken from an idle gauge is not the one to trim against.
uint32_t serial_cmd_stack_headroom(void);

#ifdef __cplusplus
}
#endif
