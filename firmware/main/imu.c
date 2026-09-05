#include "imu.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "imu";

// QMI8658 register map (datasheet section 8).
#define QMI_WHO_AM_I 0x00
#define QMI_REVISION 0x01
#define QMI_CTRL1    0x02   // bit6 ADDR_AI: auto-increment on burst reads
#define QMI_CTRL2    0x03   // accel: [6:4] full scale, [3:0] ODR
#define QMI_CTRL3    0x04   // gyro:  [6:4] full scale, [3:0] ODR
#define QMI_CTRL5    0x06   // [6:5] gLPF mode, [4] gLPF on, [2:1] aLPF mode, [0] aLPF on
#define QMI_CTRL7    0x08   // bit0 aEN, bit1 gEN
#define QMI_AX_L     0x35   // ax,ay,az,gx,gy,gz -- 12 bytes, little endian

#define QMI_WHOAMI_VALUE 0x05

// **The part must do its own smoothing, because we cannot read it fast
// enough to do ours.** The recorder task reads this chip 20 times a second.
// A four-cylinder idling at 770 rpm fires 25.7 times a second and rocks the
// whole car sideways with it, and the dash -- and this part with it -- really
// does swing +/-0.4 g at that rate. Sampling a 25.7 Hz shake 20 times a
// second does not average it away: it reappears as a slow 5.7 Hz sway that
// the car never made. That is the wagon-wheel effect, and on 2026-09-05 it
// was the G view trembling left and right with the car parked. Measured in
// that drive: 0.155 g of swing on the sideways axis against 0.040 on the
// others, repeating on a 1.2 s beat that random noise does not have.
//
// The fix is to cut the shake off BEFORE it is sampled, which only the part
// can do. Its filter's width is a percentage of its output rate, so both are
// set together:
//
//   ODR index 6 -> 117.5 Hz in 6DOF mode (Table 26; in 6DOF both axes run off
//                  the gyro's clock, so CTRL2 and CTRL3 take the same index)
//   LPF mode 01 -> 3.63% of that -> 4.3 Hz of bandwidth
//
// 4.3 Hz sits in the quiet gap between the two. Real driving is far slower:
// in the 2026-09-04 drives 54% of the braking and acceleration signal is
// below 0.25 Hz and effectively none of it is above 2.5 Hz. The engine is far
// faster at 25.7 Hz. And 4.3 Hz is under half of our 20 Hz read rate, which
// is the condition for the sampling itself to stop inventing anything.
//
// The cost is 1/(2*pi*4.3) = 37 ms of lag on the dot. A hard stop takes about
// a third of a second to build, so this is not visible.
#define QMI_CTRL2_VALUE 0x26   // aFS 010 = +/-8 g, aODR 0110 = 117.5 Hz
#define QMI_CTRL3_VALUE 0x66   // gFS 110 = +/-1024 dps, gODR 0110 = 117.5 Hz
#define QMI_CTRL5_VALUE 0x33   // gLPF 01 on (0x30), aLPF 01 on (0x03)
#define QMI_CTRL7_VALUE 0x03   // aEN | gEN

// 8 g full scale gives 4096 LSB/g, which is plenty of range for braking and
// cornering in a car.
#define ACCEL_LSB_PER_G 4096.0f
// 1024 dps full scale gives 32 LSB/dps.
#define GYRO_LSB_PER_DPS 32.0f

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static uint8_t s_whoami;

static esp_err_t rd(uint8_t reg, uint8_t* buf, size_t n) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 200);
}

static esp_err_t wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 200);
}

int imu_i2c_scan(uint8_t* out, int max_out) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return 0;
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78 && n < max_out; ++a) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) out[n++] = a;
    }
    return n;
}

static bool try_addr(uint8_t addr) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint8_t who = 0;
    if (rd(QMI_WHO_AM_I, &who, 1) != ESP_OK || who != QMI_WHOAMI_VALUE) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    s_addr = addr;
    s_whoami = who;
    return true;
}

