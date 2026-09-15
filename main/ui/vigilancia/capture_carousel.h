/* capture_carousel.h - capturas de pantalla por WiFi/SD.
 * Extraido de ui.c (god-file) el 2026-08-13. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capturas por WiFi: navega a la pantalla idx (0..count-1) y devuelve su
 * nombre corto, o NULL si idx esta fuera de rango. Lo llama el handler HTTP
 * /captura?n=<i> en config_server.c. */
const char *ui_tour_goto_screen(int idx);
int ui_tour_screen_count(void);

/* true si la pantalla idx pinta la clave Wi-Fi o las claves Victron en claro
 * ("wifi"/"victron_keys" en TOUR_SET_NAMES). Lo consulta el handler HTTP
 * /captura ANTES de navegar (ver ui_tour_goto_screen). */
bool ui_tour_screen_needs_strict_auth(int idx);

/* Carrusel de captura a demanda: recorre las pantallas de datos, guarda un
 * JPEG de cada una en la SD (sobrescribe) y termina. Lo dispara el switch
 * de Settings->Display; el switch se apaga solo al acabar. */
void ui_start_capture_carousel(void);
bool ui_capture_carousel_running(void);

#ifdef __cplusplus
}
#endif
