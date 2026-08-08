/* ne185_vlog.c — log de comparacion voltaje NE185 (crudo) vs SmartShunt.
 *
 * Escribe una linea por minuto en /sdcard/ne185v/AAAA-MM-DD.csv con los bytes
 * 12/13 de la trama NE185 SIN convertir y el voltaje que reporta el SmartShunt
 * por BLE. Objetivo: deducir por regresion la formula real del NE185, que hoy
 * esta documentada como (raw-30)/10 pero SIN verificar (ver ne185_vlog.h).
 *
 * No dibuja nada en pantalla. Se puede descargar por WiFi en /data/ne185v.csv.
 *
 * Patrones copiados del datalogger (probados en campo):
 *  - buffer en RAM + volcado periodico, para no abrir la SD cada minuto;
 *  - camera_sd_bus_lock() con timeout CORTO: el callback corre en la tarea
 *    esp_timer compartida y un bloqueo largo retrasaria los demas timers;
 *  - si el volcado falla, se conservan las muestras para el siguiente intento.
 */
#include "ne185_vlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ne185/ne185.h"
#include "dashboard_state.h"
#include "datalogger.h"      /* datalogger_sd_montada() */
#include "camera.h"          /* camera_sd_bus_lock/unlock */
#include "sd_safe.h"         /* stat/fopen/mkdir sueltos con el cerrojo incluido */

#define TAG              "ne185_vlog"
#define LOG_DIR          "/sdcard/ne185v"
#define SAMPLE_MS        60000          /* una muestra por minuto */
#define FLUSH_EVERY      10             /* volcar a SD cada 10 muestras (~10 min) */
#define MAX_ENTRIES      64             /* ~1 h de margen si la SD no responde */

typedef struct {
    char     timestamp[32];
    uint8_t  ne_raw_serv;    /* byte 12 crudo */
    uint8_t  ne_raw_mot;     /* byte 13 crudo */
    bool     ne_fresh;       /* hubo trama NE185 en los ultimos 30 s */
    uint16_t shunt_cv;       /* voltaje SmartShunt en centivoltios */
    bool     shunt_fresh;    /* dato del shunt reciente */
} vlog_entry_t;

static vlog_entry_t      s_buf[MAX_ENTRIES];
static int               s_count;        /* muestras pendientes de volcar */
static int               s_since_flush;
static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_timer;
static bool              s_ready;

