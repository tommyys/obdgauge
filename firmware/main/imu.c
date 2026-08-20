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
#define QMI_CTRL7    0x08   // bit0 aEN, bit1 gEN
#define QMI_AX_L     0x35   // ax,ay,az,gx,gy,gz -- 12 bytes, little endian

#define QMI_WHOAMI_VALUE 0x05

// CTRL2 = 0x24 -> +/-8g (010 << 4) at ODR index 4. 8g full scale gives
// 4096 LSB/g, which is plenty of range for braking and cornering in a car.
#define ACCEL_LSB_PER_G 4096.0f
// CTRL3 = 0x64 -> +/-1024 dps (110 << 4) at ODR index 4 -> 32 LSB/dps.
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
    ESP_ERROR_CHECK(wr(QMI_CTRL1, 0x40));   // auto-increment reads
    ESP_ERROR_CHECK(wr(QMI_CTRL2, 0x24));   // accel +/-8g
    ESP_ERROR_CHECK(wr(QMI_CTRL3, 0x64));   // gyro +/-1024 dps
    ESP_ERROR_CHECK(wr(QMI_CTRL7, 0x03));   // enable accel + gyro
    vTaskDelay(pdMS_TO_TICKS(20));
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
