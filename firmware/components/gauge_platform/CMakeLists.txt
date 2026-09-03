idf_component_register(SRCS "ble_transport.cpp" "wifi_time.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core bt esp_timer esp_wifi esp_netif esp_event nvs_flash lwip)
