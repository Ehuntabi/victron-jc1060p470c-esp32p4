file(GLOB_RECURSE SOURCES "*.c")

# Firmware del C6 (la radio) empotrado, para actualizarlo desde el propio 7" sin
# red ni cables: ver main/portal/slave_ota.c. Se genera compilando
# ~/esp_hosted_slave (idf.py -B build_c6 build) y copiando su
# network_adapter.bin aqui.
#
# NO se versiona (1,1 MB regenerables), asi que es OPCIONAL: si no esta -- en el
# CI, o en un clon recien hecho -- se compila sin el y el boton "Actualizar
# radio C6" avisa de que este firmware no lo lleva, en vez de romper el build.
set(SLAVE_FW "slave_fw/network_adapter.bin")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${SLAVE_FW}")
    set(SLAVE_EMBED EMBED_FILES ${SLAVE_FW})
else()
    set(SLAVE_EMBED "")
    message(STATUS "main: sin ${SLAVE_FW} -> se compila sin el firmware del C6")
endif()

idf_component_register(
    ${SLAVE_EMBED}
    SRCS       ${SOURCES}
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update
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
    json                 # cJSON para backup/restore de configuracion
    mbedtls              # base64 encode para BasicAuth HTTP
    esp_app_format       # esp_app_get_description() para la pagina About
    camera               # MIPI-CSI (Hito 1: bring-up + auto-brillo)
    ov02c10              # driver local del sensor OV02C10 (detect a 0x36)
    espcoredump          # log_capture.c: exportar coredump de un crash anterior
    sd_safe              # wrappers fopen/stat/mkdir/unlink/rename con cerrojo SD incluido
)

if(NOT SLAVE_EMBED STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLAVE_FW_EMBEBIDO=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE
    LV_LVGL_H_INCLUDE_SIMPLE
)

target_link_options(__idf_main PRIVATE "-Wl,--defsym=hal_disable_nan_tsf=0")
