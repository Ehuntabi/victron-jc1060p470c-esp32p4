cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Target obligatorio: ESP32-P4
# También se puede fijar con: idf.py set-target esp32p4
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32p4")
endif()

set(EXTRA_C_FLAGS "-Wl,--defsym=hal_disable_nan_tsf=0")

# Version del firmware (About + nombre del .bin). ESP-IDF por defecto hace
# `git describe --tags` SIN filtro -> coge el tag mas reciente, y los tags
# `sdkconfig-known-good-*` rompian el About mostrando "sdkconfig-dev". Fijamos
# PROJECT_VER contando SOLO tags de version v*.
#
# El patron es "v*.*" y NO "v*.*.*": desde el 24-ago-2026 las versiones llevan
# DOS numeros (v1.12), no tres. El comodin de git abarca tambien el punto, asi
# que "v*.*" encuentra igual las nuevas que las viejas de tres numeros, y las
# `sdkconfig-known-good-*` siguen fuera porque no empiezan por v.
#
# ── REGLA DE VERSION (13-sep-2026, decision del usuario) ─────────────────────
# La version NUNCA lleva sufijos: ni "-dirty" ni "-N-g<hash>". Un .bin se llama
# v2.6 y punto, y lo que diga la pantalla es lo que dice el fichero.
#
# Como el trabajo suele ir por delante de los tags, lo que hay por encima del
# ultimo tag ES la version siguiente, +0.1:
#     arbol justo en el tag (limpio)          -> esa version   -> v2.5
#     commits nuevos y/o cambios sin commitear -> +0.1         -> v2.6
# Asi una tanda de trabajo tiene numero propio sin necesidad de estar ya
# etiquetada, y no aparece por ningun lado la palabra "-dirty".
# El +0.1 es solo el valor del trabajo en curso: la version de verdad la fija el
# tag que crea release.sh, y en cuanto existe el tag manda el tag.
execute_process(
    COMMAND git describe --always --tags --dirty --match "v*.*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE VER_DESCRITA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(VER_DESCRITA MATCHES "^v([0-9]+)\\.([0-9]+)(\\.[0-9]+)?$")
    set(PROJECT_VER "${VER_DESCRITA}")
elseif(VER_DESCRITA MATCHES "^v([0-9]+)\\.([0-9]+)-")
    math(EXPR VER_MINOR_SIGUIENTE "1 + ${CMAKE_MATCH_2}")
    set(PROJECT_VER "v${CMAKE_MATCH_1}.${VER_MINOR_SIGUIENTE}")
else()
    set(PROJECT_VER "v?")
endif()

# Y que se VUELVA A CALCULAR cuando cambie el git. Sin esto la version se congela
# en la del ultimo `idf.py reconfigure`: se compila y se graba, pero el About
# sigue enseñando la de hace veinte commits. Miente con autoridad -- creerias
# tener instalada la version publicada (visto el 24-ago-2026: la P4 decia v1.10.0
# llevando once commits por encima de esa etiqueta).
#
# `index` cambia al hacer commit y `HEAD` al cambiar de rama o de tag. Ojo: un
# fichero tocado y SIN anadir al indice no dispara el reconfigure, asi que tras
# aplicar un parche a mano la version embebida puede quedarse en la anterior
# hasta el siguiente reconfigure (lo detecta y lo avisa bin_con_version.sh).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD"
             "${CMAKE_CURRENT_LIST_DIR}/.git/index")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Con -O2, el analisis de GCC saca -Wstringop-truncation / -Wformat-truncation en
# el idiom seguro `strncpy(dst,src,N); dst[N-1]=0` y en snprintf de struct tm sobre
# buffers holgados (falsos positivos conocidos, 26 sitios). Los bajamos de ERROR a
# aviso (siguen visibles) en vez de reescribir string ops por todo el codigo.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)

project(joint_spl_145_control)

# .bin con la version en el nombre, para saber cual se lleva a la autocaravana.
# El nombre lo calcula scripts/bin_con_version.sh (ver alli la regla de version:
# siempre un numero limpio vX.Y, nunca "-dirty"); deja el original intacto al
# lado, que es el que usan `idf.py flash` y flash_args.
#
# Va como target que DEPENDE de `app` (el target de ESP-IDF que genera el .bin
# con esptool) y NO como POST_BUILD del .elf: el POST_BUILD corre al enlazar, y
# el .bin se genera en un paso POSTERIOR, de modo que ahi el .bin que hay
# delante es todavia el de la compilacion anterior (visto el 13-sep-2026:
# copiaba el binario viejo y ademas avisaba de que la version embebida no era la
# del arbol). Tampoco vale DEPENDS del fichero .bin: como no es una salida
# declarada en este directorio, CMake no lo ordena y la copia vuelve a salir
# antes de tiempo.
add_custom_target(bin_con_version ALL
    COMMAND sh "${CMAKE_SOURCE_DIR}/scripts/bin_con_version.sh"
            "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
    DEPENDS app
    COMMENT "Copiando el .bin con la version en el nombre"
    VERBATIM
)

set_source_files_properties(
    $ENV{IDF_PATH}/components/bt/host/nimble/nimble/nimble/transport/uart_ll/src/hci_uart.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

spiffs_create_partition_image(spiffs
    "${CMAKE_SOURCE_DIR}/files"
    FLASH_IN_PROJECT
)
