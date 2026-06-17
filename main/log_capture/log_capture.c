#include "log_capture.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

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

        FILE *f = fopen(req.path, "w");
        if (!f) {
            ESP_LOGE(SAVE_TAG, "fopen %s fallo", req.path);
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

/* Lista /sdcard/log_*.txt, ordena por mtime, borra los mas viejos hasta dejar `keep`.
 * Usa malloc temporal (~10 KB), libera al salir. */
static void rotate_logs(int keep)
{
    DIR *d = opendir("/sdcard");
    if (!d) return;

    typedef struct { char name[64]; time_t mtime; } entry_t;
    const int MAX_ENTRIES = 128;
    entry_t *entries = malloc(sizeof(entry_t) * MAX_ENTRIES);
    if (!entries) { closedir(d); return; }

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < MAX_ENTRIES) {
        if (strncmp(de->d_name, "log_", 4) != 0) continue;
        size_t l = strlen(de->d_name);
        if (l < 5 || l >= sizeof(entries[0].name) ||
            strcmp(de->d_name + l - 4, ".txt") != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "/sdcard/%s", de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        strncpy(entries[n].name, de->d_name, sizeof(entries[n].name) - 1);
        entries[n].name[sizeof(entries[n].name) - 1] = 0;
        entries[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);

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
        for (int i = 0; i < n - keep; i++) {
            char path[320];
            snprintf(path, sizeof(path), "/sdcard/%s", entries[i].name);
            unlink(path);
        }
    }
    free(entries);
}

esp_err_t log_capture_autosave_now(int keep)
{
    if (!s_buf) return ESP_ERR_INVALID_STATE;

    const char *reason = reset_reason_name(esp_reset_reason());
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char path[80];
    snprintf(path, sizeof(path),
             "/sdcard/log_%s_%04d%02d%02d_%02d%02d%02d.txt",
             reason,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    esp_err_t err = log_capture_save_to_file(path);
    if (err == ESP_OK && keep > 0) {
        rotate_logs(keep);
    }
    return err;
}
