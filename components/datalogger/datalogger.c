#include "datalogger.h"
#include "camera.h"          /* camera_sd_bus_lock/unlock: evitar contencion SD<->camara */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/stat.h>

static void flush_pending_to_sd_impl(void);
static const char *TAG = "DATALOGGER";

#define MOUNT_POINT "/sdcard"
#define LOG_DIR     MOUNT_POINT "/frigo"
#define FLUSH_INTERVAL_MS 60000  /* volcado cada 60s */

/* --- SD en modo SPI (bus SPI3), NO SDMMC ---
 * Saca la SD del periferico SDMMC que comparte con el C6 (WiFi por SDIO en
 * slot 1) -> evita el timeout 0x107 por conflicto de bus con el WiFi.
 * Reutiliza los MISMOS pines fisicos del slot 0 (la tarjeta habla SPI sobre
 * ellos): CLK->SCK(43) CMD->MOSI(44) D0->MISO(39) D3->CS(42). */
#define SD_SPI_HOST   SPI3_HOST
#define SD_PIN_SCK    43   /* CLK */
#define SD_PIN_MOSI   44   /* CMD */
#define SD_PIN_MISO   39   /* D0  */
#define SD_PIN_CS     42   /* D3  */

static datalogger_entry_t s_buf[DATALOGGER_MAX_ENTRIES];
static int                s_head  = 0;
static int                s_count = 0;
static SemaphoreHandle_t  s_mutex = NULL;

/* Indice del primer entry pendiente de volcar a SD (en el orden de RAM) */
static int                s_pending_first = 0;
/* Numero de entries pendientes (escritas al buffer pero no a SD aun) */
static int                s_pending_count = 0;

static sdmmc_card_t *s_card = NULL;
static esp_ldo_channel_handle_t s_sd_ldo = NULL;
static bool          s_sd_mounted = false;
static esp_timer_handle_t s_flush_timer = NULL;
/* Serializa los flushes (timer + main + shutdown) para que dos fprintf
 * concurrentes al mismo CSV no interleaven bytes. */
static SemaphoreHandle_t s_flush_mutex = NULL;

