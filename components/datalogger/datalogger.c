#include "datalogger.h"
#include "camera.h"          /* camera_sd_bus_lock/unlock: evitar contencion SD<->camara */
#include "sd_safe.h"         /* stat/fopen/mkdir sueltos con el cerrojo incluido */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
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

/* --- SD por SDMMC: slot 0, bus de 4 bits, 40 MHz ---
 *
 * Historia: entre el 6-jul y hoy la tarjeta iba por SPI3 a 20 MHz porque el modo
 * SDMMC daba timeouts 0x107 al montar. Medido por SPI: 0.13 MB/s de escritura,
 * que hace inviable guardar video. Se vuelve a SDMMC con las dos cosas que
 * entonces no estaban resueltas:
 *
 * 1) POR QUE 40 MHz Y NO 20 (SDMMC_FREQ_DEFAULT, lo que ponia el codigo viejo).
 *    `sdmmc_host_set_card_clk()` programa el divisor de reloj GLOBAL del
 *    periferico (`sdmmc_host_set_clk_div()`: no hay divisor por slot, lo
 *    comparten los dos). El P4 tiene el enlace SDIO con el C6 (la radio) en el
 *    OTRO slot, el 1, y esp_hosted lo tiene a 40 MHz
 *    (CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000 -> divisor 4). Pidiendo 40 MHz
 *    (SDMMC_FREQ_HIGHSPEED) el divisor de la tarjeta tambien es 4 y el reloj del
 *    C6 NO se mueve al montar. Pidiendo 20 MHz el divisor pasa a 8 y el enlace
 *    del C6 se queda a la mitad mientras la tarjeta este montada. O sea: 40 MHz
 *    no es solo el doble de rapido, es la frecuencia que no pelea con la radio.
 *    El unico momento en que el divisor baja es la identificacion de la tarjeta,
 *    a 400 kHz (divisor 10, unos milisegundos). Por eso este montaje se hace en
 *    init_sd_rtc_frigo(), ANTES de init_network(): cuando el C6 todavia no habla.
 *
 * 2) EL 0x107 DE JULIO ERA LA TARJETA COLGADA, NO EL C6. El corte de corriente
 *    de verdad (`sd_power_cycle`, 300 ms) es del 25-jul; el paso a SPI fue del
 *    6-jul. En la era SDMMC el codigo solo pedia el LDO si no lo tenia y NUNCA
 *    apagaba la tarjeta, asi que un arranque en mal estado la dejaba colgada
 *    para todos los siguientes (exactamente el sintoma que se veia). Eso ya esta
 *    resuelto, y es independiente del transporte.
 *
 * Pines: los del IOMUX del slot 0 (CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42),
 * los seis cableados al conector TF en el esquematico JC1060P470C (J1: DATA0..3,
 * CLK, CMD; VDD desde ESP_LDO_VO4 = canal 4). Dejando los pines SIN definir el
 * driver usa IOMUX; comprobado en el codigo de IDF 5.5.5 que asi no exige
 * configurarlos a mano ni se queja del ancho. */

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

/* El C6 (radio) en reset MIENTRAS se identifica la tarjeta.
 *
 * El P4 tiene UN solo controlador SDMMC y lo comparten la tarjeta (slot 0) y el
 * enlace SDIO con el C6 (slot 1). Y el C6 puede seguir VIVO de la sesion
 * anterior: un reinicio o un flasheo de la P4 no lo resetea (lo resetea
 * esp_hosted al conectar con el, ~8 s despues de este montaje). Si en ese rato
 * el C6 esta pidiendo atencion por SDIO, la identificacion de la tarjeta se
 * pierde.
 *
 * MEDIDO el 2-oct-2026: arrancando justo despues de flashear, con el C6 en modo
 * streaming de la sesion anterior, 3 intentos de 3 fallaron con ESP_ERR_TIMEOUT
 * en ACMD41 (la tarjeta contesta CMD0/CMD8 y se queda muda). Con el C6 en reset
 * durante la identificacion: monta a la primera, y 6 + 17 arranques seguidos OK.
 *
 * El camino exacto por el que el C6 molesta NO esta instrumentado: las
 * transacciones si se serializan (`s_request_mutex`, sdmmc_transaction.c), pero
 * el ISR es unico y sus eventos no llevan slot (sdmmc_host.c), y esp_hosted
 * espera las interrupciones de IO por ese mismo ISR. Lo que esta medido es que
 * con el C6 quieto el montaje sale siempre y con el C6 hablando fallaba.
 *
 * C6_CHIP_PU = GPIO54 (CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE), activo a nivel
 * bajo: 0 = en reset. esp_hosted lo suelta el solo cuando conecta (hace su propio
 * pulso de reset), asi que despues se deja como estaba. No se toca nada mas de
 * la radio. */
