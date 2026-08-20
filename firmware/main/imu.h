// Minimal QMI8658 driver over the BSP's shared I2C bus.
// The Waveshare BSP declares BSP_CAPS_IMU 0 -- it does not drive the IMU -- but
// the part is on the board and the driving score needs it (SPEC.md B3): in the
// simulator "harsh" is a laggy speed-delta proxy, and this is what replaces it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float ax, ay, az;   // g
    float gx, gy, gz;   // deg/s
} imu_sample_t;

// Scan the shared bus. Writes up to max_out 7-bit addresses, returns the count.
int  imu_i2c_scan(uint8_t* out, int max_out);

// Probe and configure the QMI8658. False if it is not there.
bool imu_init(void);
uint8_t imu_address(void);      // 0 until imu_init succeeds
uint8_t imu_whoami(void);
bool imu_read(imu_sample_t* out);

#ifdef __cplusplus
}
#endif