static void get_timestamp(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    if (t.tm_year > 100) {
        snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        uint64_t ms = esp_timer_get_time() / 1000;
        uint32_t s  = (uint32_t)(ms / 1000);
        uint32_t h  = s / 3600; s %= 3600;
        uint32_t m  = s / 60;   s %= 60;
        snprintf(buf, len, "BOOT+%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

/* El fichero destino sale del timestamp YA guardado en la muestra, no de la
 * hora del volcado: si el volcado cae justo despues de medianoche, las muestras
 * de ayer tienen que acabar en el fichero de ayer. */
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

/* Corte de corriente REAL a la microSD, sin tocar el aparato.
 *
 * La tarjeta la alimenta el regulador interno del chip (canal 4 = TF_VCC), y ese
 * regulador NO se apaga en un reinicio por software: la tarjeta conserva su
 * estado y, si se habia quedado colgada, sigue colgada tras el reinicio (da
 * `sdmmc_send_cmd 0x108` en todas las lecturas). Hasta ahora la unica salida era
 * desenchufar la pantalla.
 *
 * Soltar el canal lo apaga de verdad, asi que con soltar - esperar - volver a
 * pedir se consigue el mismo efecto que desenchufar, pero por software. */
static void sd_power_cycle(const char *motivo)
{
    if (s_sd_ldo) {
        esp_ldo_release_channel(s_sd_ldo);
        s_sd_ldo = NULL;
    } else {
        /* Arranque: el canal puede venir encendido de la sesion anterior (el
         * reinicio no lo apaga). Se pide y se suelta para forzar el apagado. */
        esp_ldo_channel_config_t tmp = { .chan_id = 4, .voltage_mv = 3300 };
        esp_ldo_channel_handle_t h = NULL;
        if (esp_ldo_acquire_channel(&tmp, &h) == ESP_OK) {
            esp_ldo_release_channel(h);
        }
    }
    /* 300 ms para que el condensador del rail se descargue de verdad: con menos
     * la tarjeta no llega a perder su estado y no sirve de nada. */
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    esp_err_t ldo_err = esp_ldo_acquire_channel(&ldo_cfg, &s_sd_ldo);
    if (ldo_err != ESP_OK) {
        ESP_LOGW(TAG, "ldo_acquire ch4 failed: %s", esp_err_to_name(ldo_err));
        s_sd_ldo = NULL;
        return;
    }
    ESP_LOGI(TAG, "TF_VCC: corte de corriente a la SD (%s) -> 3300 mV ON", motivo);
    /* 100 ms para que el rail 3V3 estabilice ANTES de montar. 10 ms era muy poco:
     * en arranques marginales la tarjeta aun no respondia. */
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t mount_sd(void)
{
    /* SIEMPRE se arranca dando un corte de corriente a la tarjeta: si venimos de
     * un reinicio por software (o de un flasheo), es la unica forma de sacarla
     * de un estado colgado. */
    sd_power_cycle("arranque");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        /* 8: el datalogger/battery_history/logs ya tenian abiertos los 4 handles
         * -> las fotos de vigilancia (y el snapshot del boot) no podian abrir un
         * 5o fichero y fallaban ("no pude guardar"). Con 8 hay margen. */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    /* SD en SPI necesita pull-up en MISO y CS (el board no lleva fuertes; el
     * modo SDMMC usaba INTERNAL_PULLUP). */
    gpio_set_pull_mode(SD_PIN_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_CS,   GPIO_PULLUP_ONLY);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 8192,   /* >= trozo de camara (8KB) y menos transacciones SPI */
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  /* INVALID_STATE = ya init */
        ESP_LOGW(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = SD_PIN_CS;
    dev_cfg.host_id = SD_SPI_HOST;

    /* 3 intentos cortos (sin bloquear el boot > WDT). Ya no comparte bus con el
     * C6 (WiFi); la contencion con la camara se mitiga con camera_sd_bus_lock. */
    err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_cfg, &mount_config, &s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD(SPI) mount intento %d/3: %s", i + 1, esp_err_to_name(err));
            /* Entre intentos, otro corte de corriente: si la tarjeta esta
             * colgada, esperar mas no sirve de nada; hay que apagarla. */
            if (i < 2) sd_power_cycle("reintento");
            else       vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD montada OK");
    sdmmc_card_print_info(stdout, s_card);
    /* Crear directorio frigo si no existe */
    struct stat st;
    if (stat(LOG_DIR, &st) != 0) {
        mkdir(LOG_DIR, 0775);
        ESP_LOGI(TAG, "Creado directorio %s", LOG_DIR);
    }
    return ESP_OK;
}

static void format_temp(char *buf, size_t len, float t)
{
    if (t < -120.0f) snprintf(buf, len, "---");
    else snprintf(buf, len, "%.1f", t);
}

/* Cierre limpio para poder SACAR la tarjeta sin corromperla.
 *
 * Desmontar de verdad importa: FAT deja metadatos en cache y, si se saca la
 * tarjeta con ficheros a medio cerrar, se pierde lo ultimo escrito o se corrompe
 * el directorio. Despues de esto NO se vuelve a escribir hasta reiniciar.
 *
 * El corte de corriente a la tarjeta (ver sd_power_cycle) tambien la deja en
 * estado limpio para el siguiente arranque. */
esp_err_t datalogger_close_sd(void)
{
    datalogger_flush();          /* lo que quede en RAM, al fichero */

    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Tomar el cerrojo del bus: la camara puede estar escribiendo una foto de
     * vigilancia justo ahora y desmontar por debajo la dejaria a medias. */
    /* Si el cerrojo caduca NO se desmonta: hacerlo igual es justo lo que el
     * comentario de arriba dice que hay que evitar (dejar a medias la foto que
     * la camara esta escribiendo). Se avisa y que lo reintente. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) {
        ESP_LOGW(TAG, "la camara esta usando la tarjeta: no se desmonta, reintenta");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        ESP_LOGI(TAG, "tarjeta desmontada: ya se puede sacar");
    } else {
        ESP_LOGW(TAG, "no se pudo desmontar: %s", esp_err_to_name(err));
    }
    camera_sd_bus_unlock();
    return err;
}

bool datalogger_sd_montada(void)
{
    return s_card != NULL;
}

void datalogger_flush(void)
{
    flush_pending_to_sd_impl();
}

static void flush_pending_to_sd_impl(void)
{
    if (!s_sd_mounted || !s_mutex) return;
    if (s_pending_count <= 0) return;

    /* Serializa flushes concurrentes (timer + main thread). */
    if (s_flush_mutex && xSemaphoreTake(s_flush_mutex, 0) != pdTRUE) {
        return;
    }

    /* Dia destino = el de la PRIMERA muestra pendiente. Un volcado escribe solo
     * las muestras de ese dia; si el bloque cruza medianoche, las del dia nuevo
     * quedan pendientes y salen en el volcado de 60 s despues, ya a su fichero.
     * Se copia bajo mutex porque datalogger_log puede mover s_pending_first al
     * saturarse el ring. */
    char first_ts[sizeof s_buf[0].timestamp];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }
    snprintf(first_ts, sizeof first_ts, "%s", s_buf[s_pending_first].timestamp);
    xSemaphoreGive(s_mutex);

    char path[64];
    get_day_filename(first_ts, path, sizeof path);

    /* fopen ANTES de tocar el estado pendiente: si falla preservamos las
     * entradas para el proximo intento. */
    struct stat st;

    /* Cerrojo de bus camara<->SD: NO escribir mientras el GDMA de la camara esta
     * activo (contencion SDMMC -> INT WDT -> reinicio). Timeout corto: este
     * callback corre en la tarea esp_timer compartida, un lock largo aqui
     * retrasaria TODOS los demas timers del firmware. Si no se consigue el
     * bus, omitir este flush; los datos quedan en el ring para el siguiente.
     * El stat() de need_header TAMBIEN toca la SD: tiene que ir DESPUES del
     * cerrojo, si no la contencion SDMMC salta igual. */
    if (!camera_sd_bus_lock(200)) {
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }
    bool need_header = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s failed", path);
        camera_sd_bus_unlock();
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }

    /* Mantenemos s_mutex durante toda la escritura. La alternativa "2 fases
     * con snapshot" no se puede aplicar aqui porque s_pending_count nunca
     * decrementa (datalogger_log lo satura en MAX) y por tanto un overflow
     * del ring durante un fprintf prolongado provoca perdida silenciosa.
     * datalogger_log se llama cada ~5 min (frigo update); bloquearlo unos
     * cientos de ms en el flush es ~0.1% del tiempo. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        fclose(f);
        camera_sd_bus_unlock();
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }

    bool io_error = false;
    if (need_header) {
        if (fprintf(f, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct,excedente_solar\n") < 0) {
            io_error = true;
        }
    }
    int snapshot_count = s_pending_count;
    int written = 0;
    for (int i = 0; i < snapshot_count && !io_error; ++i) {
        int idx = (s_pending_first + i) % DATALOGGER_MAX_ENTRIES;
        const datalogger_entry_t *e = &s_buf[idx];
        /* Cambio de dia dentro del bloque pendiente: parar aqui. Lo escrito se
         * consolida y el resto lo recoge el proximo volcado en su fichero. */
        if (!mismo_dia(first_ts, e->timestamp)) break;
        char ta[10], tc[10], te[10];
        format_temp(ta, sizeof ta, e->T_Aletas);
        format_temp(tc, sizeof tc, e->T_Congelador);
        format_temp(te, sizeof te, e->T_Exterior);
        int r = fprintf(f, "%s,%s,%s,%s,%d,%d\n",
                        e->timestamp, ta, tc, te, e->fan_percent, e->excedente_solar ? 1 : 0);
        if (r < 0 || ferror(f)) { io_error = true; break; }
        written++;
    }

    /* Solo avanzamos el cursor si la escritura fue limpia. */
    if (!io_error) {
        s_pending_first = (s_pending_first + written) % DATALOGGER_MAX_ENTRIES;
        s_pending_count -= written;
    }
    xSemaphoreGive(s_mutex);
    fclose(f);
    camera_sd_bus_unlock();

    if (io_error) {
        ESP_LOGW(TAG, "I/O error en %s tras %d/%d entradas; reintento proximo flush",
                 path, written, snapshot_count);
    } else if (written > 0) {
        ESP_LOGI(TAG, "Volcadas %d entradas a %s", written, path);
    }
    if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
}

static void flush_timer_cb(void *arg)
{
    flush_pending_to_sd_impl();
}

static void start_flush_timer(void)
{
    const esp_timer_create_args_t args = { .callback = flush_timer_cb, .name = "dl_flush" };
    if (esp_timer_create(&args, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, (uint64_t)FLUSH_INTERVAL_MS * 1000ULL);
        ESP_LOGI(TAG, "Flush timer iniciado (%d ms)", FLUSH_INTERVAL_MS);
    }
}

esp_err_t datalogger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_flush_mutex = xSemaphoreCreateMutex();
    if (!s_flush_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_head  = 0;
    s_count = 0;
    s_pending_first = 0;
    s_pending_count = 0;

    /* Intentar montar SD (3 intentos cortos). NOTA: reintentar agresivamente
     * (incluido en segundo plano) hammerea el bus SDMMC compartido con el C6 y
     * lo desestabiliza (reboots) -> no hacerlo. El montaje es intermitente; si
     * falla este arranque, montara en el siguiente. */
    if (mount_sd() == ESP_OK) {
        s_sd_mounted = true;
        start_flush_timer();
    }
    /* NO reintentar (ni diferido ni en background): cualquier acceso a la SD
     * mientras el C6 usa el bus SDMMC compartido lo desestabiliza. Si la SD no
     * monta este arranque, montara en el siguiente. */

    ESP_LOGI(TAG, "Datalogger iniciado (RAM %d entradas, SD %s)",
             DATALOGGER_MAX_ENTRIES, s_sd_mounted ? "OK" : "no disponible");
    return ESP_OK;
}

esp_err_t datalogger_log(const frigo_state_t *frigo)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    datalogger_entry_t entry;
    get_timestamp(entry.timestamp, sizeof(entry.timestamp));
    entry.T_Aletas     = frigo->T_Aletas;
    entry.T_Congelador = frigo->T_Congelador;
    entry.T_Exterior   = frigo->T_Exterior;
    entry.fan_percent  = frigo->fan_percent;
    entry.excedente_solar = frigo_solar_get_active();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Log descartado: timeout tomando mutex");
        return ESP_ERR_TIMEOUT;
    }
    s_buf[s_head] = entry;
    s_head = (s_head + 1) % DATALOGGER_MAX_ENTRIES;
    if (s_count < DATALOGGER_MAX_ENTRIES) s_count++;
    if (s_pending_count < DATALOGGER_MAX_ENTRIES) {
        s_pending_count++;
    } else {
        /* buffer pendientes lleno: descartar mas antiguo */
        s_pending_first = (s_pending_first + 1) % DATALOGGER_MAX_ENTRIES;
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Log[%d]: %s | %.1f | %.1f | %.1f | fan=%d%%",
             s_count, entry.timestamp,
             entry.T_Aletas, entry.T_Congelador,
             entry.T_Exterior, entry.fan_percent);
    return ESP_OK;
}