static void get_timestamp(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    if (t.tm_year > 100) {
        /* Buffer holgado a proposito: gcc calcula el peor caso de cada %d y
         * con 24 bytes avisaba de posible truncado. */
        snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
                 (t.tm_year + 1900) & 0xFFFF, (t.tm_mon + 1) & 0xFF,
                 t.tm_mday & 0xFF, t.tm_hour & 0xFF,
                 t.tm_min & 0xFF, t.tm_sec & 0xFF);
    } else {
        uint64_t ms = esp_timer_get_time() / 1000;
        uint32_t s  = (uint32_t)(ms / 1000);
        uint32_t h  = s / 3600; s %= 3600;
        uint32_t m  = s / 60;   s %= 60;
        snprintf(buf, len, "BOOT+%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

/* Igual que en el datalogger: el fichero sale del timestamp de la muestra, no
 * de la hora del volcado, para que un volcado pasada la medianoche no meta las
 * muestras de ayer en el fichero de hoy. */
static bool ts_con_fecha(const char *ts)
{
    return ts[0] >= '0' && ts[0] <= '9' && ts[4] == '-';
}

static void get_day_filename(const char *ts, char *buf, size_t len)
{
    if (ts_con_fecha(ts)) snprintf(buf, len, LOG_DIR "/%.10s.csv", ts);
    else                  snprintf(buf, len, LOG_DIR "/boot.csv");   /* reloj sin hora */
}

/* true si las dos muestras van al mismo fichero diario. */
static bool mismo_dia(const char *a, const char *b)
{
    if (ts_con_fecha(a) != ts_con_fecha(b)) return false;
    return !ts_con_fecha(a) || strncmp(a, b, 10) == 0;
}

/* Vuelca a la SD. Devuelve sin escribir (conservando las muestras) si la SD no
 * esta montada o si el bus lo tiene ocupado la camara. */
static void flush_to_sd(void)
{
    if (!s_ready) return;
    if (!datalogger_sd_montada()) return;

    int pending;
    char first_ts[sizeof s_buf[0].timestamp];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    pending = s_count;
    if (pending > 0) snprintf(first_ts, sizeof first_ts, "%s", s_buf[0].timestamp);
    xSemaphoreGive(s_mutex);
    if (pending <= 0) return;

    /* Dia destino = el de la muestra mas antigua pendiente. Si el bloque cruza
     * medianoche se escribe solo hasta el corte; el resto sale en el proximo
     * volcado, ya al fichero del dia nuevo. */
    char path[80];
    get_day_filename(first_ts, path, sizeof path);
    struct stat st;

    /* Timeout corto a proposito: si la camara tiene el bus, se intenta luego.
     * El stat() de need_header TAMBIEN toca la SD: va DESPUES del cerrojo. */
    if (!camera_sd_bus_lock(200)) return;

    bool need_header = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) {
        camera_sd_bus_unlock();
        ESP_LOGW(TAG, "no se pudo abrir %s", path);
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        fclose(f);
        camera_sd_bus_unlock();
        return;
    }

    bool io_error = false;
    if (need_header) {
        if (fprintf(f, "timestamp,ne_raw_serv,ne_raw_mot,ne_fresh,"
                       "shunt_centivolts,shunt_fresh\n") < 0) {
            io_error = true;
        }
    }

    int written = 0;
    for (int i = 0; i < s_count && !io_error; ++i) {
        const vlog_entry_t *e = &s_buf[i];
        if (!mismo_dia(first_ts, e->timestamp)) break;   /* corte de dia */
        int r = fprintf(f, "%s,%u,%u,%d,%u,%d\n",
                        e->timestamp,
                        (unsigned)e->ne_raw_serv, (unsigned)e->ne_raw_mot,
                        e->ne_fresh ? 1 : 0,
                        (unsigned)e->shunt_cv,
                        e->shunt_fresh ? 1 : 0);
        if (r < 0 || ferror(f)) { io_error = true; break; }
        written++;
    }

    /* Solo se descartan las muestras que se escribieron bien. */
    if (written > 0) {
        int rest = s_count - written;
        if (rest > 0) memmove(s_buf, &s_buf[written], rest * sizeof(vlog_entry_t));
        s_count = rest;
    }
    xSemaphoreGive(s_mutex);

    fclose(f);
    camera_sd_bus_unlock();

    if (io_error) {
        ESP_LOGW(TAG, "error de escritura en %s (%d escritas)", path, written);
    } else if (written) {
        ESP_LOGD(TAG, "%d muestras -> %s", written, path);
    }
}

static void sample_timer_cb(void *arg)
{
    (void)arg;

    ne185_data_t ne;
    ne185_get(&ne);
    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_count < MAX_ENTRIES) {
            vlog_entry_t *e = &s_buf[s_count++];
            get_timestamp(e->timestamp, sizeof e->timestamp);
            e->ne_raw_serv = ne.battery1_raw;
            e->ne_raw_mot  = ne.battery2_raw;
            e->ne_fresh    = ne.fresh;
            e->shunt_cv    = snap.bat_has ? snap.bat_v_centi : 0;
            e->shunt_fresh = snap.bat_has && snap.bat_fresh;
        } else {
            /* Buffer lleno (SD ausente mucho rato): descartar la mas vieja. */
            memmove(s_buf, &s_buf[1], (MAX_ENTRIES - 1) * sizeof(vlog_entry_t));
            vlog_entry_t *e = &s_buf[MAX_ENTRIES - 1];
            get_timestamp(e->timestamp, sizeof e->timestamp);
            e->ne_raw_serv = ne.battery1_raw;
            e->ne_raw_mot  = ne.battery2_raw;
            e->ne_fresh    = ne.fresh;
            e->shunt_cv    = snap.bat_has ? snap.bat_v_centi : 0;
            e->shunt_fresh = snap.bat_has && snap.bat_fresh;
        }
        xSemaphoreGive(s_mutex);
    }

    if (++s_since_flush >= FLUSH_EVERY) {
        s_since_flush = 0;
        flush_to_sd();
    }
}

void ne185_vlog_flush(void)
{
    s_since_flush = 0;
    flush_to_sd();
}

esp_err_t ne185_vlog_init(void)
{
    if (s_ready) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "sin memoria para el mutex");
        return ESP_ERR_NO_MEM;
    }

    sd_mkdir(LOG_DIR, 0775, 3000);   /* si la SD no esta lista todavia, falla sin ruido */

    const esp_timer_create_args_t args = {
        .callback = sample_timer_cb,
        .name     = "ne185_vlog",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, (uint64_t)SAMPLE_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "log NE185 vs shunt activo (1 muestra/%d s -> %s)",
             SAMPLE_MS / 1000, LOG_DIR);
    return ESP_OK;
}
