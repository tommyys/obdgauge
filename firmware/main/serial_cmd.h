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

// Starts the reader task. Harmless if the recorder never mounted: the
// commands answer ERR instead.
void serial_cmd_init(void);

// Smallest bytes ever left unused on the console task's stack. 0xFFFFFFFF
// before it has run. Its true peak only happens during a GET, so a figure
// taken from an idle gauge is not the one to trim against.
uint32_t serial_cmd_stack_headroom(void);

#ifdef __cplusplus
}
#endif
