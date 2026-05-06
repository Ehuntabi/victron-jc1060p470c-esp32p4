idf_component_register(
    SRCS "battery_history.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer esp_common log
)