#define C6_CHIP_PU_GPIO 54

static void c6_en_reset(bool en_reset)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << C6_CHIP_PU_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(C6_CHIP_PU_GPIO, en_reset ? 0 : 1);
    ESP_LOGW(TAG, "C6 %s durante la identificacion de la SD", en_reset ? "en reset" : "suelto");
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
    /* SD en SDMMC: los pull-ups internos del P4 hacen falta porque el board no
     * lleva pull-ups externos fuertes en las lineas de datos. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40 MHz = mismo divisor que el C6 */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* 3 intentos cortos (sin bloquear el boot > WDT). Los dos primeros con el bus
     * completo de 4 bits; el tercero en 1 bit: si una de las lineas D1..D3 esta
     * marginal, en 4 bits falla siempre y en 1 bit si funciona. El reloj es el
     * mismo en los dos casos (40 MHz), asi que el C6 no se entera. Antes lento
     * que sin tarjeta. */
    esp_err_t err = ESP_FAIL;
    /* El C6 se queda en reset hasta que la tarjeta este identificada (ver el
     * comentario de c6_en_reset). Va AQUI, no antes del corte de corriente: el
     * corte de 300 ms se lleva la sesion del C6 por delante igual, y asi el pin
     * solo se toca lo justo. */
    c6_en_reset(true);
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        slot_config.width = (i < 2) ? 4 : 1;
        err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD mount intento %d/3 (%d bits, 40 MHz): %s",
                     i + 1, slot_config.width, esp_err_to_name(err));
            /* Entre intentos, otro corte de corriente: si la tarjeta esta
             * colgada, esperar mas no sirve de nada; hay que apagarla. */
            if (i < 2) sd_power_cycle("reintento");
            else       vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    c6_en_reset(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD montada OK (%d bits, %d kHz reales)", slot_config.width,
             s_card ? s_card->max_freq_khz : host.max_freq_khz);
    sdmmc_card_print_info(stdout, s_card);
    /* Crear directorio frigo si no existe */
    struct stat st;
    if (!sd_stat(LOG_DIR, &st, 3000)) {
        sd_mkdir(LOG_DIR, 0775, 3000);
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
        /* s_sd_mounted es una bandera APARTE de s_card, y flush_pending_to_sd_impl()
         * mira esta, no s_card. Sin limpiarla aqui, un desmontaje "seguro" de
         * verdad dejaba flush_pending_to_sd_impl() creyendo que la tarjeta
         * seguia montada -- reintentaria fopen() sobre un punto de montaje ya
         * desregistrado cada 60 s para siempre, en vez de callarse. Detectado
         * auditando el 08-sep-2026. */
        s_sd_mounted = false;
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

/* El dia que se instala una version con la columna nueva (min_solar_hoy), el
 * fichero de ESE dia ya lo creo la version anterior: se le anadiran filas de 7
 * valores bajo una cabecera de 6 nombres. Los lectores por indice del firmware
 * lo aguantan, pero los que van por nombre (la app del movil y el analizador
 * del PC) se lian. Se arregla una vez: si la primera linea no es la cabecera
 * actual, se reescribe el fichero con la cabecera nueva y las filas viejas con
 * un campo vacio al final (que los lectores ya interpretan como "sin dato").
 * Se llama con el cerrojo de la SD ya tomado y el fichero abierto aparte. */
static void arreglar_cabecera_si_hace_falta(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char linea[160];
    if (!fgets(linea, sizeof(linea), f)) { fclose(f); return; }
    if (strcmp(linea, DATALOGGER_CSV_HEADER) == 0) { fclose(f); return; }

    char tmp[80];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) { fclose(f); return; }
    FILE *o = fopen(tmp, "w");
    if (!o) { fclose(f); return; }

    fputs(DATALOGGER_CSV_HEADER, o);
    int campos = 1;                       /* comas + 1 */
    for (const char *p = linea; *p; ++p) if (*p == ',') campos++;
    size_t n = strlen(linea);
    while (n > 0 && (linea[n-1] == '\n' || linea[n-1] == '\r')) linea[--n] = 0;
    if (campos >= 7) fprintf(o, "%s\n", linea);
    else             fprintf(o, "%s,\n", linea);   /* el campo que le falta, vacio */
    while (fgets(linea, sizeof(linea), f)) fputs(linea, o);

    fclose(o);
    fclose(f);
    /* Cambio de fichero con RESPALDO (23-sep-2026). Antes era remove(path) +
     * rename(tmp, path): si el rename fallaba despues del remove (FAT llena,
     * tarjeta arrancada a mitad...), el CSV del dia ya estaba borrado y solo
     * quedaba el .tmp con otro nombre. Ahora se aparta el original, se pone el
     * nuevo y solo entonces se borra el respaldo; si el segundo paso falla, se
     * devuelve el original a su sitio y no se pierde nada. */
    char bak[84];
    bool hay_bak = snprintf(bak, sizeof(bak), "%s.bak", path) < (int)sizeof(bak);
    if (hay_bak) remove(bak);                       /* resto de un intento anterior */
    if (hay_bak && rename(path, bak) != 0) {        /* 1) apartar el original */
        ESP_LOGW(TAG, "no he podido apartar %s; se queda como estaba", path);
        remove(tmp);
    } else if (rename(tmp, path) != 0) {            /* 2) poner el nuevo */
        ESP_LOGW(TAG, "no he podido poner el CSV nuevo en %s", path);
        if (hay_bak) rename(bak, path);             /* 3) devolver el original */
        remove(tmp);
    } else {
        if (hay_bak) remove(bak);                   /* 4) ya sobra el respaldo */
        ESP_LOGI(TAG, "cabecera del CSV actualizada (formato viejo): %s", path);
    }
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
     * activo (con la camara capturando, escribir en la SD a la vez daba INT WDT y
     * reinicios). Timeout corto para no
     * acaparar el bus aunque ya no corre en la tarea esp_timer compartida
     * (tiene su propia tarea, ver flush_task) -- la camara y el resto de
     * escritores de SD siguen esperando el mismo cerrojo. Si no se consigue el
     * bus, omitir este flush; los datos quedan en el ring para el siguiente.
     * El stat() de need_header TAMBIEN toca la SD: tiene que ir DESPUES del
     * cerrojo, si no la contencion salta igual.
     * TODO(2-oct-2026): aquello se midio con la SD en SPI; ahora la SD ha vuelto
     * a SDMMC (4 bits, 40 MHz), asi que hay que volver a medir si el cerrojo
     * sigue haciendo falta. Se queda puesto: no cuesta nada y sin medir no se
     * quita. */
    if (!camera_sd_bus_lock(200)) {
        if (s_flush_mutex) xSemaphoreGive(s_flush_mutex);
        return;
    }
    bool need_header = (stat(path, &st) != 0);
    if (!need_header) arreglar_cabecera_si_hace_falta(path);
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
        if (fprintf(f, DATALOGGER_CSV_HEADER) < 0) {
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
        int r = fprintf(f, DATALOGGER_CSV_ROW,
                        e->timestamp, ta, tc, te, e->fan_percent,
                        e->excedente_solar ? 1 : 0, (unsigned)e->min_solar_hoy);
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

/* La tarea de esp_timer es COMPARTIDA por todo el firmware (heap log, GPS,
 * RTC, modo noche, feed solar del frigo...). fopen/fprintf/fclose no tienen
 * timeout propio: si la SD se queda colgada a nivel hardware (no solo
 * contencion con la camara, que si tiene su timeout de 200ms arriba), esa
 * llamada bloquea sin limite y con ella TODOS esos timers, no solo este
 * flush. Por eso el timer periodico solo NOTIFICA a esta tarea dedicada, que
 * es la unica que de verdad toca la SD -- si se atasca, se atasca sola.
 * datalogger_flush() (llamada directa, p.ej. al desmontar) sigue siendo
 * sincrona a proposito: quien la llama necesita saber que ya termino antes
 * de seguir. Detectado auditando el 07-sep-2026. */
static TaskHandle_t s_flush_task_handle = NULL;
static datalogger_heartbeat_cb_t s_hb_cb = NULL;   /* ver datalogger_set_heartbeat_cb */

void datalogger_set_heartbeat_cb(datalogger_heartbeat_cb_t cb) { s_hb_cb = cb; }

static void flush_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_hb_cb) s_hb_cb();   /* latido watchdog: ver datalogger_set_heartbeat_cb */
        flush_pending_to_sd_impl();
    }
}