bool imu_init(void) {
    // Both 7-bit addresses the part can take, depending on its SA0 strap.
    if (!try_addr(0x6B) && !try_addr(0x6A)) {
        ESP_LOGW(TAG, "QMI8658 not found at 0x6B or 0x6A");
        return false;
    }
    ESP_LOGI(TAG, "QMI8658 at 0x%02X, WHO_AM_I=0x%02X", s_addr, s_whoami);
    // Not ESP_ERROR_CHECK. That aborts, and an abort here is a boot loop in
    // the car over one I2C hiccup on a part the gauge can live without: the
    // G view goes dark, every other view keeps working. Say so and stand down.
    const struct { uint8_t reg; uint8_t val; const char* what; } setup[] = {
        {QMI_CTRL1, 0x40,            "CTRL1 auto-increment reads"},
        {QMI_CTRL2, QMI_CTRL2_VALUE, "CTRL2 accel range and rate"},
        {QMI_CTRL3, QMI_CTRL3_VALUE, "CTRL3 gyro range and rate"},
        {QMI_CTRL5, QMI_CTRL5_VALUE, "CTRL5 low-pass filters"},
        {QMI_CTRL7, QMI_CTRL7_VALUE, "CTRL7 enable accel and gyro"},
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; ++i) {
        const esp_err_t e = wr(setup[i].reg, setup[i].val);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s failed: %s -- no IMU this boot",
                     setup[i].what, esp_err_to_name(e));
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            s_addr = 0;
            return false;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    // Read the three back. A write that returned ESP_OK only says the part
    // acknowledged the address; this says it kept the value. The filter is
    // the whole point of the settings above and it is invisible from the
    // outside -- without this line a chip that quietly ignored CTRL5 would
    // look exactly like one that applied it, and the only symptom would be
    // the G view trembling again months later.
    uint8_t back[3] = {0, 0, 0};
    if (rd(QMI_CTRL2, &back[0], 1) == ESP_OK &&
        rd(QMI_CTRL3, &back[1], 1) == ESP_OK &&
        rd(QMI_CTRL5, &back[2], 1) == ESP_OK) {
        const bool ok = back[0] == QMI_CTRL2_VALUE &&
                        back[1] == QMI_CTRL3_VALUE &&
                        back[2] == QMI_CTRL5_VALUE;
        ESP_LOGI(TAG, "CTRL2=0x%02X CTRL3=0x%02X CTRL5=0x%02X (%s), "
                      "117.5 Hz sampled, 4.3 Hz bandwidth",
                 back[0], back[1], back[2], ok ? "as written" : "NOT AS WRITTEN");
        if (!ok) {
            ESP_LOGE(TAG, "wanted CTRL2=0x%02X CTRL3=0x%02X CTRL5=0x%02X -- "
                          "the anti-shake filter is NOT on",
                     QMI_CTRL2_VALUE, QMI_CTRL3_VALUE, QMI_CTRL5_VALUE);
        }
    } else {
        ESP_LOGW(TAG, "could not read the control registers back");
    }
    return true;
}

uint8_t imu_address(void) { return s_addr; }
uint8_t imu_whoami(void)  { return s_whoami; }

bool imu_read(imu_sample_t* out) {
    if (!s_dev || !out) return false;
    uint8_t b[12];
    if (rd(QMI_AX_L, b, sizeof b) != ESP_OK) return false;
    int16_t raw[6];
    for (int i = 0; i < 6; ++i) {
        raw[i] = (int16_t)((uint16_t)b[i * 2] | ((uint16_t)b[i * 2 + 1] << 8));
    }
    out->ax = raw[0] / ACCEL_LSB_PER_G;
    out->ay = raw[1] / ACCEL_LSB_PER_G;
    out->az = raw[2] / ACCEL_LSB_PER_G;
    out->gx = raw[3] / GYRO_LSB_PER_DPS;
    out->gy = raw[4] / GYRO_LSB_PER_DPS;
    out->gz = raw[5] / GYRO_LSB_PER_DPS;
    return true;
}
