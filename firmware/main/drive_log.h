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

// The Mac's clock, from `TIME <epoch>`. Applies to drives opened afterwards.
void drive_log_set_epoch(uint32_t epoch_s);

// Takes drive_log_lock() itself; do not call it while already holding it.
// False if there is no recorder, or if the lock could not be had in 5 s.
bool drive_log_stats(drive_log_stats_t* out);

#ifdef __cplusplus
}

// For serial_cmd.cpp's LIST/GET. Null until drive_log_init has mounted.
// Only the serial task may touch it, and only while holding drive_log_lock().
namespace gauge { class LogBuf; }
gauge::LogBuf* drive_log_buf(void);
bool drive_log_lock(int ms);
void drive_log_unlock(void);
#endif
