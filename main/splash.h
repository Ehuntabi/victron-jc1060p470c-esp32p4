#pragma once
#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Muestra el splash a pantalla completa (logo + texto + barra de progreso).
 * Lee load_splash_mode() de NVS y, si == 0, no hace nada y devuelve false.
 * Devuelve true si se ha mostrado el splash. */
bool splash_show(void);

/* Elimina el splash. Llamar tras la inicializacion. Acepta tambien si no
 * se llamo a splash_show (no-op). */
void splash_hide(void);

#ifdef __cplusplus
}
#endif
