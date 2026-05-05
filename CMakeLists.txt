cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Target obligatorio: ESP32-P4
# También se puede fijar con: idf.py set-target esp32p4
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32p4")
endif()

set(EXTRA_C_FLAGS "-Wl,--defsym=hal_disable_nan_tsf=0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(VictronSolarDisplay_JC1060P470C_I)

set_source_files_properties(
    $ENV{IDF_PATH}/components/bt/host/nimble/nimble/nimble/transport/uart_ll/src/hci_uart.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

spiffs_create_partition_image(spiffs
    "${CMAKE_SOURCE_DIR}/files"
    FLASH_IN_PROJECT
)
