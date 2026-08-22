idf_component_register(SRCS "main.cpp" "imu.c" "drive_source.c"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core gauge_ui espressif__esp_lvgl_adapter driver esp_driver_i2c esp_timer esp_partition)
