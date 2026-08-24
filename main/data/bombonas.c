#include "bombonas.h"

#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera.h"          /* camera_sd_bus_lock: la SD se comparte con el GDMA */

static const char *TAG = "bombonas";
static const char *NVS_NS  = "bombonas";
static const char *NVS_KEY = "cambios";

/* El log en la tarjeta va con el resto de cosas del VEHICULO (no de un viaje):
 * una bombona no pertenece a ningun viaje concreto. */
#define CSV_DIR  "/sdcard/vehiculo"
#define CSV_RUTA CSV_DIR "/bombonas.csv"

static bombonas_t s;
static SemaphoreHandle_t s_mtx;

/* El reloj vale a partir de 2001; por debajo de eso aun no se ha puesto en hora. */
static bool hora_buena(time_t t) { return t >= 1000000000L; }

static void guardar_nvs_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    /* Blob y no una clave por cambio: asi el guardado es atomico y no puede
     * quedarse a medias con la mitad de las fechas nuevas y la mitad viejas. */
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &s, sizeof(s));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "no se ha guardado: %s", esp_err_to_name(err));
    nvs_close(h);
}

void bombonas_init(void)
{
    if (s_mtx == NULL) s_mtx = xSemaphoreCreateMutex();
    memset(&s, 0, sizeof(s));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s);
        bombonas_t tmp;
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
            /* Se valida antes de fiarse: un blob de un firmware viejo o a medio
             * escribir con n fuera de rango recorreria el array por fuera. */
            if (tmp.n <= BOMBONAS_MAX) s = tmp;
            else ESP_LOGW(TAG, "registro descartado, n=%u fuera de rango", tmp.n);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "cambios de bombona guardados: %u", s.n);
}

bool bombonas_cambio(void)
{
    if (!s_mtx) bombonas_init();
    time_t ahora = time(NULL);
    if (!hora_buena(ahora)) {
        /* Sin fecha de verdad no se apunta NADA. Guardar un cero o el uptime
         * daria una duracion inventada, y peor: creible. */
        ESP_LOGW(TAG, "el reloj no esta en hora, no apunto el cambio");
        return false;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s.n == BOMBONAS_MAX) {
        /* Lleno: se va la mas antigua. Se pierde historia vieja, no la reciente. */
        memmove(&s.cambios[0], &s.cambios[1], sizeof(s.cambios[0]) * (BOMBONAS_MAX - 1));
        s.n--;
    }
    s.cambios[s.n++] = (uint32_t)ahora;
    guardar_nvs_locked();
    xSemaphoreGive(s_mtx);

    /* Copia en la tarjeta, para poder llevarselo al ordenador. Es un extra: si
     * la SD esta ocupada o llena NO se deshace el apunte, que ya esta a salvo
     * en la NVS. */
    if (camera_sd_bus_lock(1000)) {
        struct stat st;
        mkdir(CSV_DIR, 0777);
        bool nuevo = !(stat(CSV_RUTA, &st) == 0 && st.st_size > 0);
        FILE *f = fopen(CSV_RUTA, "a");
        if (f) {
            if (nuevo) fprintf(f, "fecha_hora,dias_de_la_anterior\n");
            struct tm tm_l;
            localtime_r(&ahora, &tm_l);
            char cuando[20];
            strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l);
            /* La duracion que se apunta es la de la bombona que ACABA de
             * terminar, no la de la que se pone: esa aun no ha durado nada. */
            if (s.n >= 2) {
                double d = (double)(s.cambios[s.n - 1] - s.cambios[s.n - 2]) / 86400.0;
                fprintf(f, "%s,%.1f\n", cuando, d);
            } else {
                fprintf(f, "%s,\n", cuando);
            }
            fclose(f);
        }
        camera_sd_bus_unlock();
    }

    ESP_LOGI(TAG, "cambio de bombona apuntado (van %u)", s.n);
    return true;
}

bool bombonas_deshacer(void)
{
    if (!s_mtx) bombonas_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = (s.n > 0);
    if (ok) {
        s.n--;
        s.cambios[s.n] = 0;
        guardar_nvs_locked();
    }
    xSemaphoreGive(s_mtx);
    /* A proposito NO se toca el csv de la tarjeta: es un diario de lo que se
     * pulso, y reescribirlo para borrar una linea es mas peligroso que dejar
     * una linea de mas. La estadistica sale de la NVS. */
    if (ok) ESP_LOGI(TAG, "deshecho el ultimo cambio (quedan %u)", s.n);
    return ok;
}

void bombonas_get(bombonas_t *out)
{
    if (!out) return;
    if (!s_mtx) bombonas_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s;
    xSemaphoreGive(s_mtx);
}

int bombonas_dias_actual(void)
{
    bombonas_t b;
    bombonas_get(&b);
    if (b.n == 0) return -1;
    time_t ahora = time(NULL);
    if (!hora_buena(ahora)) return -1;
    if ((uint32_t)ahora < b.cambios[b.n - 1]) return -1;   /* reloj movido hacia atras */
    return (int)(((uint32_t)ahora - b.cambios[b.n - 1]) / 86400);
}

float bombonas_media_dias(void)
{
    bombonas_t b;
    bombonas_get(&b);
    if (b.n < 2) return -1.0f;   /* con un solo cambio no ha terminado ninguna */
    double suma = 0.0;
    int cuenta = 0;
    for (int i = 1; i < b.n; i++) {
        if (b.cambios[i] <= b.cambios[i - 1]) continue;   /* dato raro, fuera */
        suma += (double)(b.cambios[i] - b.cambios[i - 1]) / 86400.0;
        cuenta++;
    }
    if (cuenta == 0) return -1.0f;
    return (float)(suma / cuenta);
}
