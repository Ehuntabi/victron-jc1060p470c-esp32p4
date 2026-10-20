# OJO: file(GLOB) se evalua al CONFIGURAR. Un .c que aparezca despues NO entra
# en un build/ ya generado (el enlace falla con "undefined reference" y parece
# que el codigo esta mal). Al anadir un fichero nuevo: `idf.py reconfigure` o
# borrar build/. El CI y release.sh configuran de cero, asi que alli no pasa.
# CONFIGURE_DEPENDS NO vale aqui: el IDF incluye este CMakeLists en modo script
# y CMake lo rechaza ("invalid for script and find package modes").
# 17-oct-2026: al anadir portal/ota_pantalla.c.
file(GLOB_RECURSE SOURCES "*.c")

idf_component_register(
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
        # esp_hosted: preguntar la version de firmware de la radio (C6) al
        # arrancar y dejarla en el log. Ver main/main.c (radio_version_task).
        espressif__esp_hosted
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

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LOG_LOCAL_LEVEL=ESP_LOG_VERBOSE
    LV_LVGL_H_INCLUDE_SIMPLE
)

target_link_options(__idf_main PRIVATE "-Wl,--defsym=hal_disable_nan_tsf=0")

# ── LVGL: que un assert deje coredump (tanda 4 de la auditoria del 18-oct-2026)
#
# LVGL 8.4 trae "define LV_ASSERT_HANDLER while(1);" y con eso un assert se queda
# COLGADO hasta que el Task WDT reinicia: ni backtrace ni coredump, o sea que se
# pierde el motivo. El Kconfig de LVGL no tiene CONFIG_LV_ASSERT_HANDLER y por -D
# no se puede pasar (el valor lleva un ';' y CMake parte la lista), asi que se
# fuerza la inclusion de main/lvgl_assert_project.h ANTES de su config, que lo
# respeta porque va dentro de un "ifndef".
#
# TIENE QUE IR AQUI Y NO EN EL CMakeLists DE ARRIBA: alli el componente 'lvgl' no
# existe todavia (idf_component_get_property aborta) y con -include global el
# ensamblador de los .S se comia el stdlib.h ("unrecognized opcode").
# Se busca el TARGET y no con idf_component_get_property(): lvgl entra por
# esp_lvgl_port (no es un REQUIRES directo de main), asi que resolver el
# componente por nombre aqui aborta con "Failed to resolve component 'lvgl'".
# El target, en cambio, ya existe cuando se procesa este CMakeLists.
# El target del componente gestionado lleva el nombre COMPLETO del componente
# ("lvgl__lvgl", con el namespace como prefijo), no "lvgl": comprobado en
# build.ninja. Se prueban los dos por si algun dia cambia.
set(_lvgl_target "")
foreach(_t __idf_lvgl__lvgl __idf_lvgl)
    if(TARGET ${_t})
        set(_lvgl_target ${_t})
        break()
    endif()
endforeach()
if(_lvgl_target)
    target_compile_options(${_lvgl_target} PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/lvgl_assert_project.h")
    message(STATUS "LVGL: LV_ASSERT_HANDLER -> abort() y coredump (${_lvgl_target})")
else()
    message(WARNING "no encuentro el target de lvgl: un LV_ASSERT seguira colgando")
endif()

# ── Analisis estatico (auditoria 21-sep-2026) ──────────────────────────────────
# -fanalyzer de gcc 13 busca fugas, dobles libres, usos despues de free, NULL sin
# comprobar y desbordamientos. Es lento, asi que se activa solo cuando se pide:
#     ANALYZE=1 bash scripts/build_p4.sh build
if(DEFINED ENV{ANALYZE})
    target_compile_options(${COMPONENT_LIB} PRIVATE -fanalyzer -Wanalyzer-too-complex)
    message(STATUS "main: analisis estatico -fanalyzer ACTIVADO")
endif()
