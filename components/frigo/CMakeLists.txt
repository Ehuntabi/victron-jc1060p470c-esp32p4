idf_component_register(
    SRCS "frigo.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        espressif__onewire_bus
        espressif__ds18b20
        esp_driver_ledc
        esp_driver_gpio
        nvs_flash
        log
        freertos
)
