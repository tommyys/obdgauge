#include "drive_source.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char* TAG = "drives";

// NOT esp_partition_mmap: the ESP32-S3 MMU can only map the first 16 MB of
// flash, and the drives partition sits at 0x1410000 (21 MB). mmap there
// reports success and then reads nothing, which cost an hour to find.
// esp_partition_read has no such limit, and 0.49 MB in PSRAM is free.
const uint8_t* drive_library_map(size_t* out_len) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "drives");
    if (!part) {
        ESP_LOGW(TAG, "no 'drives' partition");
        return NULL;
    }

    // The header states the real size, so only the used bytes are read.
    uint8_t hdr[32];
    if (esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK ||
        memcmp(hdr, "MX5D", 4) != 0) {
        ESP_LOGW(TAG, "no drive library in the partition");
        return NULL;
    }
    uint16_t channels, drives;
    uint32_t records;
    memcpy(&channels, hdr + 6, 2);
    memcpy(&drives, hdr + 8, 2);
    memcpy(&records, hdr + 12, 4);
    size_t need = 32 + (size_t)channels * 16 + (size_t)drives * 32 + (size_t)records * 12;
    if (need > part->size) {
        ESP_LOGE(TAG, "library claims %u bytes, partition holds %u",
                 (unsigned)need, (unsigned)part->size);
        return NULL;
    }

    uint8_t* buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for %u bytes", (unsigned)need);
        return NULL;
    }
    if (esp_partition_read(part, 0, buf, need) != ESP_OK) {
        ESP_LOGE(TAG, "read failed");
        heap_caps_free(buf);
        return NULL;
    }
    if (out_len) *out_len = need;
    ESP_LOGI(TAG, "drive library loaded: %u bytes, %u drives, %u channels, %u records",
             (unsigned)need, drives, channels, (unsigned)records);
    return buf;
}
