#include "alerts.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "alerts";
static const char *NS  = "alerts";

static int   s_freezer_min  = 30;
static float s_freezer_temp = -2.0f;
static int   s_soc_crit     = 30;
static int   s_soc_warn     = 60;

static void load_int(nvs_handle_t h, const char *k, int *out)
{
    int32_t v;
    if (nvs_get_i32(h, k, &v) == ESP_OK) *out = (int)v;
}

static void load_float(nvs_handle_t h, const char *k, float *out)
{
    int32_t v;
    if (nvs_get_i32(h, k, &v) == ESP_OK) *out = v / 100.0f;
}

/* Mismos limites que ya usa config_backup.c al importar un backup (el unico
 * sitio que tenia clamp hasta ahora): un valor de NVS corrupto (bit
 * volteado, escritura a medias) alimentaria umbrales de alarma absurdos o
 * negativos hasta el proximo ajuste manual en Ajustes. La carga normal de
 * NVS se habia quedado sin este clamp. Detectado por el usuario el
 * 09-sep-2026. */
static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clamp_float(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void alerts_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_int(h, "freezer_min", &s_freezer_min);
        load_float(h, "freezer_t", &s_freezer_temp);
        load_int(h, "soc_crit", &s_soc_crit);
        load_int(h, "soc_warn", &s_soc_warn);
        nvs_close(h);
    }
    s_freezer_min  = clamp_int(s_freezer_min, 0, 1440);
    s_freezer_temp = clamp_float(s_freezer_temp, -30.0f, 10.0f);
    s_soc_crit     = clamp_int(s_soc_crit, 0, 100);
    s_soc_warn     = clamp_int(s_soc_warn, 0, 100);
    ESP_LOGI(TAG, "Umbrales: freezer=%d min @ %.1fC, soc_crit=%d soc_warn=%d",
             s_freezer_min, s_freezer_temp, s_soc_crit, s_soc_warn);
}

static void save_int(const char *k, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, k, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

int   alerts_get_freezer_minutes(void) { return s_freezer_min; }
void  alerts_set_freezer_minutes(int m) { s_freezer_min = m; save_int("freezer_min", m); }
float alerts_get_freezer_temp_c(void)  { return s_freezer_temp; }
void  alerts_set_freezer_temp_c(float t) { s_freezer_temp = t; save_int("freezer_t", (int32_t)(t * 100.0f)); }
int   alerts_get_soc_critical(void)    { return s_soc_crit; }
void  alerts_set_soc_critical(int p)   { s_soc_crit = p; save_int("soc_crit", p); }
int   alerts_get_soc_warning(void)     { return s_soc_warn; }
void  alerts_set_soc_warning(int p)    { s_soc_warn = p; save_int("soc_warn", p); }
