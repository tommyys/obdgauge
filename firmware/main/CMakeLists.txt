idf_component_register(SRCS "main.cpp" "imu.c" "drive_source.c" "live_link.cpp" "flight_log.cpp" "drive_log.cpp" "drives_list.cpp" "serial_cmd.cpp" "sweep.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core gauge_ui gauge_platform espressif__esp_lvgl_adapter driver esp_driver_i2c esp_timer esp_partition nvs_flash esp_rom)
