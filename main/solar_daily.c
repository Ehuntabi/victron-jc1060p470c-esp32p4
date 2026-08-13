/* Produccion solar dia a dia. Ver solar_daily.h para el porque. */
#include "solar_daily.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera.h"   /* camera_sd_bus_lock: serializar la SD con el GDMA de la camara */

static const char *TAG = "solar_daily";

#define DIR_SD       "/sdcard/solar"
#define CSV_SD       DIR_SD "/produccion.csv"
#define NVS_NS       "solardaily"
/* Por debajo de esto se considera que el panel NO esta produciendo (ruido de
 * amanecer/anochecer). Sube o baja este numero para afinar "horas de produccion". */
#define UMBRAL_W     5
/* Si entre dos muestras pasa mas de esto, no se integra: hubo un hueco (reinicio,
 * BLE caido) y sumarlo entero falsearia el total. */
#define HUECO_MAX_S  60

static solar_day_t s_dias[SOLAR_DAILY_MAX_DAYS];   /* anillo de dias cerrados */
static int         s_n_dias = 0;                   /* cuantos hay (0..MAX) */
static solar_day_t s_hoy;
static solar_day_t s_pend;                         /* dia cerrado que aun no cabe en el CSV */
static bool        s_pend_valid = false;
static int64_t     s_last_us = 0;                  /* ultima muestra de panel integrada */
static int64_t     s_last_bat_us = 0;              /* ultima muestra de shunt integrada */
static int64_t     s_last_save_us = 0;
static SemaphoreHandle_t s_mtx = NULL;
static bool        s_sd_ok = false;

#define LOCK()   do { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mtx) xSemaphoreGive(s_mtx); } while (0)

static int32_t hoy_id(void)
{
    time_t t = time(NULL);
    if (t < 1704067200) return 0;          /* reloj aun sin hora: dia 0 */
    struct tm tm_l;
    localtime_r(&t, &tm_l);
    tm_l.tm_hour = 0; tm_l.tm_min = 0; tm_l.tm_sec = 0;
    return (int32_t)(mktime(&tm_l) / 86400);
}

static void guardar_hoy_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, "hoy", &s_hoy, sizeof(s_hoy));
    if (err != ESP_OK) ESP_LOGW(TAG, "solar diario (set) no persistio: %s", esp_err_to_name(err));
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "solar diario (commit) no persistio: %s", esp_err_to_name(err));
    nvs_close(h);
}

/* Anade el dia cerrado al CSV. Una linea por dia, se abre y cierra en el acto
 * (escritura corta, como el datalogger, para no chocar con la camara en el bus).
 *
 * Cerrojo de bus camara<->SD: escribir con el GDMA de la camara activo atasca el
 * SDMMC con las interrupciones bloqueadas -> INT WDT -> reinicio. Timeout corto
 * porque esto corre en la task de NimBLE con el lock de LVGL cogido; si el bus
 * esta ocupado se devuelve false y el llamante lo deja pendiente para la
 * siguiente muestra (misma politica que el datalogger). */
static bool anadir_csv(const solar_day_t *d)
{
    if (!s_sd_ok) return false;
    if (!camera_sd_bus_lock(200)) return false;
    struct stat st;
    bool cabecera = (stat(CSV_SD, &st) != 0);
    FILE *f = fopen(CSV_SD, "a");
    if (!f) {
        camera_sd_bus_unlock();
        ESP_LOGW(TAG, "no se puede escribir %s", CSV_SD);
        return false;
    }
    if (cabecera) fprintf(f, "fecha,kwh,horas,pico_w,kwh_consumo\n");
    time_t t = (time_t)d->day_id * 86400;
    struct tm tm_l;
    localtime_r(&t, &tm_l);
    fprintf(f, "%04d-%02d-%02d,%.3f,%.2f,%ld,%.3f\n",
            tm_l.tm_year + 1900, tm_l.tm_mon + 1, tm_l.tm_mday,
            d->kwh, d->horas, (long)d->pico_w, d->kwh_consumo);
    fclose(f);
    camera_sd_bus_unlock();
    return true;
}