static void flush_timer_cb(void *arg)
{
    if (s_flush_task_handle) xTaskNotifyGive(s_flush_task_handle);
}

static void start_flush_timer(void)
{
    /* 3072 causo un bootloop real en bh_flush_task (mismo patron, misma SD)
     * el 08-sep-2026: fopen/fprintf/fclose es de lo mas hambriento de pila de
     * ESP-IDF. Formateo de float ligero aqui (solo %.1f) -> se penso que 4096
     * bastaba. SUBIDA A 6144 el 21-sep-2026: el stackwatch midio 1152 bytes
     * libres (72% usado); con eso, un camino de error desborda. */
    if (xTaskCreate(flush_task, "dl_flush_task", 6144, NULL,
                     tskIDLE_PRIORITY + 2, &s_flush_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de flush: sin volcado periodico a SD");
        return;
    }
    const esp_timer_create_args_t args = { .callback = flush_timer_cb, .name = "dl_flush" };
    if (esp_timer_create(&args, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, (uint64_t)FLUSH_INTERVAL_MS * 1000ULL);
        ESP_LOGI(TAG, "Flush timer iniciado (%d ms)", FLUSH_INTERVAL_MS);
    }
}

TaskHandle_t datalogger_flush_task_handle(void)
{
    return s_flush_task_handle;
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

    /* Intentar montar SD (3 intentos cortos). El montaje se hace UNA vez por
     * arranque, en init_sd_rtc_frigo(), antes de que arranque la radio (ver la
     * explicacion del reloj compartido en la cabecera del fichero). */
    if (mount_sd() == ESP_OK) {
        s_sd_mounted = true;
        start_flush_timer();
    }
    /* NO reintentar (ni diferido ni en background): volver a montar en caliente
     * obliga a identificar la tarjeta otra vez a 400 kHz, y en ese momento el
     * divisor global del SDMMC baja a 10 -> el enlace SDIO del C6 (la radio, que
     * ya esta hablando) se queda a 16 MHz unos milisegundos por debajo de sus
     * transacciones. Si la SD no monta este arranque, montara en el siguiente. */

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
    /* Acotado antes del cast: con el RTC sin hora el acumulado no se reinicia
     * nunca y a los 45 dias daria la vuelta (65535 min). */
    uint32_t min_sol = frigo_solar_get_seg_hoy() / 60u;
    entry.min_solar_hoy   = (uint16_t)(min_sol > 65535u ? 65535u : min_sol);
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
    pos += snprintf(csv + pos, size - pos, DATALOGGER_CSV_HEADER);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (int i = 0; i < s_count && pos < (int)size - 80; i++) {
            datalogger_entry_t e;  /* get_entry_locked: ya tenemos s_mutex */
            if (!get_entry_locked(i, &e)) continue;
            char ta[10], tc[10], te[10];
            format_temp(ta, sizeof ta, e.T_Aletas);
            format_temp(tc, sizeof tc, e.T_Congelador);
            format_temp(te, sizeof te, e.T_Exterior);
            pos += snprintf(csv + pos, size - pos, DATALOGGER_CSV_ROW,
                            e.timestamp, ta, tc, te, e.fan_percent,
                            e.excedente_solar ? 1 : 0, (unsigned)e.min_solar_hoy);
        }
        xSemaphoreGive(s_mutex);
    }
    return csv;
}
