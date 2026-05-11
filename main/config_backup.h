#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_BACKUP_PATH  "/sdcard/config_backup.json"

/* Exporta la configuración principal del firmware a un fichero JSON en SD.
 * Incluye: brightness, view_mode, timezone, night_mode, screensaver, alerts,
 * dispositivos Victron (MAC + AES + nombre), Wi-Fi SSID (la contraseña NO se
 * exporta por seguridad).
 * Devuelve ESP_OK si el fichero se ha escrito correctamente. */
esp_err_t config_backup_export(const char *path);

/* Lee el JSON de la ruta indicada y aplica los valores usando los save_*()
 * de config_storage. NO reinicia el equipo; el caller decide cuándo. */
esp_err_t config_backup_import(const char *path);

#ifdef __cplusplus
}
#endif
