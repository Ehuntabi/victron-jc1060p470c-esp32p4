idf_component_register(
    SRCS "rtc_rx8025t.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        log
        esp_common
        nvs_flash   # la marca de "el RTC ya guarda UTC" (14-oct-2026)
)
