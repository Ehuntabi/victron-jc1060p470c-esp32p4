file(GLOB SOURCES "*.c" "ui/*.c")

idf_component_register(
    SRCS       ${SOURCES}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        # ── Heredadas del proyecto original ───────────────────────────────────
        config_storage
        victron_ble
        dns_server
        esp_netif
        esp_lcd              # incluye MIPI-DSI en IDF 5.3
        spi_flash
        nvs_flash
        bt
        spiffs
        esp_event
        esp_wifi
        esp_http_server
        # ── Nuevas: ESP32-P4 / MIPI-DSI / JD9165BA / GT911 ──────────────────
        driver               # i2c_master + ledc + gpio (unificado en IDF 5.x)
        esp_hw_support       # contiene esp_ldo_regulator.h (IDF 5.3)
        esp_lcd_jd9165       # controlador panel JD9165BA
        esp_lcd_touch_gt911  # touch GT911
        esp_lvgl_port        # port LVGL Espressif (lvgl_port_add_disp_dsi)
        frigo                # DS18B20 + PWM ventilador
        rtc_rx8025t          # RTC RX8025T hora real
        datalogger           # SD + CSV logging
    battery_history      # Histórico corriente batería
    fatfs                # FATFS f_getfree para info SD
    log_cleanup          # Limpieza automatica de logs antiguos
    audio_es8311         # Codec audio ES8311 + amplificador NS4150
    alerts               # Umbrales configurables de alarmas
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE
    LV_LVGL_H_INCLUDE_SIMPLE
)

target_link_options(__idf_main PRIVATE "-Wl,--defsym=hal_disable_nan_tsf=0")
