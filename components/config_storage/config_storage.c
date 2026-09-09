// config_storage.c
#include "config_storage.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cfgstore";

/* true solo si la clave nunca se ha guardado (primer arranque): toca
 * inicializarla con el default. Cualquier OTRO error de NVS (flash danada,
 * particion corrupta...) no es "clave ausente": tratarlo igual sobrescribe
 * un valor guardado bueno con el default por un fallo que puede ser
 * transitorio. En ese caso se usa el default solo para esta lectura, sin
 * tocar lo que haya en NVS. */
static bool nvs_missing_or_log(esp_err_t err, const char *what)
{
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    ESP_LOGW(TAG, "%s: error NVS inesperado (%s), uso el valor por defecto sin sobrescribir lo guardado",
             what, esp_err_to_name(err));
    return false;
}

#define AES_NAMESPACE  "victron"
#define AES_KEY        "aes_key"
#define WIFI_NAMESPACE "wifi"
#define BRIGHTNESS_NAMESPACE "display"
#define BRIGHTNESS_KEY       "brightness"
#define UI_VIEW_MODE_KEY     "view_mode"
#define SCREENSAVER_NAMESPACE "screensaver"
#define SS_ENABLED_KEY        "enabled"
#define SS_BRIGHT_KEY         "brightness"
#define SS_TIMEOUT_KEY        "timeout"

#define DEBUG_NAMESPACE       "debug"
#define VICTRON_DEBUG_KEY     "victron_debug"

#define VICTRON_DEVICES_NAMESPACE "victron_dev"
#define VICTRON_DEVICES_COUNT_KEY "count"
#define VICTRON_DEVICES_DATA_KEY  "devices"
/* El blob de VICTRON_DEVICES_DATA_KEY es un array crudo de
 * victron_device_config_t: si ese struct cambia de layout en una futura
 * version (reordenar/añadir campos manteniendo el mismo sizeof) el chequeo
 * de blob_size de abajo no lo detecta, y se leerian claves AES y MACs de
 * sitios equivocados sin ningun aviso. Version explicita para detectar ese
 * caso: si esta clave YA existe y no coincide, se trata igual que un blob
 * corrupto (reinicio a vacio, ver mas abajo) -- si NO existe (dato guardado
 * por firmware anterior a este cambio, mismo layout que hoy), se asume la
 * version actual, para no borrar la configuracion de quien ya tenia
 * dispositivos emparejados. Subir VICTRON_DEVICES_SCHEMA_VERSION el dia que
 * el struct cambie de verdad. Detectado por el usuario el 09-sep-2026. */
#define VICTRON_DEVICES_VERSION_KEY   "ver"
#define VICTRON_DEVICES_SCHEMA_VERSION 1

#define NE185_NAMESPACE       "ne185"
#define AUTOSTART_LOADS_KEY   "autostart"

