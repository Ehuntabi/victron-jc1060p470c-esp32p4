#include "stack_watch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "battery_history.h"
#include "ne185_vlog.h"
#include "log_cleanup.h"
#include "datalogger.h"
#include "portal/config_server_viaje.h"

static const char *TAG = "stackwatch";

typedef struct {
    const char   *name;
    TaskHandle_t (*get_handle)(void);
} watched_task_t;

static const watched_task_t s_tasks[] = {
    { "bh_flush_task",    battery_history_flush_task_handle },
    { "ne185_vlog_flush", ne185_vlog_flush_task_handle },
    { "log_cleanup_task", log_cleanup_task_handle },
    { "dl_flush_task",    datalogger_flush_task_handle },
    { "viaje_tick_task",  viaje_tick_task_handle },
};
#define N_TASKS (sizeof(s_tasks) / sizeof(s_tasks[0]))

static SemaphoreHandle_t s_mtx;   /* ver stack_watch_bloquea() */

void stack_watch_bloquea(void)
{
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}

void stack_watch_suelta(void)
{
    if (s_mtx) xSemaphoreGive(s_mtx);
}

static void stack_watch_cb(void *arg)
{
    (void)arg;
    /* Si el dueno de una tarea esta borrandola, se espera: leer el TCB despues
     * de vTaskDelete() es tocar memoria liberada. */
    stack_watch_bloquea();
    for (size_t i = 0; i < N_TASKS; ++i) {
        TaskHandle_t h = s_tasks[i].get_handle();
        if (!h) continue;   /* tarea aun no creada (fallo de xTaskCreate) */
        UBaseType_t words_free = uxTaskGetStackHighWaterMark(h);
        ESP_LOGI(TAG, "%-18s minimo_libre=%u bytes", s_tasks[i].name,
                 (unsigned)(words_free * sizeof(StackType_t)));
    }
    stack_watch_suelta();
}

void stack_watch_start(void)
{
    static esp_timer_handle_t timer;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = {
        .callback = &stack_watch_cb,
        .name = "stack_watch",
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 60ULL * 1000 * 1000);
    } else {
        ESP_LOGW(TAG, "no se pudo crear el timer de medicion");
    }
}