int datalogger_get_count(void)
{
    if (!s_mutex) return 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    int c = s_count;
    xSemaphoreGive(s_mutex);
    return c;
}

/* Copia el entry `index` a *out. El caller DEBE tener s_mutex tomado. Devuelve
 * false si el indice esta fuera de rango. */
static bool get_entry_locked(int index, datalogger_entry_t *out)
{
    if (index < 0 || index >= s_count) return false;
    int real = (s_count < DATALOGGER_MAX_ENTRIES) ? index : (s_head + index) % DATALOGGER_MAX_ENTRIES;
    *out = s_buf[real];
    return true;
}

const datalogger_entry_t *datalogger_get_entry(int index)
{
    /* Copia bajo lock: leer s_buf/s_head/s_count sin lock mientras
     * datalogger_log muta bajo s_mutex da una lectura rota. Buffer estatico:
     * todos los callers publicos corren en el hilo de LVGL y leen la struct
     * antes de la siguiente llamada. */
    static datalogger_entry_t copy;
    if (!s_mutex) return NULL;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return NULL;
    bool ok = get_entry_locked(index, &copy);
    xSemaphoreGive(s_mutex);
    return ok ? &copy : NULL;
}

char *datalogger_get_csv(void)
{
    if (!s_mutex || s_count == 0) return NULL;
    size_t size = 80 + (size_t)s_count * 80;
    char *csv = malloc(size);
    if (!csv) return NULL;
    int pos = 0;
    pos += snprintf(csv + pos, size - pos,
                    "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct,excedente_solar\n");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < s_count && pos < (int)size - 80; i++) {
            datalogger_entry_t e;  /* get_entry_locked: ya tenemos s_mutex */
            if (!get_entry_locked(i, &e)) continue;
            char ta[10], tc[10], te[10];
            format_temp(ta, sizeof ta, e.T_Aletas);
            format_temp(tc, sizeof tc, e.T_Congelador);
            format_temp(te, sizeof te, e.T_Exterior);
            pos += snprintf(csv + pos, size - pos,
                            "%s,%s,%s,%s,%d,%d\n",
                            e.timestamp, ta, tc, te, e.fan_percent, e.excedente_solar ? 1 : 0);
        }
        xSemaphoreGive(s_mutex);
    }
    return csv;
}
