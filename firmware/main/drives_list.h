// Wires the Drives view to the flash ring. Call once at boot, after
// drive_log_init() and after the UI exists.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void drives_list_init(void);

// Lends a date to a drive that recorded with no clock. Kept in NVS beside the
// ring, because the epoch on flash cannot be rewritten in place. False if NVS
// refused. An epoch of 0 clears it.
bool drives_list_set_date(uint32_t id, uint32_t epoch_s);

// Takes a drive off the list, or puts it back. The records stay on flash and
// stay pullable -- a ring cannot erase one drive out of its middle -- so this
// hides, it does not delete.
bool drives_list_hide(uint32_t id, bool hidden);

// Prints the view's own cache -- one line per row it would draw, then OK.
// The console's `DRIVES` command.
void drives_list_dump(void);

#ifdef __cplusplus
}
#endif
