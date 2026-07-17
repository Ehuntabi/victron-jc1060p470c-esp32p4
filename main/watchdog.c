#include "watchdog.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "display.h"

static const char *TAG = "WD";
static const char *NVS_NS = "wd";
static const char *KEY_COUNT = "count";
/* Motivo del reset FORZADO por el monitor SW (esp_restart -> aparece como ESP_RST_SW,
 * indistinguible de un reboot planificado). 0=ninguno 1=LVGL congelada 2=tarea muda. */
static const char *KEY_FORCED = "forced";

static uint32_t s_reset_count = 0;
static const char *s_reason_str = "Unknown";

/* Configuración del monitor LVGL */
#define WD_MONITOR_PERIOD_MS   3000   /* cadencia de chequeo */
#define WD_LVGL_LOCK_TIMEOUT   200    /* ms */
#define WD_LVGL_FAIL_THRESHOLD 3      /* fallos consecutivos para reset */

/* Vigilancia de tareas por heartbeat. Umbral generoso (margen amplio sobre
 * la cadencia mas lenta) para no provocar resets espureos. */
#define WD_TASK_TIMEOUT_US     (10LL * 1000000LL)  /* 10 s sin latido -> reset */

static volatile int64_t s_last_beat[WD_TASK_COUNT];   /* 0 = nunca latio */
static portMUX_TYPE s_beat_mux = portMUX_INITIALIZER_UNLOCKED;

void watchdog_heartbeat(wd_task_t task)
{
    if (task >= WD_TASK_COUNT) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_beat_mux);
    s_last_beat[task] = now;
    portEXIT_CRITICAL(&s_beat_mux);
}

/* Devuelve el indice de la primera tarea que lleva muda mas del umbral, o -1.
 * Ignora tareas que aun no han latido (s_last_beat == 0). */
static int wd_stalled_task(int64_t now)
{
    int stalled = -1;
    portENTER_CRITICAL(&s_beat_mux);
    for (int i = 0; i < WD_TASK_COUNT; i++) {
        if (s_last_beat[i] != 0 && (now - s_last_beat[i]) > WD_TASK_TIMEOUT_US) {
            stalled = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_beat_mux);
    return stalled;
}

static void wd_increment_counter_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t v = 0;
    nvs_get_u32(h, KEY_COUNT, &v);
    v++;
    nvs_set_u32(h, KEY_COUNT, v);
    nvs_commit(h);
    nvs_close(h);
    s_reset_count = v;
}

static uint32_t wd_load_counter_nvs(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, KEY_COUNT, &v);
        nvs_close(h);
    }
    return v;
}

/* Marca el motivo del reset forzado ANTES de esp_restart (1=LVGL, 2=tarea muda). */
static void wd_set_forced_reason_nvs(uint8_t code)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_FORCED, code);
    nvs_commit(h);
    nvs_close(h);
}

/* Task de un solo uso: escribe el motivo en NVS y muere. Se lanza aparte del
 * monitor porque nvs_open NO admite timeout: si el hang que disparo el
 * watchdog retiene el lock de flash/NVS, nvs_open bloquea para siempre. */
static void wd_reason_writer_task(void *arg)
{
    wd_set_forced_reason_nvs((uint8_t)(uintptr_t)arg);
    vTaskDelete(NULL);
}

/* Reinicio forzado GARANTIZADO: la escritura del motivo es best-effort en una
 * task aparte (si se cuelga en flash/NVS no arrastra al reset) y esta funcion
 * solo hace operaciones sin flash antes de esp_restart(), que SIEMPRE se
 * alcanza. En el caso sano el motivo se persiste en la ventana de 100 ms; en
 * un hang de clase flash se pierde solo el diagnostico, no el reinicio. */
static void wd_force_reset(uint8_t reason_code)
{
    xTaskCreate(wd_reason_writer_task, "wd_reason", 3072,
                (void *)(uintptr_t)reason_code, tskIDLE_PRIORITY + 1, NULL);
    bsp_display_brightness_set(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* Lee y BORRA el motivo forzado. Devuelve 0 si no habia. */
static uint8_t wd_take_forced_reason_nvs(void)
{
    nvs_handle_t h;
    uint8_t code = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_FORCED, &code);
        if (code != 0) { nvs_set_u8(h, KEY_FORCED, 0); nvs_commit(h); }
        nvs_close(h);
    }
    return code;
}

