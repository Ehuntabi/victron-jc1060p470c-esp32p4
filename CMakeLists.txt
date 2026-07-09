cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Target obligatorio: ESP32-P4
# También se puede fijar con: idf.py set-target esp32p4
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32p4")
endif()

set(EXTRA_C_FLAGS "-Wl,--defsym=hal_disable_nan_tsf=0")

# Version del firmware (About). ESP-IDF por defecto hace `git describe --tags`
# SIN filtro -> coge el tag mas reciente, y los tags `sdkconfig-known-good-*`
# rompian el About mostrando "sdkconfig-dev". Fijamos PROJECT_VER contando SOLO
# tags de version v* -> queda "v1.0.0-N-g<hash>" (el formateador lo muestra como
# "v1.0.0-dev"). Inmune a cualquier tag que no sea v*.
execute_process(
    COMMAND git describe --always --tags --dirty --match "v*.*.*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PROJECT_VER)
    set(PROJECT_VER "v?")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Con -O2, el analisis de GCC saca -Wstringop-truncation / -Wformat-truncation en
# el idiom seguro `strncpy(dst,src,N); dst[N-1]=0` y en snprintf de struct tm sobre
# buffers holgados (falsos positivos conocidos, 26 sitios). Los bajamos de ERROR a
# aviso (siguen visibles) en vez de reescribir string ops por todo el codigo.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)

project(VictronSolarDisplay_JC1060P470C_I)

set_source_files_properties(
    $ENV{IDF_PATH}/components/bt/host/nimble/nimble/nimble/transport/uart_ll/src/hci_uart.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

spiffs_create_partition_image(spiffs
    "${CMAKE_SOURCE_DIR}/files"
    FLASH_IN_PROJECT
)
