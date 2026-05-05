idf_component_register(
    SRCS "rtc_rx8025t.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        log
        esp_common
)
