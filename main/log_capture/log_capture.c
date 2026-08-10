#include "log_capture.h"
#include "camera.h"   /* camera_sd_bus_lock/unlock: evitar contencion SD<->camara */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_core_dump.h"

static log_capture_entry_t *s_buf = NULL;
static size_t   s_head  = 0;        /* siguiente posicion a escribir */
static size_t   s_count = 0;        /* entradas validas (max LOG_CAPTURE_MAX_LINES) */
static uint32_t s_seq   = 0;
static SemaphoreHandle_t s_mtx = NULL;
static vprintf_like_t    s_prev_vprintf = NULL;
static bool s_inited = false;

/* Parse el formato IDF estandar "X (12345) tag: msg\n" en una entrada.
 * Si no encaja con el formato, deja level=ESP_LOG_INFO, tag vacio y todo
 * el texto como msg. */
static void parse_idf_line(const char *line, log_capture_entry_t *e)
{
    e->level = ESP_LOG_INFO;
    e->ts_ms = 0;
    e->tag[0] = 0;
    e->msg[0] = 0;

    if (!line || !line[0]) return;

    const char *p = line;

    /* X */
    char first = p[0];
    if ((first == 'E' || first == 'W' || first == 'I' || first == 'D' || first == 'V')
        && p[1] == ' ' && p[2] == '(') {
        switch (first) {
            case 'E': e->level = ESP_LOG_ERROR;   break;
            case 'W': e->level = ESP_LOG_WARN;    break;
            case 'I': e->level = ESP_LOG_INFO;    break;
            case 'D': e->level = ESP_LOG_DEBUG;   break;
            case 'V': e->level = ESP_LOG_VERBOSE; break;
            default:  break;
        }
        p += 3;  /* salta "X (" */

        /* timestamp numerico */
        uint32_t ts = 0;
        while (*p >= '0' && *p <= '9') {
            ts = ts * 10 + (uint32_t)(*p - '0');
            p++;
        }
        e->ts_ms = ts;

        if (*p == ')' && p[1] == ' ') {
            p += 2;
            /* tag = hasta ':' */
            size_t tlen = 0;
            while (*p && *p != ':' && tlen < LOG_CAPTURE_MAX_TAG - 1) {
                e->tag[tlen++] = *p++;
            }
            e->tag[tlen] = 0;
            if (*p == ':') {
                p++;
                if (*p == ' ') p++;
            }
        } else {
            /* formato raro, vuelta al texto entero */
            p = line;
            e->level = ESP_LOG_INFO;
            e->ts_ms = 0;
            e->tag[0] = 0;
        }
    }

    /* msg: copiar lo que queda, quitar \n / \r del final, truncar a buffer */
    strncpy(e->msg, p, LOG_CAPTURE_MAX_MSG - 1);
    e->msg[LOG_CAPTURE_MAX_MSG - 1] = 0;
    size_t mlen = strlen(e->msg);
    while (mlen > 0 && (e->msg[mlen - 1] == '\n' || e->msg[mlen - 1] == '\r')) {
        e->msg[--mlen] = 0;
    }
}

/* vprintf hook. Formatea el log en un buffer local, parsea para meterlo en
 * el ring, y forwardea al vprintf previo (UART). */
static int log_capture_vprintf(const char *fmt, va_list args)
{
    /* Copiar args ANTES de usarlas, para poder reenviar al UART */
    va_list args_copy;
    va_copy(args_copy, args);

    char tmp[LOG_CAPTURE_MAX_MSG + LOG_CAPTURE_MAX_TAG + 64];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    /* args ya consumida; usamos args_copy para forward */

    if (n > 0 && s_buf && s_mtx) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
            log_capture_entry_t *slot = &s_buf[s_head];
            parse_idf_line(tmp, slot);
            slot->seq = ++s_seq;
            s_head = (s_head + 1) % LOG_CAPTURE_MAX_LINES;
            if (s_count < LOG_CAPTURE_MAX_LINES) s_count++;
            xSemaphoreGive(s_mtx);
        }
    }

    /* Forward al vprintf previo (UART) usando la copia */
    int r = n;
    if (s_prev_vprintf) {
        r = s_prev_vprintf(fmt, args_copy);
    }
    va_end(args_copy);
    return r;
}

