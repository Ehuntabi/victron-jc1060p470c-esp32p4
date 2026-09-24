# Puente de pruebas: se graba en una placa ESP32 APARTE (no en la P4) y sirve
# para probar la P4 desde el PC: emula un Victron por BLE y un satelite por WiFi.
# NO forma parte del firmware del proyecto: es herramienta de banco.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(puente_pruebas)
