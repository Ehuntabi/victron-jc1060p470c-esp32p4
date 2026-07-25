#pragma once
/* Utilidades compartidas entre las paginas de Ajustes.
 *
 * settings_panel.c habia crecido hasta 4.150 lineas y cada cambio obligaba a
 * leer medio fichero. Se esta partiendo por paginas (settings_victron_keys.c,
 * etc.); aqui vive lo poco que TODAS necesitan, para que cada pagina no tenga
 * que conocer el resto. */

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "ui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Contexto de una pagina de Ajustes. Las paginas se crean vacias en el arranque
 * y su contenido se construye la primera vez que el usuario entra (populate),
 * para no llenar el pool de LVGL de golpe. */
struct settings_page_ctx_s;
typedef struct settings_page_ctx_s settings_page_ctx_t;
struct settings_page_ctx_s {
    uint32_t accent;
    ui_state_t *ui;
    lv_obj_t *page;   /* pagina de menu asociada (para navegacion programatica) */
    void (*populate)(settings_page_ctx_t *ctx, lv_obj_t *page);
    bool populated;
    /* Extras (solo Wi-Fi los usa) */
    char wifi_ssid[40];
    char wifi_pass[68];
    uint8_t wifi_ap_enabled;
};

/* Etiqueta de log comun a todas las paginas de Ajustes ("UI_SETTINGS"). */
extern const char *TAG_SETTINGS;

/* Scrollbar visible (AUTO) con el estilo naranja del panel. Llamar despues de
 * crear la page para que se note que se puede deslizar. */
void style_settings_scrollbar(lv_obj_t *page);

/* Menu y pagina principal de Ajustes. Los dialogos modales de cualquier pagina
 * los necesitan para volver al sitio correcto al cerrarse. */
extern lv_obj_t *s_settings_menu;
extern lv_obj_t *s_settings_main_page;

/* Callback comun de los campos de texto (teclado en pantalla). */
void ta_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif
