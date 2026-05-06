idf_component_register(
    SRCS "datalogger.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        fatfs
        sdmmc

        log
        esp_timer
        freertos
        frigo
        rtc_rx8025t
        esp_hw_support
)
