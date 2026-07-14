// config_server.h
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

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
