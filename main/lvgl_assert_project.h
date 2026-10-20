#pragma once
/* lvgl_assert_project.h — QUE UN LV_ASSERT DEJE VOLCADO, no un cuelgue mudo.
 *
 * LVGL 8.4 deja el manejador por defecto en "while(1)" (ver lv_conf_internal.h:
 * "#define LV_ASSERT_HANDLER while(1);" con el comentario "Halt by default").
 * Con eso, un assert de LVGL en la P4 se queda COLGADO: el Task WDT reinicia a
 * los 10 s y en el arranque siguiente no hay coredump que exportar (un WDT no
 * es un panic), asi que se pierde el motivo y el sitio exacto.
 *
 * CONFIG_LV_ASSERT_HANDLER NO EXISTE en el Kconfig de LVGL (solo
 * CONFIG_LV_ASSERT_HANDLER_INCLUDE), y pasar el manejador por -D no vale porque
 * el valor lleva un ';' y CMake parte la lista de definiciones. La via que si
 * funciona es esta: forzar la inclusion de ESTE fichero en todas las
 * compilaciones (-include, ver CMakeLists.txt). El lv_conf_internal.h respeta
 * la definicion porque va dentro de un "ifndef".
 *
 * Resultado: el assert imprime su expresion y su fichero:linea (LV_USE_LOG esta
 * encendido), aborta -> panic con backtrace -> coredump a la particion
 * "coredump" -> en el arranque siguiente se exporta a /sdcard/logs/<dia>/
 * crash_*.txt (ver log_capture_export_coredump). Se cambia "se reinicia y no se
 * sabe por que" por "se reinicia y el fichero dice donde".
 *
 * Va con ';' final y sin llaves: LV_ASSERT lo expande asi:
 *     if (!(expr)) { LV_LOG_ERROR(...); LV_ASSERT_HANDLER }
 */
#include <stdlib.h>   /* abort() */

#define LV_ASSERT_HANDLER do { abort(); } while (0);
