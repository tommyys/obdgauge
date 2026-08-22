idf_component_register(SRCS "ui.cpp" "views.cpp" "slide.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES gauge_core lvgl espressif__esp_lvgl_adapter esp_timer esp_mm)
