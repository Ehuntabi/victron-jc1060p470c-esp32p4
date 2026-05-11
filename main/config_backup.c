#include "config_backup.h"
#include "config_storage.h"
#include "alerts.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "CFG_BAK";

static void hex_encode(const uint8_t *in, size_t n, char *out_hex)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out_hex[i * 2 + 0] = h[(in[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = h[in[i] & 0xF];
    }
    out_hex[n * 2] = 0;
}

static int hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) < n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        char a = hex[i * 2], b = hex[i * 2 + 1];
        int va = (a >= '0' && a <= '9') ? a - '0'
               : (a >= 'a' && a <= 'f') ? a - 'a' + 10
               : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        int vb = (b >= '0' && b <= '9') ? b - '0'
               : (b >= 'a' && b <= 'f') ? b - 'a' + 10
               : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if (va < 0 || vb < 0) return -1;
        out[i] = (uint8_t)((va << 4) | vb);
    }
    return 0;
}

esp_err_t config_backup_export(const char *path)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);

    /* Display */
    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    uint8_t bri = 80;
    load_brightness(&bri);
    cJSON_AddNumberToObject(disp, "brightness", bri);

    uint8_t view_mode = 1;
    load_ui_view_mode(&view_mode);
    cJSON_AddNumberToObject(disp, "view_mode", view_mode);

    char tz[48] = {0};
    load_timezone(tz, sizeof(tz));
    cJSON_AddStringToObject(disp, "timezone", tz);

    /* Night mode */
    bool nm_en = false;
    uint8_t nm_s = 22, nm_e = 7, nm_b = 15;
    load_night_mode(&nm_en, &nm_s, &nm_e, &nm_b);
    cJSON *nm = cJSON_AddObjectToObject(disp, "night_mode");
    cJSON_AddBoolToObject(nm, "enabled", nm_en);
    cJSON_AddNumberToObject(nm, "start_h", nm_s);
    cJSON_AddNumberToObject(nm, "end_h", nm_e);
    cJSON_AddNumberToObject(nm, "brightness", nm_b);

    /* Screensaver */
    bool ss_en = false;
    uint8_t ss_bri = 30;
    uint16_t ss_to = 60;
    load_screensaver_settings(&ss_en, &ss_bri, &ss_to);
    uint8_t ss_mode = 0, ss_period = 5;
    load_screensaver_mode(&ss_mode, &ss_period);
    cJSON *ss = cJSON_AddObjectToObject(disp, "screensaver");
    cJSON_AddBoolToObject(ss, "enabled", ss_en);
    cJSON_AddNumberToObject(ss, "brightness", ss_bri);
    cJSON_AddNumberToObject(ss, "timeout_s", ss_to);
    cJSON_AddNumberToObject(ss, "mode", ss_mode);
    cJSON_AddNumberToObject(ss, "rotate_period_min", ss_period);

    /* Alerts */
    cJSON *al = cJSON_AddObjectToObject(root, "alerts");
    cJSON_AddNumberToObject(al, "freezer_minutes", alerts_get_freezer_minutes());
    cJSON_AddNumberToObject(al, "freezer_temp_c", alerts_get_freezer_temp_c());
    cJSON_AddNumberToObject(al, "soc_critical", alerts_get_soc_critical());
    cJSON_AddNumberToObject(al, "soc_warning", alerts_get_soc_warning());

    /* Wi-Fi (sin password por seguridad) */
    char ssid[33] = {0}, pass[65] = {0};
    size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
    uint8_t wifi_en = 0;
    load_wifi_config(ssid, &ssid_len, pass, &pass_len, &wifi_en);
    cJSON *wf = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wf, "ssid", ssid);
    cJSON_AddBoolToObject(wf, "enabled", wifi_en != 0);

    /* Victron devices */
    cJSON *vict = cJSON_AddArrayToObject(root, "victron_devices");
    victron_device_config_t devs[VICTRON_MAX_DEVICES];
    uint8_t count = 0;
    load_victron_devices(devs, &count, VICTRON_MAX_DEVICES);
    char hex_key[33];
    for (uint8_t i = 0; i < count; i++) {
        hex_encode(devs[i].aes_key, 16, hex_key);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "mac", devs[i].mac_address);
        cJSON_AddStringToObject(d, "aes_key_hex", hex_key);
        cJSON_AddStringToObject(d, "name", devs[i].device_name);
        cJSON_AddBoolToObject(d, "enabled", devs[i].enabled);
        cJSON_AddItemToArray(vict, d);
    }

    /* Victron debug */
    bool vdbg = false;
    load_victron_debug(&vdbg);
    cJSON_AddBoolToObject(root, "victron_debug", vdbg);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    FILE *fp = fopen(path, "w");
    if (!fp) { free(json); ESP_LOGE(TAG, "fopen %s", path); return ESP_FAIL; }
    size_t len = strlen(json);
    size_t wrote = fwrite(json, 1, len, fp);
    fclose(fp);
    free(json);
    ESP_LOGI(TAG, "Exportado: %s (%u bytes)", path, (unsigned)wrote);
    return wrote == len ? ESP_OK : ESP_FAIL;
}

