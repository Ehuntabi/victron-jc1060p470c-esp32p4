#pragma once
/* Utilidades compartidas entre las paginas de Ajustes.
 *
 * settings_panel.c habia crecido hasta 4.150 lineas y cada cambio obligaba a
 * leer medio fichero. Se esta partiendo por paginas (settings_victron_keys.c,
 * etc.); aqui vive lo poco que TODAS necesitan, para que cada pagina no tenga
 * que conocer el resto. */

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

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
