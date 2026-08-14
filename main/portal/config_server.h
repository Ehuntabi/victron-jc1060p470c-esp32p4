// config_server.h
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// Deja en NVS una clave de AP valida y aleatoria (y migra las antiguas de
// fabrica). Llamar ANTES de ui_init(): Ajustes lee la clave una sola vez al
// arrancar y si no la mostraria desfasada. Necesita NVS ya inicializada.
void config_server_ensure_ap_password(void);

// Initialize & start Wi-Fi Soft-AP for configuration portal
esp_err_t wifi_ap_init(void);

// Mount SPIFFS and start HTTP config server
esp_err_t config_server_start(void);

// Lee las credenciales del portal web (Basic Auth) desde NVS, para mostrarlas
// en Ajustes -> Wi-Fi. Deja cadenas vacias si no hay.
void config_server_get_web_credentials(char *user, size_t ulen,
                                       char *pass, size_t plen);

// true si el servidor HTTP (portal web 192.168.4.1) esta activo ahora mismo.
// Sondear esto NO toca flash (solo comprueba un puntero), es seguro por tick.
bool config_server_is_running(void);

// Aplica en CALIENTE el on/off del Wi-Fi guardado en NVS ("wifi"/"enabled"),
// sin reiniciar la placa. Solo ENCOLA el trabajo y vuelve al instante: lo
// ejecuta la tarea de ciclo de vida del portal, porque levantar la radio es un
// RPC al C6 por SDIO (con espera de hasta 2 s) y bloquear ahi al hilo de LVGL
// congelaria la UI. Seguro de llamar desde un callback de LVGL.
//
// OJO: apagar el Wi-Fi deja al display mini SIN telemetria (la recibe por UDP
// broadcast sobre este mismo AP).
void config_server_request_wifi_apply(void);

// Levanta el portal HTTP si estaba parado por el auto-off. Igual que la
// anterior: solo encola, no bloquea. Usar en vez de config_server_start()
// desde LVGL o desde handlers de evento (monta SPIFFS y lee NVS).
void config_server_request_start(void);