static void cargar_csv(void)
{
    FILE *f = fopen(CSV_SD, "r");
    if (!f) return;
    char linea[96];
    if (!fgets(linea, sizeof(linea), f)) { fclose(f); return; }   /* cabecera */
    while (fgets(linea, sizeof(linea), f)) {
        int y, m, d;
        float kwh, horas, consumo = 0.0f;
        long pico;
        /* 6 campos = formato viejo (sin consumo); 7 = con consumo. */
        const int campos = sscanf(linea, "%d-%d-%d,%f,%f,%ld,%f",
                                  &y, &m, &d, &kwh, &horas, &pico, &consumo);
        if (campos < 6) continue;
        struct tm tm_l = {0};
        tm_l.tm_year = y - 1900; tm_l.tm_mon = m - 1; tm_l.tm_mday = d;
        solar_day_t sd = {
            .day_id = (int32_t)(mktime(&tm_l) / 86400),
            .kwh = kwh, .horas = horas, .pico_w = (int32_t)pico,
            .kwh_consumo = consumo,
        };
        if (s_n_dias < SOLAR_DAILY_MAX_DAYS) {
            s_dias[s_n_dias++] = sd;
        } else {   /* anillo: tira el mas antiguo */
            memmove(&s_dias[0], &s_dias[1], sizeof(s_dias[0]) * (SOLAR_DAILY_MAX_DAYS - 1));
            s_dias[SOLAR_DAILY_MAX_DAYS - 1] = sd;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "historico cargado: %d dias", s_n_dias);
}

void solar_daily_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    memset(&s_hoy, 0, sizeof(s_hoy));
    s_hoy.day_id = hoy_id();

    LOCK();
    /* Crear el directorio y leer el historico tambien con el cerrojo del bus, como
     * el resto de accesos a la tarjeta. Aqui la camara aun no ha arrancado, asi que
     * el cerrojo concede sin esperar; se toma igual por si cambia el orden de init. */
    if (camera_sd_bus_lock(3000)) {
        struct stat st;
        if (stat(DIR_SD, &st) != 0) {
            s_sd_ok = (mkdir(DIR_SD, 0777) == 0);
        } else {
            s_sd_ok = true;
        }
        cargar_csv();
        camera_sd_bus_unlock();
    }
    if (!s_sd_ok) ESP_LOGW(TAG, "sin SD: solo se guarda el dia en curso (NVS)");
    /* Dia en curso desde NVS (sobrevive a un reinicio a media tarde). */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        solar_day_t guardado;
        size_t len = sizeof(guardado);
        if (nvs_get_blob(h, "hoy", &guardado, &len) == ESP_OK && len == sizeof(guardado)) {
            if (guardado.day_id == s_hoy.day_id) {
                s_hoy = guardado;
                ESP_LOGI(TAG, "dia en curso recuperado: %.2f kWh", s_hoy.kwh);
            }
        }
        nvs_close(h);
    }
    UNLOCK();
}