static const char *reason_to_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External pin";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_PANIC:     return "Panic";
        case ESP_RST_INT_WDT:   return "Watchdog (INT)";
        case ESP_RST_TASK_WDT:  return "Watchdog (TASK)";
        case ESP_RST_WDT:       return "Watchdog (otro)";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brown-out";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_UNKNOWN:   /* fall-through */
        default:                return "Unknown";
    }
}

static void wd_monitor_task(void *arg)
{
    int consecutive_fail = 0;
    /* Grace period inicial: ignorar fallos los primeros 30 s para que la
     * inicializacion completa (display, BLE, audio, SD, BT...) no provoque
     * resets espureos antes de que LVGL este realmente listo. */
    int64_t start_us = esp_timer_get_time();
    const int64_t GRACE_US = 30LL * 1000000LL;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WD_MONITOR_PERIOD_MS));
        int64_t now_us = esp_timer_get_time();
        bool in_grace = (now_us - start_us) < GRACE_US;

        if (lvgl_port_lock(WD_LVGL_LOCK_TIMEOUT)) {
            lvgl_port_unlock();
            consecutive_fail = 0;
        } else if (!in_grace) {
            consecutive_fail++;
            ESP_LOGW(TAG, "LVGL lock timeout (%d/%d)",
                     consecutive_fail, WD_LVGL_FAIL_THRESHOLD);
            if (consecutive_fail >= WD_LVGL_FAIL_THRESHOLD) {
                ESP_LOGE(TAG, "UI congelada — reset controlado (sin flush SD)");
                /* No hacemos flush a SD aqui: si el cuelgue lo causa el
                 * propio subsistema de SD/FAT (mutex retenido por una task
                 * muerta), datalogger_flush() / battery_history_flush()
                 * deadlock-an y el reset nunca ocurre. Preferimos perder
                 * el ultimo bloque de muestras antes que no reiniciar. El
                 * motivo (1) se escribe best-effort dentro de wd_force_reset,
                 * que garantiza el esp_restart. */
                wd_force_reset(1);   /* R4: registrar el motivo + reset */
            }
        }

        /* Vigilancia de tareas de app por heartbeat. Una tarea que dejo de
         * latir (colgada en UART/1-Wire) no la detecta el chequeo de LVGL. */
        if (!in_grace) {
            int stalled = wd_stalled_task(now_us);
            if (stalled >= 0) {
                ESP_LOGE(TAG, "Tarea %d sin latido >10s — reset controlado",
                         stalled);
                wd_force_reset(2);   /* R4: registrar el motivo + reset garantizado */
            }
        }
    }
}

esp_err_t watchdog_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_reason_str = reason_to_str(r);

    /* Si el ultimo reset fue por watchdog/panic (sintomas de cuelgue),
     * incrementamos el contador para diagnostico posterior. */
    bool is_wdt_reset = (r == ESP_RST_TASK_WDT ||
                         r == ESP_RST_INT_WDT ||
                         r == ESP_RST_WDT     ||
                         r == ESP_RST_PANIC);

    s_reset_count = wd_load_counter_nvs();
    /* R4: un reset FORZADO por este monitor llega como ESP_RST_SW (no como WDT), asi
     * que no lo contaba is_wdt_reset. Leer/borrar el motivo guardado y contarlo. */
    uint8_t forced = wd_take_forced_reason_nvs();
    if (is_wdt_reset || forced) {
        wd_increment_counter_nvs();
    }
    if (forced) {
        ESP_LOGW(TAG, "Reset FORZADO por watchdog SW: %s",
                 forced == 1 ? "UI/LVGL congelada" : "tarea sin latido");
    }
    ESP_LOGI(TAG, "Reset reason: %s; total WDT/panic/forzados: %lu",
             s_reason_str, (unsigned long)s_reset_count);

    /* Task monitor de salud de LVGL */
    BaseType_t ok = xTaskCreate(wd_monitor_task, "wd_monitor",
                                4096, NULL, 2, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

uint32_t watchdog_get_reset_count(void)
{
    return s_reset_count;
}

const char *watchdog_last_reset_reason(void)
{
    return s_reason_str;
}
