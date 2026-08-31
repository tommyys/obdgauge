// Records every reading of every drive to the `logs` partition.
//
// The gauge's real life is the car's own USB socket with no laptop attached,
// and until now that life left no trace: readings arrived, got drawn, and were
// gone. This keeps them, oldest drive dropped when the 10 MB fills, and
// tools/pull_drives.py hands them back as logs/*.csv.
//
// Nothing here may run on the UI loop. Erasing a sector takes tens of
// milliseconds, which is visibly dropped frames, and the draw path is already
// the fragile one (SPEC.md s6). So the UI loop's whole contribution is one
// non-blocking queue send, and a task on core 0 does the flash.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "imu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Sector headers flagged as opening a drive: an UPPER BOUND, not an
    // answer. It is never decremented when the ring drops a drive and it
    // counts drives too short to be offered, so it can exceed what LIST
    // shows. LIST is the only authority on what is held and pullable. (It
    // was called `drives` and documented as "drives held and offerable",
    // which it has never been.)
    uint32_t drive_starts;
    uint32_t records;       // records committed since mount
    uint32_t sectors;       // sectors in the partition
    uint32_t sectors_used;  // of those, the ones carrying data
    uint32_t bytes_used;    // sectors_used x 4096
    uint32_t dropped;       // samples the queue could not take
    uint32_t write_fail;    // LogBuf writes/erases that failed -- silent data loss
    uint32_t epoch_s;       // borrowed wall clock, 0 if never set
    // The clock last persisted to NVS by a previous run. A drive recorded
    // with no Mac attached happened AFTER this, and that floor is the only
    // thing that places it in time at all (design s5).
    uint32_t clock_floor_s;
    uint16_t table_version;
} drive_log_stats_t;

// Mounts the ring and starts the writer task. Safe to call with no partition:
// it says so and records nothing.
void drive_log_init(void);

// Hands one live reading to the writer. Called from the UI loop -- never
// blocks, never allocates, drops when the queue is full.
void drive_log_sample(const char* key, float value, double t_s);

// Smallest number of bytes ever left unused on the recorder task's stack.
// 0xFFFFFFFF until the task has run once. Printed by the ui: line so the
// stack can be trimmed from a measurement -- internal RAM here competes
// directly with the BT controller's contiguous 30 KB.
uint32_t drive_log_stack_headroom(void);

// The Mac's clock, from `TIME <epoch>`. Applies to drives opened afterwards.
void drive_log_set_epoch(uint32_t epoch_s);

// The wall clock now, or 0 if nobody has set one this run. Note that a reboot
// loses it: the epoch is not restored from NVS, only the floor below is, so
// after a power cycle the gauge honestly does not know the time until someone
// tells it -- on the Clock view or over `TIME`.
uint32_t drive_log_now(void);
// The last wall clock a previous run persisted, effectively the end of the
// last drive. What the Clock view's wheels start on.
uint32_t drive_log_clock_floor(void);

// Takes drive_log_lock() itself; do not call it while already holding it.
// False if there is no recorder, or if the lock could not be had in 5 s.
bool drive_log_stats(drive_log_stats_t* out);

#ifdef __cplusplus
}

// For serial_cmd.cpp's LIST/GET. Null until drive_log_init has mounted.
// Only the serial task may touch it, and only while holding drive_log_lock().
namespace gauge { class LogBuf; }
// The latest accelerometer reading, and the board clock it was taken at.
// False when the part has not reported yet, or when the recorder task was
// caught mid-write -- one skipped g sample, which the score does not notice.
//
// The recorder task is the only thing allowed to touch the I2C bus, so the UI
// loop cannot read the part itself; it reads what that task last published.
// The part is sampled at 20 Hz for this and written to flash at 5, because
// the driving score measures jerk and the log wants hours.
bool drive_log_imu(imu_sample_t* out, double* t_s);

gauge::LogBuf* drive_log_buf(void);
bool drive_log_lock(int ms);
void drive_log_unlock(void);
#endif