void log_capture_init(void)
{
    if (s_inited) return;

    s_buf = heap_caps_malloc(sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        /* fallback DRAM (consume ~120 KB de DRAM, evitar si es posible) */
        s_buf = malloc(sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES);
    }
    if (!s_buf) return;  /* sin memoria, no instalamos el hook */

    memset(s_buf, 0, sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES);

    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        free(s_buf);
        s_buf = NULL;
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(log_capture_vprintf);
    s_inited = true;
}

uint32_t log_capture_last_seq(void)
{
    uint32_t v = 0;
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        v = s_seq;
        xSemaphoreGive(s_mtx);
    }
    return v;
}

void log_capture_clear(void)
{
    if (!s_mtx) return;
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        s_head = 0;
        s_count = 0;
        /* s_seq no se resetea: que siga creciendo para no confundir a consumers */
        if (s_buf) {
            memset(s_buf, 0, sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES);
        }
        xSemaphoreGive(s_mtx);
    }
}

/* Match case-insensitive substring */
static bool tag_matches(const char *tag, const char *substr)
{
    if (!substr || !substr[0]) return true;
    if (!tag || !tag[0]) return false;
    size_t tl = strlen(tag);
    size_t sl = strlen(substr);
    if (sl > tl) return false;
    for (size_t i = 0; i + sl <= tl; ++i) {
        size_t j = 0;
        for (; j < sl; ++j) {
            if (tolower((unsigned char)tag[i + j]) != tolower((unsigned char)substr[j])) break;
        }
        if (j == sl) return true;
    }
    return false;
}

static bool level_passes(esp_log_level_t lvl, uint8_t mask)
{
    if (mask == 0) return true;
    uint8_t bit = 0;
    switch (lvl) {
        case ESP_LOG_ERROR:   bit = 1u << 0; break;
        case ESP_LOG_WARN:    bit = 1u << 1; break;
        case ESP_LOG_INFO:    bit = 1u << 2; break;
        case ESP_LOG_DEBUG:   bit = 1u << 3; break;
        case ESP_LOG_VERBOSE: bit = 1u << 4; break;
        default: return false;
    }
    return (mask & bit) != 0;
}

size_t log_capture_get_lines(log_capture_entry_t *out, size_t max,
                              uint8_t level_mask, const char *tag_substr)
{
    if (!out || !max || !s_buf || !s_mtx) return 0;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    /* Recorrer en orden cronologico: el mas viejo esta en (s_head - s_count)
     * y el mas nuevo en (s_head - 1). Si overflow, s_count == LOG_CAPTURE_MAX_LINES. */
    size_t first = (s_head + LOG_CAPTURE_MAX_LINES - s_count) % LOG_CAPTURE_MAX_LINES;

    /* Pasada 1: copiamos las que pasan el filtro al final del out hasta max.
     * Si hay mas que max, queremos las MAS RECIENTES (las ultimas en el orden). */
    /* Estrategia simple: recorrer al reves desde la mas nueva y llenar out[max-1..0]. */
    size_t out_cnt = 0;
    for (size_t i = 0; i < s_count && out_cnt < max; ++i) {
        size_t idx = (s_head + LOG_CAPTURE_MAX_LINES - 1 - i) % LOG_CAPTURE_MAX_LINES;
        const log_capture_entry_t *e = &s_buf[idx];
        if (!level_passes(e->level, level_mask)) continue;
        if (!tag_matches(e->tag, tag_substr)) continue;
        out[max - 1 - out_cnt] = *e;
        out_cnt++;
    }
    (void)first;

    /* out ahora tiene out_cnt entradas en posicion [max-out_cnt .. max-1] en orden cronologico.
     * Compactar al inicio del array para que el caller las lea de 0 a out_cnt-1. */
    if (out_cnt > 0 && out_cnt < max) {
        memmove(&out[0], &out[max - out_cnt], out_cnt * sizeof(log_capture_entry_t));
    }

    xSemaphoreGive(s_mtx);
    return out_cnt;
}

/* Flag para evitar dos saves concurrentes. */
static volatile bool s_save_busy = false;
static portMUX_TYPE  s_busy_mux  = portMUX_INITIALIZER_UNLOCKED;