esp_err_t config_backup_import(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { ESP_LOGE(TAG, "No se puede abrir %s", path); return ESP_FAIL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024) { fclose(fp); return ESP_FAIL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return ESP_ERR_NO_MEM; }
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = 0;
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGE(TAG, "JSON invalido"); return ESP_FAIL; }

    /* Display */
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (disp) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(disp, "brightness")) && cJSON_IsNumber(v))
            save_brightness((uint8_t)v->valueint);
        if ((v = cJSON_GetObjectItem(disp, "view_mode")) && cJSON_IsNumber(v))
            save_ui_view_mode((uint8_t)v->valueint);
        if ((v = cJSON_GetObjectItem(disp, "timezone")) && cJSON_IsString(v))
            save_timezone(v->valuestring);

        cJSON *nm = cJSON_GetObjectItem(disp, "night_mode");
        if (nm) {
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(nm, "enabled"));
            int s = cJSON_GetObjectItem(nm, "start_h")  ? cJSON_GetObjectItem(nm, "start_h")->valueint : 22;
            int e = cJSON_GetObjectItem(nm, "end_h")    ? cJSON_GetObjectItem(nm, "end_h")->valueint   : 7;
            int b = cJSON_GetObjectItem(nm, "brightness")? cJSON_GetObjectItem(nm, "brightness")->valueint : 15;
            save_night_mode(en, (uint8_t)s, (uint8_t)e, (uint8_t)b);
        }
        cJSON *ss = cJSON_GetObjectItem(disp, "screensaver");
        if (ss) {
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(ss, "enabled"));
            int b  = cJSON_GetObjectItem(ss, "brightness")? cJSON_GetObjectItem(ss, "brightness")->valueint : 30;
            int to = cJSON_GetObjectItem(ss, "timeout_s") ? cJSON_GetObjectItem(ss, "timeout_s")->valueint  : 60;
            int md = cJSON_GetObjectItem(ss, "mode")      ? cJSON_GetObjectItem(ss, "mode")->valueint      : 0;
            int pe = cJSON_GetObjectItem(ss, "rotate_period_min") ? cJSON_GetObjectItem(ss, "rotate_period_min")->valueint : 5;
            save_screensaver_settings(en, (uint8_t)b, (uint16_t)to);
            save_screensaver_mode((uint8_t)md, (uint8_t)pe);
        }
    }

    cJSON *al = cJSON_GetObjectItem(root, "alerts");
    if (al) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(al, "freezer_minutes")) && cJSON_IsNumber(v))
            alerts_set_freezer_minutes(v->valueint);
        if ((v = cJSON_GetObjectItem(al, "freezer_temp_c")) && cJSON_IsNumber(v))
            alerts_set_freezer_temp_c((float)v->valuedouble);
        if ((v = cJSON_GetObjectItem(al, "soc_critical")) && cJSON_IsNumber(v))
            alerts_set_soc_critical(v->valueint);
        if ((v = cJSON_GetObjectItem(al, "soc_warning")) && cJSON_IsNumber(v))
            alerts_set_soc_warning(v->valueint);
    }

    /* Victron devices: sustitución completa */
    cJSON *vict = cJSON_GetObjectItem(root, "victron_devices");
    if (cJSON_IsArray(vict)) {
        victron_device_config_t devs[VICTRON_MAX_DEVICES];
        memset(devs, 0, sizeof(devs));
        uint8_t count = 0;
        cJSON *d;
        cJSON_ArrayForEach(d, vict) {
            if (count >= VICTRON_MAX_DEVICES) break;
            const char *mac  = cJSON_GetObjectItem(d, "mac") ? cJSON_GetObjectItem(d, "mac")->valuestring : NULL;
            const char *khex = cJSON_GetObjectItem(d, "aes_key_hex") ? cJSON_GetObjectItem(d, "aes_key_hex")->valuestring : NULL;
            const char *name = cJSON_GetObjectItem(d, "name") ? cJSON_GetObjectItem(d, "name")->valuestring : "";
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(d, "enabled"));
            if (!mac || !khex) continue;
            strncpy(devs[count].mac_address, mac, sizeof(devs[count].mac_address) - 1);
            strncpy(devs[count].device_name, name, sizeof(devs[count].device_name) - 1);
            if (hex_decode(khex, devs[count].aes_key, 16) != 0) continue;
            devs[count].enabled = en;
            count++;
        }
        save_victron_devices(devs, count);
    }

    cJSON *vd = cJSON_GetObjectItem(root, "victron_debug");
    if (vd) save_victron_debug(cJSON_IsTrue(vd));

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Importado desde %s", path);
    return ESP_OK;
}
