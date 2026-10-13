#ifndef UI_SETTINGS_RESTART_FLUSH_H
#define UI_SETTINGS_RESTART_FLUSH_H

/* Vuelca a la tarjeta y a la NVS TODO lo que vive en RAM, antes de un
 * esp_restart. Lo implementa settings_about.c y lo comparten los TRES caminos
 * de reinicio -- el switch de Wi-Fi, el boton Reiniciar y la escalera del AP
 * (config_server_ap.c) -- para que no puedan divergir: cuando cada uno volcaba
 * lo suyo, la escalera solo salvaba ruta.csv y cada reinicio automatico se
 * llevaba por delante hasta ~5 min de contadores de viaje, el dia de produccion
 * solar en curso y hasta ~10 min del log de voltaje NE185 (auditoria del
 * 14-oct-2026).
 *
 * Se declara en su propio header, sin dependencias, para que el portal pueda
 * llamarlo sin arrastrar la interfaz (settings_panel.h incluye ui_state.h). */
void ui_flush_all_before_restart(void);

#endif