/* Task asincrona dedicada al SD save. El callback LVGL solo encola la
 * peticion (no bloquea), y esta task hace el fprintf real (lento, ~1-3s)
 * en background. Evita Task Watchdog del LVGL task. */
typedef struct {
    char path[80];
    log_capture_entry_t *snap;
    size_t snap_count;
    size_t snap_head;
} save_request_t;

static QueueHandle_t s_save_queue = NULL;
static const char *SAVE_TAG = "log_save";

static void log_save_task(void *arg)
{
    (void)arg;
    save_request_t req;
    while (1) {
        if (xQueueReceive(s_save_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(SAVE_TAG, "iniciando save -> %s (%u lineas)",
                 req.path, (unsigned)req.snap_count);

        /* Cerrojo de bus camara<->SD (evita INT WDT por contencion SDMMC). Si no
         * se consigue, omitir este save (best-effort; reintenta en el proximo). */
        if (!camera_sd_bus_lock(2500)) {
            ESP_LOGW(SAVE_TAG, "bus SD ocupado, omito save -> %s", req.path);
            free(req.snap);
            s_save_busy = false;
            continue;
        }
        FILE *f = fopen(req.path, "w");
        if (!f) {
            ESP_LOGE(SAVE_TAG, "fopen %s fallo", req.path);
            camera_sd_bus_unlock();
            free(req.snap);
            s_save_busy = false;
            continue;
        }

        for (size_t i = 0; i < req.snap_count; ++i) {
            size_t idx = (req.snap_head + LOG_CAPTURE_MAX_LINES - req.snap_count + i)
                         % LOG_CAPTURE_MAX_LINES;
            const log_capture_entry_t *e = &req.snap[idx];
            const char lvl_ch = (e->level == ESP_LOG_ERROR)   ? 'E' :
                                (e->level == ESP_LOG_WARN)    ? 'W' :
                                (e->level == ESP_LOG_INFO)    ? 'I' :
                                (e->level == ESP_LOG_DEBUG)   ? 'D' :
                                (e->level == ESP_LOG_VERBOSE) ? 'V' : '?';
            fprintf(f, "%lu [%c] %s: %s\n",
                    (unsigned long)e->ts_ms, lvl_ch, e->tag, e->msg);
        }

        fclose(f);
        camera_sd_bus_unlock();
        free(req.snap);
        s_save_busy = false;
        ESP_LOGI(SAVE_TAG, "save completo -> %s", req.path);
    }
}

esp_err_t log_capture_save_to_file(const char *path)
{
    if (!path || !s_buf) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    /* Lazy init de la task asincrona (1 vez) */
    if (!s_save_queue) {
        s_save_queue = xQueueCreate(2, sizeof(save_request_t));
        if (!s_save_queue) return ESP_ERR_NO_MEM;
        BaseType_t r = xTaskCreatePinnedToCore(log_save_task, "log_save",
                                               4096, NULL, 3, NULL,
                                               tskNO_AFFINITY);
        if (r != pdPASS) {
            vQueueDelete(s_save_queue);
            s_save_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    /* Rechazar save concurrente (segunda pulsacion mientras el primero
     * aun procesa). Test-and-set atomico para evitar que dos tareas pasen
     * el chequeo a la vez. */
    portENTER_CRITICAL(&s_busy_mux);
    if (s_save_busy) {
        portEXIT_CRITICAL(&s_busy_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_save_busy = true;
    portEXIT_CRITICAL(&s_busy_mux);

    /* Snapshot rapido del buffer (~118 KB PSRAM) bajo mutex.
     * Esto es lo unico que bloquea al caller (LVGL), ~1-5ms, no causa WDT. */
    save_request_t req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = 0;
    req.snap = heap_caps_malloc(
        sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES,
        MALLOC_CAP_SPIRAM);
    if (!req.snap) {
        s_save_busy = false;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(req.snap);
        s_save_busy = false;
        return ESP_ERR_TIMEOUT;
    }
    req.snap_count = s_count;
    req.snap_head  = s_head;
    memcpy(req.snap, s_buf, sizeof(log_capture_entry_t) * LOG_CAPTURE_MAX_LINES);
    xSemaphoreGive(s_mtx);

    /* Encolar a la task. La task hara fopen+fprintf+fclose+free(snap)
     * en background sin bloquear el LVGL task. */
    if (xQueueSend(s_save_queue, &req, 0) != pdTRUE) {
        free(req.snap);
        s_save_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ── Auto-save con reset reason + rotacion FIFO ────────────────────── */

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "intwdt";
        case ESP_RST_TASK_WDT:  return "taskwdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        default:                return "unknown";
    }
}

#define LOG_SD_DIR "/sdcard/logs"

/* Crea LOG_SD_DIR y su subcarpeta de dia si no existen. Llamar bajo
 * camera_sd_bus_lock (mismo patron que el resto de I/O de metadatos). */
static void ensure_log_daydir(const char *daydir)
{
    struct stat stx;
    if (stat(LOG_SD_DIR, &stx) != 0) mkdir(LOG_SD_DIR, 0777);
    if (stat(daydir, &stx) != 0) mkdir(daydir, 0777);
}

/* Lista LOG_SD_DIR/<dia>/<prefix>*.txt en todas las subcarpetas de dia,
 * ordena por mtime, borra los mas viejos hasta dejar `keep` EN TOTAL (misma
 * semantica que antes: conserva los N ficheros mas recientes, no N por dia).
 * Las carpetas de dia que quedan vacias tras borrar se eliminan. Usa malloc
 * temporal (~10 KB), libera al salir. */
static void rotate_files(const char *prefix, int keep)
{
    /* Sin cerrojo no se toca la SD: la rotacion puede esperar a la proxima vuelta,
     * pisar el GDMA de la camara no. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) return;
    DIR *dtop = opendir(LOG_SD_DIR);
    if (!dtop) { camera_sd_bus_unlock(); return; }

    typedef struct { char rel[80]; time_t mtime; } entry_t;
    const int MAX_ENTRIES = 128;
    entry_t *entries = malloc(sizeof(entry_t) * MAX_ENTRIES);
    if (!entries) { closedir(dtop); camera_sd_bus_unlock(); return; }

    size_t prefix_len = strlen(prefix);
    int n = 0;
    struct dirent *dday;
    while ((dday = readdir(dtop)) != NULL && n < MAX_ENTRIES) {
        if (dday->d_name[0] == '.') continue;
        char daydir[64];
        snprintf(daydir, sizeof(daydir), "%s/%s", LOG_SD_DIR, dday->d_name);
        DIR *dsub = opendir(daydir);
        if (!dsub) continue;   /* no es una carpeta de dia (o vacia/inaccesible) */
        struct dirent *de;
        while ((de = readdir(dsub)) != NULL && n < MAX_ENTRIES) {
            if (strncmp(de->d_name, prefix, prefix_len) != 0) continue;
            size_t l = strlen(de->d_name);
            if (l < 5 || strcmp(de->d_name + l - 4, ".txt") != 0) continue;
            char path[160];
            snprintf(path, sizeof(path), "%s/%s", daydir, de->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            snprintf(entries[n].rel, sizeof(entries[n].rel), "%s/%s", dday->d_name, de->d_name);
            entries[n].mtime = st.st_mtime;
            n++;
        }
        closedir(dsub);
    }
    closedir(dtop);
    camera_sd_bus_unlock();

    if (n > keep) {
        /* Bubble sort por mtime ascendente (mas viejo primero) */
        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 1; j < n; j++) {
                if (entries[i].mtime > entries[j].mtime) {
                    entry_t tmp = entries[i];
                    entries[i] = entries[j];
                    entries[j] = tmp;
                }
            }
        }
        /* Sin cerrojo no se borra: los logs viejos se quedan una vuelta mas.
         * 2026-07-26. */
        if (camera_sd_bus_lock(3000)) {
            for (int i = 0; i < n - keep; i++) {
                char path[160];
                snprintf(path, sizeof(path), "%s/%s", LOG_SD_DIR, entries[i].rel);
                unlink(path);
            }
            camera_sd_bus_unlock();
        }
        /* Carpetas de dia que se hayan quedado vacias: fuera (rmdir falla
         * en silencio si aun les queda algo dentro). */
        if (camera_sd_bus_lock(3000)) {
            DIR *dtop2 = opendir(LOG_SD_DIR);
            if (dtop2) {
                struct dirent *dday2;
                while ((dday2 = readdir(dtop2)) != NULL) {
                    if (dday2->d_name[0] == '.') continue;
                    char daydir[64];
                    snprintf(daydir, sizeof(daydir), "%s/%s", LOG_SD_DIR, dday2->d_name);
                    rmdir(daydir);
                }
                closedir(dtop2);
            }
            camera_sd_bus_unlock();
        }
    }
    free(entries);
}

/* Migra a LOG_SD_DIR/<YYYYMMDD>/ los log_*.txt y crash_*.txt sueltos que
 * quedaron en la raiz de la SD de antes de este cambio. La fecha se extrae
 * del propio nombre de fichero (sufijo fijo "_YYYYMMDD_HHMMSS.txt", 20
 * caracteres antes de la extension), no del reloj actual. */
void log_capture_migrate_legacy_flat_files(void)
{
    static const char *MTAG = "logmigrate";
    if (!camera_sd_bus_lock(3000)) return;
    DIR *d = opendir("/sdcard");
    camera_sd_bus_unlock();
    if (!d) return;

    /* Un fichero a la vez, SOLTANDO el bus entre cada uno: igual que el resto
     * de I/O de metadatos de este fichero, para no asfixiar la ventana GDMA de
     * la camara si hay muchos ficheros sueltos que migrar. */
    int moved = 0;
    for (;;) {
        if (!camera_sd_bus_lock(1000)) break;
        struct dirent *de = readdir(d);
        camera_sd_bus_unlock();
        if (!de) break;

        const char *n = de->d_name;
        if (strncmp(n, "log_", 4) != 0 && strncmp(n, "crash_", 6) != 0) continue;

        size_t l = strlen(n);
        if (l < 20 || strcmp(n + l - 4, ".txt") != 0) continue;
        const char *day = n + l - 20 + 1;   /* saltar el '_' del sufijo "_YYYYMMDD_HHMMSS.txt" */
        bool digits_ok = true;
        for (int i = 0; i < 8 && digits_ok; i++) {
            if (!isdigit((unsigned char)day[i])) digits_ok = false;
        }
        if (!digits_ok) continue;

        char daystr[9];
        memcpy(daystr, day, 8);
        daystr[8] = '\0';

        char daydir[32], oldpath[96], newpath[128];
        snprintf(daydir, sizeof(daydir), "%s/%s", LOG_SD_DIR, daystr);
        snprintf(oldpath, sizeof(oldpath), "/sdcard/%s", n);
        snprintf(newpath, sizeof(newpath), "%s/%s", daydir, n);

        if (!camera_sd_bus_lock(1000)) continue;   /* se reintenta en el proximo arranque */
        ensure_log_daydir(daydir);
        if (rename(oldpath, newpath) == 0) moved++;
        else ESP_LOGW(MTAG, "no pude migrar %s", n);
        camera_sd_bus_unlock();
    }
    while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
    closedir(d);
    camera_sd_bus_unlock();
    if (moved > 0) ESP_LOGI(MTAG, "migrados %d logs/crash sueltos a carpetas por dia", moved);
}

esp_err_t log_capture_autosave_now(int keep)
{
    if (!s_buf) return ESP_ERR_INVALID_STATE;

    const char *reason = reset_reason_name(esp_reset_reason());
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char daydir[32];
    snprintf(daydir, sizeof(daydir), LOG_SD_DIR "/%04d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    /* Si no se consigue el lock, no crear el directorio -> NO seguir: encolar
     * el guardado igualmente haria que log_save_task fallase el fopen (ENOENT)
     * en silencio, mientras aqui se devolveria OK. Mejor fallar ya, claro. */
    if (!camera_sd_bus_lock(2000)) return ESP_ERR_TIMEOUT;
    ensure_log_daydir(daydir);
    camera_sd_bus_unlock();

    char path[96];
    snprintf(path, sizeof(path),
             "%s/log_%s_%04d%02d%02d_%02d%02d%02d.txt", daydir,
             reason,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    esp_err_t err = log_capture_save_to_file(path);
    if (err == ESP_OK && keep > 0) {
        rotate_files("log_", keep);
    }
    return err;
}

/* ── Coredump (panic/TWDT/INT_WDT) -> /sdcard/crash_*.txt ──────────────────
 *
 * El coredump de ESP-IDF (particion "coredump" en partitions.csv) se escribe
 * SOLO POR HARDWARE justo al panicar, antes del reset — nunca depende del
 * hilo de ejecucion que crasheo, así que sobrevive exactamente los cuelgues
 * que el ring buffer en RAM (arriba) NO puede: ese buffer se pierde en el
 * reset porque nadie llega a volcarlo a tiempo. Aqui solo LEEMOS lo que el
 * hardware ya dejo grabado, en el arranque siguiente. */
esp_err_t log_capture_export_coredump(int keep)
{
    static const char *TAG = "coredump";
    if (esp_core_dump_image_check() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;   /* no habia coredump pendiente */
    }

    esp_core_dump_summary_t *sum = heap_caps_malloc(sizeof(*sum), MALLOC_CAP_SPIRAM);
    if (!sum) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_core_dump_get_summary(sum);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "coredump presente pero ilegible: %s", esp_err_to_name(err));
        free(sum);
        esp_core_dump_image_erase();   /* corrupto: no insistir en el siguiente boot */
        return err;
    }

    char reason[160] = "";
    esp_core_dump_get_panic_reason(reason, sizeof(reason));

    const char *rname = reset_reason_name(esp_reset_reason());
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char daydir[32];
    snprintf(daydir, sizeof(daydir), LOG_SD_DIR "/%04d%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    char path[96];
    snprintf(path, sizeof(path),
             "%s/crash_%s_%04d%02d%02d_%02d%02d%02d.txt", daydir,
             rname, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    if (!camera_sd_bus_lock(3000)) { free(sum); return ESP_ERR_TIMEOUT; }
    ensure_log_daydir(daydir);
    FILE *f = fopen(path, "w");
    if (!f) {
        camera_sd_bus_unlock();
        free(sum);
        return ESP_FAIL;
    }
    fprintf(f, "%s\n\n", reason[0] ? reason : "(motivo no disponible)");
    fprintf(f, "tarea: %s (tcb=0x%08" PRIx32 ")\n", sum->exc_task, sum->exc_tcb);
    fprintf(f, "exc_pc=0x%08" PRIx32 "  <- riscv32-esp-elf-addr2line -e joint_spl_145_control.elf -fCp 0x%08" PRIx32 "\n",
            sum->exc_pc, sum->exc_pc);
    fprintf(f, "mcause=0x%08" PRIx32 "  mtval=0x%08" PRIx32 "  mstatus=0x%08" PRIx32 "  mtvec=0x%08" PRIx32 "\n",
            sum->ex_info.mcause, sum->ex_info.mtval, sum->ex_info.mstatus, sum->ex_info.mtvec);
    fprintf(f, "ra=0x%08" PRIx32 "  sp=0x%08" PRIx32 "\n", sum->ex_info.ra, sum->ex_info.sp);
    for (int i = 0; i < 8; i++) {
        fprintf(f, "a%d=0x%08" PRIx32 "%s", i, sum->ex_info.exc_a[i], (i % 4 == 3) ? "\n" : "  ");
    }
    fprintf(f, "elf_sha256=%s\n", sum->app_elf_sha256);
    fprintf(f, "\nstackdump (%" PRIu32 " bytes, hex):\n", sum->exc_bt_info.dump_size);
    for (uint32_t i = 0; i < sum->exc_bt_info.dump_size; i++) {
        fprintf(f, "%02x", sum->exc_bt_info.stackdump[i]);
        if ((i % 32) == 31) fprintf(f, "\n");
    }
    fprintf(f, "\n");
    fclose(f);
    camera_sd_bus_unlock();
    free(sum);

    esp_core_dump_image_erase();   /* ya exportado: no releerlo en el proximo boot */
    ESP_LOGW(TAG, "coredump exportado -> %s", path);

    if (keep > 0) rotate_files("crash_", keep);
    return ESP_OK;
}