esp_err_t load_brightness(uint8_t *brightness_out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, BRIGHTNESS_KEY, brightness_out);
    if (err != ESP_OK) {
        *brightness_out = 50; // default value
        if (nvs_missing_or_log(err, "brightness")) {
            nvs_set_u8(h, BRIGHTNESS_KEY, *brightness_out);
            nvs_commit(h);
        }
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_brightness(uint8_t brightness) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, BRIGHTNESS_KEY, brightness);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_aes_key(uint8_t key_out[16]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(AES_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t required = 16;
    err = nvs_get_blob(h, AES_KEY, key_out, &required);
    nvs_close(h);
    return err;
}

esp_err_t save_aes_key(const uint8_t key_in[16]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(AES_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, AES_KEY, key_in, 16);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_wifi_config(char *ssid_out, size_t *ssid_len,
                           char *pass_out, size_t *pass_len,
                           uint8_t *enabled_out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // Read SSID
    err = nvs_get_str(h, "ssid", ssid_out, ssid_len);
    if (err != ESP_OK) {
        // default SSID
        const char *d = "VictronConfig";
        size_t dlen = strlen(d) + 1;
        if (*ssid_len >= dlen) memcpy(ssid_out, d, dlen);
        *ssid_len = dlen;
        if (nvs_missing_or_log(err, "wifi_ssid")) nvs_set_str(h, "ssid", ssid_out);
    }

    // Read Password
    err = nvs_get_str(h, "password", pass_out, pass_len);
    if (err != ESP_OK) {
        // default empty
        if (*pass_len > 0) pass_out[0] = '\0';
        *pass_len = 1;
        if (nvs_missing_or_log(err, "wifi_password")) nvs_set_str(h, "password", pass_out);
    }

    // Read Enabled flag
    err = nvs_get_u8(h, "enabled", enabled_out);
    if (err != ESP_OK) {
        *enabled_out = 1; // default enabled
        if (nvs_missing_or_log(err, "wifi_enabled")) nvs_set_u8(h, "enabled", *enabled_out);
    }

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_wifi_config(const char *ssid,
                           const char *pass,
                           uint8_t enabled_out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "password", pass);
    if (err == ESP_OK) err = nvs_set_u8(h, "enabled", enabled_out);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_screensaver_settings(bool *enabled, uint8_t *brightness, uint16_t *timeout) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCREENSAVER_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t en = 1, bright = 20;
    uint16_t tout = 60;
    bool changed = false;

    // Persistir el default solo si la clave no existia (evita escritura +
    // commit NVS en cada carga, que desgasta flash innecesariamente) y solo
    // si de verdad no existia, no ante cualquier error (ver nvs_missing_or_log).
    esp_err_t e1 = nvs_get_u8(h, SS_ENABLED_KEY, &en);
    if (e1 != ESP_OK && nvs_missing_or_log(e1, "screensaver_enabled")) {
        nvs_set_u8(h, SS_ENABLED_KEY, en); changed = true;
    }
    esp_err_t e2 = nvs_get_u8(h, SS_BRIGHT_KEY, &bright);
    if (e2 != ESP_OK && nvs_missing_or_log(e2, "screensaver_brightness")) {
        nvs_set_u8(h, SS_BRIGHT_KEY, bright); changed = true;
    }
    esp_err_t e3 = nvs_get_u16(h, SS_TIMEOUT_KEY, &tout);
    if (e3 != ESP_OK && nvs_missing_or_log(e3, "screensaver_timeout")) {
        nvs_set_u16(h, SS_TIMEOUT_KEY, tout); changed = true;
    }

    if (enabled) *enabled = en;
    if (brightness) *brightness = bright;
    if (timeout) *timeout = tout;

    if (changed) nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_screensaver_settings(bool enabled, uint8_t brightness, uint16_t timeout) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCREENSAVER_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, SS_ENABLED_KEY, enabled ? 1 : 0);
    nvs_set_u8(h, SS_BRIGHT_KEY, brightness);
    nvs_set_u16(h, SS_TIMEOUT_KEY, timeout);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_screensaver_mode(uint8_t *mode_out, uint8_t *rotate_period_min_out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCREENSAVER_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t mode = 0;          // default: atenuar
    uint8_t period = 1;        // default: 1 min
    nvs_get_u8(h, "mode", &mode);
    nvs_get_u8(h, "rot_period", &period);
    if (period < 1) period = 1;
    if (period > 10) period = 10;
    if (mode_out) *mode_out = mode;
    if (rotate_period_min_out) *rotate_period_min_out = period;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_screensaver_mode(uint8_t mode, uint8_t rotate_period_min) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCREENSAVER_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (rotate_period_min < 1) rotate_period_min = 1;
    if (rotate_period_min > 10) rotate_period_min = 10;
    nvs_set_u8(h, "mode", mode);
    nvs_set_u8(h, "rot_period", rotate_period_min);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_victron_debug(bool *enabled_out)
{
    if (enabled_out == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(DEBUG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t v = 0;
    esp_err_t tmp = nvs_get_u8(h, VICTRON_DEBUG_KEY, &v);
    if (tmp != ESP_OK) {
        v = 0; // default: debug disabled
        if (nvs_missing_or_log(tmp, "victron_debug")) {
            nvs_set_u8(h, VICTRON_DEBUG_KEY, v);
            nvs_commit(h);
        }
    }

    *enabled_out = (v != 0);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_victron_debug(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DEBUG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, VICTRON_DEBUG_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_autostart_loads(bool *enabled_out)
{
    if (enabled_out == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NE185_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t v = 0;
    esp_err_t tmp = nvs_get_u8(h, AUTOSTART_LOADS_KEY, &v);
    if (tmp != ESP_OK) {
        v = 0; // default: deshabilitado
        if (nvs_missing_or_log(tmp, "autostart_loads")) {
            nvs_set_u8(h, AUTOSTART_LOADS_KEY, v);
            nvs_commit(h);
        }
    }

    *enabled_out = (v != 0);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_autostart_loads(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NE185_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, AUTOSTART_LOADS_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t load_victron_devices(victron_device_config_t *devices_out, 
                               uint8_t *count_out, 
                               uint8_t max_devices)
{
    if (devices_out == NULL || count_out == NULL || max_devices == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(VICTRON_DEVICES_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    bool changed = false;

    // Version del blob (ver comentario junto a VICTRON_DEVICES_VERSION_KEY).
    // Ausente = dato de firmware pre-version, se asume el layout actual.
    uint8_t ver = VICTRON_DEVICES_SCHEMA_VERSION;
    esp_err_t vtmp = nvs_get_u8(h, VICTRON_DEVICES_VERSION_KEY, &ver);
    bool version_mismatch = (vtmp == ESP_OK && ver != VICTRON_DEVICES_SCHEMA_VERSION);
    if (version_mismatch) {
        ESP_LOGW(TAG, "victron_devices: version de blob %u != %u esperada, reinicio a vacio",
                 ver, (unsigned)VICTRON_DEVICES_SCHEMA_VERSION);
    }

    // Load device count
    uint8_t count = 0;
    esp_err_t tmp;
    if (version_mismatch) {
        // Blob de version distinta: no fiarse de su layout ni intentar
        // migrar desde la clave AES legacy (esa migracion es solo para el
        // primer arranque de verdad). Tratarlo como vacio, igual que abajo
        // se hace con el blob de datos.
        count = 0;
        nvs_set_u8(h, VICTRON_DEVICES_COUNT_KEY, count);
        changed = true;
    } else {
    tmp = nvs_get_u8(h, VICTRON_DEVICES_COUNT_KEY, &count);
    if (tmp != ESP_OK) {
        // Solo migrar/inicializar a vacio si de verdad no habia clave
        // guardada. Un error real de NVS aqui NO debe borrar la lista de
        // dispositivos configurados (con sus claves AES) via una migracion
        // o un conteo a 0 escritos por encima de lo que hubiera.
        bool persist = nvs_missing_or_log(tmp, "victron_devices_count");
        // No devices configured yet, try to migrate from legacy single device
        uint8_t legacy_key[16] = {0};
        if (load_aes_key(legacy_key) == ESP_OK) {
            // Create a single device from legacy data.
            // Construir en un buffer local del tamano completo del blob:
            // devices_out lo dimensiona el caller con max_devices (puede ser
            // < VICTRON_MAX_DEVICES), asi que escribir el blob desde devices_out
            // leeria fuera de su buffer. El dispositivo migrado se releera mas
            // abajo desde NVS a stored_devices y se copiara a devices_out.
            count = 1;
            if (persist) {
                victron_device_config_t migrated[VICTRON_MAX_DEVICES];
                memset(migrated, 0, sizeof(migrated));
                strcpy(migrated[0].mac_address, "00:00:00:00:00:00");
                memcpy(migrated[0].aes_key, legacy_key, 16);
                strcpy(migrated[0].device_name, "Legacy Device");
                migrated[0].enabled = true;

                // Save the migrated data
                nvs_set_u8(h, VICTRON_DEVICES_COUNT_KEY, count);
                nvs_set_blob(h, VICTRON_DEVICES_DATA_KEY, migrated, sizeof(migrated));
                changed = true;
            }
        } else {
            count = 0;
            if (persist) {
                nvs_set_u8(h, VICTRON_DEVICES_COUNT_KEY, count);
                changed = true;
            }
        }
    }
    }

    if (count > VICTRON_MAX_DEVICES) {
        count = VICTRON_MAX_DEVICES;
        nvs_set_u8(h, VICTRON_DEVICES_COUNT_KEY, count);
        changed = true;
    }

    // Load devices data
    victron_device_config_t stored_devices[VICTRON_MAX_DEVICES];
    memset(stored_devices, 0, sizeof(stored_devices));
    size_t blob_size = sizeof(stored_devices);
    tmp = nvs_get_blob(h, VICTRON_DEVICES_DATA_KEY, stored_devices, &blob_size);
    if (version_mismatch || tmp != ESP_OK || blob_size != sizeof(stored_devices)) {
        // blob_size solo queda distinto de sizeof(stored_devices) si la
        // lectura fue ESP_OK con un tamano viejo/corrupto: en ese caso si
        // conviene reescribir. Un error real de NVS (no NOT_FOUND) dejaria
        // blob_size intacto -> nvs_missing_or_log decide si hay que avisar
        // en vez de sobrescribir el blob guardado con la lista vacia.
        bool persist = version_mismatch || (tmp == ESP_OK) || nvs_missing_or_log(tmp, "victron_devices_data");
        // Initialize empty devices (solo en RAM si no se va a persistir)
        for (size_t i = 0; i < VICTRON_MAX_DEVICES; ++i) {
            memset(&stored_devices[i], 0, sizeof(victron_device_config_t));
            strcpy(stored_devices[i].mac_address, "00:00:00:00:00:00");
            strcpy(stored_devices[i].device_name, "");
            stored_devices[i].enabled = false;
        }
        if (persist) {
            nvs_set_blob(h, VICTRON_DEVICES_DATA_KEY, stored_devices, sizeof(stored_devices));
            changed = true;
        }
    }

    // Sellar la version actual siempre que se haya escrito algo (blob
    // nuevo, migracion o reinicio por version distinta): la proxima carga
    // ya no debe volver a tratarla como "version ausente".
    if (changed) {
        nvs_set_u8(h, VICTRON_DEVICES_VERSION_KEY, VICTRON_DEVICES_SCHEMA_VERSION);
        nvs_commit(h);
    }

    nvs_close(h);

    // Copy devices to output (limit to max_devices)
    uint8_t copy_count = (count < max_devices) ? count : max_devices;
    for (size_t i = 0; i < max_devices; ++i) {
        if (i < copy_count) {
            memcpy(&devices_out[i], &stored_devices[i], sizeof(victron_device_config_t));
            // Ensure strings are null-terminated
            devices_out[i].mac_address[17] = '\0';
            devices_out[i].device_name[31] = '\0';
        } else {
            memset(&devices_out[i], 0, sizeof(victron_device_config_t));
            strcpy(devices_out[i].mac_address, "00:00:00:00:00:00");
            strcpy(devices_out[i].device_name, "");
            devices_out[i].enabled = false;
        }
    }

    *count_out = copy_count;
    return ESP_OK;
}

esp_err_t save_victron_devices(const victron_device_config_t *devices, 
                               uint8_t count)
{
    if (count > VICTRON_MAX_DEVICES) {
        count = VICTRON_MAX_DEVICES;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(VICTRON_DEVICES_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    victron_device_config_t stored_devices[VICTRON_MAX_DEVICES];
    memset(stored_devices, 0, sizeof(stored_devices));

    // Copy input devices and initialize unused slots
    for (size_t i = 0; i < VICTRON_MAX_DEVICES; ++i) {
        if (i < count && devices != NULL) {
            memcpy(&stored_devices[i], &devices[i], sizeof(victron_device_config_t));
            // Ensure strings are null-terminated
            stored_devices[i].mac_address[17] = '\0';
            stored_devices[i].device_name[31] = '\0';
        } else {
            strcpy(stored_devices[i].mac_address, "00:00:00:00:00:00");
            strcpy(stored_devices[i].device_name, "");
            memset(stored_devices[i].aes_key, 0, 16);
            stored_devices[i].enabled = false;
        }
    }

    err = nvs_set_u8(h, VICTRON_DEVICES_COUNT_KEY, count);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, VICTRON_DEVICES_DATA_KEY, stored_devices, sizeof(stored_devices));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, VICTRON_DEVICES_VERSION_KEY, VICTRON_DEVICES_SCHEMA_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t add_victron_device(const uint8_t mac[6], const uint8_t aes_key[16])
{
    if (mac == NULL || aes_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Load existing devices
    victron_device_config_t devices[VICTRON_MAX_DEVICES];
    uint8_t count = 0;
    esp_err_t err = load_victron_devices(devices, &count, VICTRON_MAX_DEVICES);
    if (err != ESP_OK) {
        return err;
    }

    // Format MAC address as string
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Check if device already exists (by MAC address)
    int existing_index = -1;
    for (int i = 0; i < count; i++) {
        if (strcasecmp(devices[i].mac_address, mac_str) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index >= 0) {
        // Update existing device
        memcpy(devices[existing_index].aes_key, aes_key, 16);
        devices[existing_index].enabled = true;
        // Keep existing device name if it has one
        if (strlen(devices[existing_index].device_name) == 0) {
            snprintf(devices[existing_index].device_name, sizeof(devices[existing_index].device_name), 
                     "Device %02X%02X", mac[4], mac[5]);
        }
    } else {
        // Add new device if we have space
        if (count >= VICTRON_MAX_DEVICES) {
            return ESP_ERR_NO_MEM; // No more space
        }

        // Add to the end of the list
        strcpy(devices[count].mac_address, mac_str);
        memcpy(devices[count].aes_key, aes_key, 16);
        snprintf(devices[count].device_name, sizeof(devices[count].device_name), 
                 "Device %02X%02X", mac[4], mac[5]);
        devices[count].enabled = true;
        count++;
    }

    // Save updated devices list
    return save_victron_devices(devices, count);
}

esp_err_t load_ui_view_mode(uint8_t *mode_out)
{
    if (mode_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_get_u8(h, UI_VIEW_MODE_KEY, mode_out);
    if (err != ESP_OK) {
        *mode_out = 1; // Default to UI_VIEW_MODE_DEFAULT_BATTERY (value 1)
        if (nvs_missing_or_log(err, "ui_view_mode")) {
            nvs_set_u8(h, UI_VIEW_MODE_KEY, *mode_out);
            nvs_commit(h);
        }
    }
    
    nvs_close(h);
    return ESP_OK;
}

esp_err_t save_ui_view_mode(uint8_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_u8(h, UI_VIEW_MODE_KEY, mode);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* ── Splash screen mode ─────────────────────────────────────────────────── */
#define SPLASH_KEY "splash"

esp_err_t load_splash_mode(uint8_t *mode_out)
{
    if (!mode_out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { *mode_out = 1; return ESP_OK; }
    uint8_t v = 1;
    esp_err_t g = nvs_get_u8(h, SPLASH_KEY, &v);
    if (g != ESP_OK && nvs_missing_or_log(g, "splash_mode")) {
        nvs_set_u8(h, SPLASH_KEY, v);
        nvs_commit(h);
    }
    nvs_close(h);
    *mode_out = (v > 1) ? 1 : v;
    return ESP_OK;
}

esp_err_t save_splash_mode(uint8_t mode)
{
    if (mode > 1) mode = 1;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, SPLASH_KEY, mode);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Zona horaria en POSIX TZ string ────────────────────────────────────── */
#define TZ_KEY  "tz"
#define TZ_DEFAULT "CET-1CEST,M3.5.0,M10.5.0/3"  /* Madrid */

esp_err_t load_timezone(char *tz_out, size_t maxlen)
{
    if (!tz_out || maxlen == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        strncpy(tz_out, TZ_DEFAULT, maxlen - 1);
        tz_out[maxlen - 1] = 0;
        return ESP_OK;
    }
    size_t sz = maxlen;
    err = nvs_get_str(h, TZ_KEY, tz_out, &sz);
    nvs_close(h);
    if (err != ESP_OK) {
        /* nvs_missing_or_log ya distingue "no habia nada guardado" (primer
         * arranque, normal) de un error real -- aqui interesa sobre todo
         * ESP_ERR_NVS_INVALID_LENGTH (el TZ guardado no cabe en maxlen):
         * antes se caia aqui en silencio y el usuario veia "Madrid" sin
         * ningun aviso de que su zona horaria guardada no se pudo leer.
         * Detectado por el usuario el 09-sep-2026. */
        nvs_missing_or_log(err, "timezone");
        strncpy(tz_out, TZ_DEFAULT, maxlen - 1);
        tz_out[maxlen - 1] = 0;
    }
    return ESP_OK;
}

esp_err_t save_timezone(const char *tz_str)
{
    if (!tz_str) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, TZ_KEY, tz_str);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Night mode (auto brightness por hora del RTC) ─────────────────────── */
#define NIGHT_EN_KEY     "nm_en"
#define NIGHT_START_KEY  "nm_start"
#define NIGHT_END_KEY    "nm_end"

esp_err_t load_night_mode(bool *enabled_out,
                          uint8_t *start_h_out,
                          uint8_t *end_h_out)
{
    if (!enabled_out || !start_h_out || !end_h_out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t en = 0, sh = 22, eh = 7;
    esp_err_t e1 = nvs_get_u8(h, NIGHT_EN_KEY,    &en);
    esp_err_t e2 = nvs_get_u8(h, NIGHT_START_KEY, &sh);
    esp_err_t e3 = nvs_get_u8(h, NIGHT_END_KEY,   &eh);
    /* commit solo si de verdad se escribio algun default (primer arranque);
     * un commit de NVS es una escritura a flash -- hacerlo incondicional en
     * cada load_night_mode() (se llama en cada apertura de Ajustes) desgasta
     * flash de balde cuando ya habia valores guardados y nada cambio.
     * Detectado por el usuario el 09-sep-2026. */
    bool changed = false;
    if (e1 != ESP_OK && nvs_missing_or_log(e1, "night_mode_enabled")) { nvs_set_u8(h, NIGHT_EN_KEY,    en); changed = true; }
    if (e2 != ESP_OK && nvs_missing_or_log(e2, "night_mode_start"))   { nvs_set_u8(h, NIGHT_START_KEY, sh); changed = true; }
    if (e3 != ESP_OK && nvs_missing_or_log(e3, "night_mode_end"))     { nvs_set_u8(h, NIGHT_END_KEY,   eh); changed = true; }
    if (changed) nvs_commit(h);
    nvs_close(h);

    *enabled_out    = (en != 0);
    *start_h_out    = sh > 23 ? 22 : sh;
    *end_h_out      = eh > 23 ? 7  : eh;
    return ESP_OK;
}

esp_err_t save_night_mode(bool enabled,
                          uint8_t start_h,
                          uint8_t end_h)
{
    if (start_h > 23) start_h = 22;
    if (end_h   > 23) end_h   = 7;

    nvs_handle_t h;
    esp_err_t err = nvs_open(BRIGHTNESS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NIGHT_EN_KEY,    enabled ? 1 : 0);
    nvs_set_u8(h, NIGHT_START_KEY, start_h);
    nvs_set_u8(h, NIGHT_END_KEY,   end_h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
// force screensaver defaults