void solar_daily_on_pv(int32_t watts)
{
    if (watts < 0 || !s_mtx) return;
    const int64_t ahora = esp_timer_get_time();

    LOCK();
    /* Dia cerrado que no se pudo escribir en su momento (bus SD ocupado): el MPPT
     * sigue emitiendo tambien de noche, asi que el reintento entra a los pocos
     * segundos. */
    if (s_pend_valid && anadir_csv(&s_pend)) s_pend_valid = false;

    /* Cambio de dia: cierra el que acaba y arranca el nuevo. */
    const int32_t id = hoy_id();
    if (id != s_hoy.day_id) {
        if (s_hoy.kwh > 0.0f && s_hoy.day_id > 0) {
            if (s_n_dias < SOLAR_DAILY_MAX_DAYS) {
                s_dias[s_n_dias++] = s_hoy;
            } else {
                memmove(&s_dias[0], &s_dias[1], sizeof(s_dias[0]) * (SOLAR_DAILY_MAX_DAYS - 1));
                s_dias[SOLAR_DAILY_MAX_DAYS - 1] = s_hoy;
            }
            if (!anadir_csv(&s_hoy)) {
                s_pend = s_hoy;
                s_pend_valid = true;
                ESP_LOGW(TAG, "tarjeta ocupada: el dia queda pendiente de escribir");
            }
            ESP_LOGI(TAG, "dia cerrado: %.2f kWh producidos en %.1f h (pico %ld W), "
                     "%.2f kWh consumidos",
                     s_hoy.kwh, s_hoy.horas, (long)s_hoy.pico_w, s_hoy.kwh_consumo);
        }
        memset(&s_hoy, 0, sizeof(s_hoy));
        s_hoy.day_id = id;
        s_last_us = 0;
        s_last_bat_us = 0;
    }

    if (s_last_us > 0) {
        const double dt_s = (double)(ahora - s_last_us) / 1e6;
        if (dt_s > 0.0 && dt_s <= HUECO_MAX_S) {
            s_hoy.kwh += (float)((double)watts * dt_s / 3600.0 / 1000.0);
            if (watts > UMBRAL_W) s_hoy.horas += (float)(dt_s / 3600.0);
        }
    }
    s_last_us = ahora;
    if (watts > s_hoy.pico_w) s_hoy.pico_w = watts;

    /* Persistir el dia en curso cada 5 min (barato: 16 bytes en NVS). */
    if (ahora - s_last_save_us > 300LL * 1000000LL) {
        s_last_save_us = ahora;
        guardar_hoy_nvs();
    }
    UNLOCK();
}

void solar_daily_on_battery(int32_t milli_amps, int32_t centi_volts)
{
    /* Solo descarga: corriente negativa = sale de la bateria. */
    if (milli_amps >= 0 || centi_volts <= 0 || !s_mtx) return;
    const int64_t ahora = esp_timer_get_time();

    LOCK();
    if (s_last_bat_us > 0) {
        const double dt_s = (double)(ahora - s_last_bat_us) / 1e6;
        if (dt_s > 0.0 && dt_s <= HUECO_MAX_S) {
            const double w = ((double)(-milli_amps) / 1000.0) * ((double)centi_volts / 100.0);
            s_hoy.kwh_consumo += (float)(w * dt_s / 3600.0 / 1000.0);
        }
    }
    s_last_bat_us = ahora;
    UNLOCK();
}

void solar_daily_get_today(solar_day_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_hoy;
    UNLOCK();
}

int solar_daily_get_days(solar_day_t *out, int max)
{
    if (!out || max <= 0) return 0;
    LOCK();
    const int n = (s_n_dias < max) ? s_n_dias : max;
    memcpy(out, &s_dias[s_n_dias - n], sizeof(solar_day_t) * (size_t)n);
    UNLOCK();
    return n;
}

float solar_daily_avg(int dias)
{
    if (dias <= 0) return 0.0f;
    LOCK();
    const int n = (s_n_dias < dias) ? s_n_dias : dias;
    float suma = 0.0f;
    for (int i = s_n_dias - n; i < s_n_dias; i++) suma += s_dias[i].kwh;
    const float media = (n > 0) ? suma / (float)n : 0.0f;
    UNLOCK();
    return media;
}

float solar_daily_avg_consumo(int dias)
{
    if (dias <= 0) return 0.0f;
    LOCK();
    const int n = (s_n_dias < dias) ? s_n_dias : dias;
    float suma = 0.0f;
    for (int i = s_n_dias - n; i < s_n_dias; i++) suma += s_dias[i].kwh_consumo;
    const float media = (n > 0) ? suma / (float)n : 0.0f;
    UNLOCK();
    return media;
}

void solar_daily_flush(void)
{
    if (!s_mtx) return;
    LOCK();
    guardar_hoy_nvs();
    UNLOCK();
}
