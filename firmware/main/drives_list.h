// Wires the Drives view to the flash ring. Call once at boot, after
// drive_log_init() and after the UI exists.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void drives_list_init(void);

#ifdef __cplusplus
}
#endif
