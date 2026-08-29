// Wires the Drives view to the flash ring. Call once at boot, after
// drive_log_init() and after the UI exists.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void drives_list_init(void);

// Prints the view's own cache -- one line per row it would draw, then OK.
// The console's `DRIVES` command.
void drives_list_dump(void);

#ifdef __cplusplus
}
#endif
