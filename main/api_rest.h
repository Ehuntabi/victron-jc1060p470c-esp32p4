#pragma once
/* Endpoints REST sobre el httpd ya activo en config_server.c.
 * Registra:
 *   GET  /api/system           - uptime, heap, freertos stats
 *   GET  /api/ne185/state      - estado parsed del NE185 (luces, tanks, bat)
 *   GET  /api/ne185/raw        - ultimo frame raw + counters frames_ok/fail
 *   POST /api/ne185/toggle/<b> - envia press (b = luz_int|luz_ext|bomba)
 *   GET  /api/state            - composicion de los anteriores en un solo JSON
 *
 * Llamar despues de httpd_start() en config_server.c.
 */
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void api_rest_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
