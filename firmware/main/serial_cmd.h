// A line reader on the USB console, so tools/pull_drives.py can take the
// recorded drives off the board.
//
// This shares the port with `idf.py monitor` -- they cannot both be open. The
// puller says so when the port is busy rather than hanging.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts the reader task. Harmless if the recorder never mounted: the
// commands answer ERR instead.
void serial_cmd_init(void);

#ifdef __cplusplus
}
#endif
