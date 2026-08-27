// A flight recorder for runs with no laptop attached.
//
// The gauge's real life is on the car's own power with nothing plugged into
// it, and every diagnostic this firmware has is a printf down a USB cable that
// will not be there. This keeps the session's events in NVS, which survives
// the power being cut, and prints the PREVIOUS session back the next time the
// board boots with a console attached. So the sequence is: unplug, drive,
// plug back in, read what happened.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Prints the last session's log, then starts a fresh one. Call once, early.
void flight_log_init(void);

// Reprints the previous session. The boot-time print alone is unreliable: the
// board boots the instant it is plugged in, and a console opened a second
// later has already missed it -- which would lose exactly the record the
// unplugged run existed to produce. Call periodically for the first minute.
void flight_log_replay(void);

// Appends one timestamped line and commits it to flash immediately -- the
// whole point is surviving a power cut that gives no warning.
void flight_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
