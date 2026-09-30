/* slave_ota.c - Graba el firmware de la radio (C6) desde /sdcard/network_adapter.bin
 *
 * HERRAMIENTA DE MANTENIMIENTO, no va en el firmware de publicacion. Ver LEEME.md.
 *
 * POR QUE EXISTE
 * El C6 (la radio Wi-Fi/BLE del P4) lleva su propio firmware, y a partir de
 * esp_hosted 2.12.13 el host y el esclavo van EMPARREADOS: si el C6 se queda
 * atras, el AP funciona pero el BLE no arranca (medido el 23-sep-2026). Para
 * actualizar una placa que aun tiene el C6 viejo hay que grabarle el firmware
 * ANTES de ponerle el firmware nuevo de la P4.
 *
 * COMO
 * Este modulo usa las primitivas de OTA de esp_hosted 0.0.27 (las del firmware
 * viejo, que es el unico que puede hablar con un C6 viejo lo bastante como para
 * grabarle nada). La imagen se lee de la SD, se pasa a PSRAM y se empuja por el
 * mismo bus SDIO. La escritura va a la particion INACTIVA del C6: un corte a
 * medias deja el firmware actual intacto.
 *
 * LO QUE NO SE PUEDE DESHACER
 * Un firmware que arranque pero no hable dejaria el C6 sin poder grabarse desde
 * la P4 (habria que llegar a su UART, que va soldado). Por eso se comprueba el
 * fichero antes de empezar y no se lanza nada si no esta.
 */
#include "slave_ota.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera.h"      /* camera_sd_bus_lock/unlock: cerrojo del bus SD */
#include "datalogger.h"  /* datalogger_sd_montada() */
#include "sd_safe.h"     /* sd_stat() con cerrojo y timeout */

static const char *TAG = "slave_ota";

/* Las primitivas del componente esp_hosted 0.0.27. No estan en su cabecera
 * publica (esa solo ofrece rpc_ota(), que quiere una URL o un fichero), asi que
 * se declaran aqui. */
extern int rpc_ota_begin(void);
extern int rpc_ota_write(uint8_t *ota_data, uint32_t ota_data_len);
extern int rpc_ota_end(void);

#define RUTA_IMAGEN   "/sdcard/network_adapter.bin"
#define TAM_MINIMO    (900 * 1024)   /* por debajo de esto no es una imagen */
#define TAM_MAXIMO    (2 * 1024 * 1024)
#define CHUNK         1400           /* el mismo trozo que usa esp_hosted */
#define ESPERA_SD_MS  60000          /* cuanto esperar a que monte la SD */

/* ── Camino sin tarjeta SD ────────────────────────────────────────────────────
 * La imagen tambien se puede dejar en la particion de aplicacion INACTIVA
 * (ota_1), que en esta placa son 4 MB y no se usa mientras la placa arranca del
 * ota_0. Delante van 16 bytes de cabecera (magic, tamano y crc32) para no
 * grabar por error cualquier cosa que hubiera ahi: si no cuadra, no se toca el
 * C6. Se prepara y se graba con `aplicar.sh flash-sin-sd`. */
#define RADIO_MAGIC   "RADIO1"
#define RADIO_CAB     16

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint32_t tam;
    uint32_t crc;
} radio_cab_t;

static volatile bool s_en_curso = false;

bool slave_ota_en_curso(void) { return s_en_curso; }

/* Lee y valida la cabecera de la particion. Devuelve el tamano de la imagen o 0
 * si ahi no hay nada que se pueda grabar. */
static size_t particion_radio_tam(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!p) return 0;

    radio_cab_t cab;
    if (esp_partition_read(p, 0, &cab, sizeof cab) != ESP_OK) return 0;
    if (memcmp(cab.magic, RADIO_MAGIC, sizeof cab.magic) != 0) return 0;
    if (cab.tam < TAM_MINIMO || cab.tam > TAM_MAXIMO) return 0;
    if (cab.tam + RADIO_CAB > p->size) return 0;
    return (size_t)cab.tam;
}

static bool slave_ota_hay_particion(void) { return particion_radio_tam() > 0; }


bool slave_ota_hay_fichero(void)
{
    struct stat st;
    if (!datalogger_sd_montada()) return false;
    if (!sd_stat(RUTA_IMAGEN, &st, 2000)) return false;
    if (st.st_size < TAM_MINIMO || st.st_size > TAM_MAXIMO) {
        ESP_LOGE(TAG, "%s ocupa %ld bytes: no parece la imagen del C6 "
                      "(se esperan entre %d y %d)", RUTA_IMAGEN, (long)st.st_size,
                 TAM_MINIMO, TAM_MAXIMO);
        return false;
    }
    return true;
}

/* Lee la imagen entera a PSRAM. Se lee de golpe y se suelta la SD antes de
 * empezar a grabar: la grabacion tarda ~1 minuto y no tiene sentido tener el
 * bus de la SD bloqueado todo ese rato (lo comparten datalogger y camara). */
static uint8_t *leer_imagen(size_t *tam)
{
    struct stat st;
    uint8_t *buf = NULL;

    if (!sd_stat(RUTA_IMAGEN, &st, 2000)) {
        ESP_LOGE(TAG, "no puedo mirar %s", RUTA_IMAGEN);
        return NULL;
    }
    if (st.st_size < TAM_MINIMO || st.st_size > TAM_MAXIMO) {
        ESP_LOGE(TAG, "%s no parece la imagen del C6 (%ld bytes)",
                 RUTA_IMAGEN, (long)st.st_size);
        return NULL;
    }

    buf = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "sin memoria en PSRAM para %ld bytes", (long)st.st_size);
        return NULL;
    }

    /* El cerrojo del bus SD pide plazo (camera.h): 5 s de sobra para leer 1,2 MB. */
    if (camera_sd_bus_lock(5000)) {
        FILE *f = fopen(RUTA_IMAGEN, "rb");
        if (!f) {
            ESP_LOGE(TAG, "no puedo abrir %s", RUTA_IMAGEN);
            camera_sd_bus_unlock();
            free(buf);
            return NULL;
        }
        size_t leido = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        camera_sd_bus_unlock();
        if (leido != (size_t)st.st_size) {
            ESP_LOGE(TAG, "leidos %u de %ld bytes", (unsigned)leido, (long)st.st_size);
            free(buf);
            return NULL;
        }
    } else {
        ESP_LOGE(TAG, "no consigo el cerrojo de la SD (¿la camara esta usandola?)");
        free(buf);
        return NULL;
    }

    *tam = (size_t)st.st_size;
    return buf;
}

/* La misma imagen, pero leida de la particion ota_1 (sin SD de por medio). */
static uint8_t *leer_imagen_particion(size_t *tam)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    size_t n = particion_radio_tam();
    if (!p || n == 0) return NULL;

    radio_cab_t cab;
    esp_partition_read(p, 0, &cab, sizeof cab);

    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "sin memoria en PSRAM para %u bytes", (unsigned)n);
        return NULL;
    }
    if (esp_partition_read(p, RADIO_CAB, buf, n) != ESP_OK) {
        ESP_LOGE(TAG, "no puedo leer la imagen de la particion ota_1");
        free(buf);
        return NULL;
    }
    uint32_t crc = esp_rom_crc32_le(0, buf, (uint32_t)n);
    if (crc != cab.crc) {
        ESP_LOGE(TAG, "el crc de la imagen en ota_1 no cuadra (0x%08x != 0x%08x): "
                      "no grabo nada", (unsigned)crc, (unsigned)cab.crc);
        free(buf);
        return NULL;
    }
    ESP_LOGW(TAG, "imagen leida de la particion ota_1: %u bytes, crc 0x%08x",
             (unsigned)n, (unsigned)crc);
    *tam = n;
    return buf;
}

static void slave_ota_task(void *arg)
{
    (void)arg;

    size_t total = 0;
    uint8_t *imagen = NULL;
    const char *origen = "SD";

    if (slave_ota_hay_particion()) {
        /* Camino sin tarjeta: la imagen viene en la particion ota_1 y no hay
         * que esperar a que monte la SD. */
        imagen = leer_imagen_particion(&total);
    }

    if (imagen) {
        origen = "ota_1";
    }

    if (!imagen) {
        /* Esperar a que la SD este montada (el datalogger la monta al arrancar). */
        int esperado = 0;
        while (!datalogger_sd_montada() && esperado < ESPERA_SD_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esperado += 500;
        }
        if (!datalogger_sd_montada()) {
            ESP_LOGE(TAG, "ni imagen en la particion ota_1 ni SD montada: "
                          "no hay de donde leer");
            s_en_curso = false;
            vTaskDelete(NULL);
            return;
        }
        imagen = leer_imagen(&total);
    }

    if (!imagen) {
        ESP_LOGE(TAG, "==== NO empiezo: no tengo la imagen (ni en %s ni en la "
                      "particion ota_1) ====", RUTA_IMAGEN);
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==== Grabando el firmware del C6: %u bytes (desde %s) ====",
             (unsigned)total, origen);

    if (rpc_ota_begin() != 0) {
        ESP_LOGE(TAG, "el C6 no acepta empezar la actualizacion. Se deja como estaba.");
        free(imagen);
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    size_t enviado = 0;
    int ultimo_pct = -1;
    while (enviado < total) {
        size_t n = total - enviado;
        if (n > CHUNK) n = CHUNK;

        if (rpc_ota_write(imagen + enviado, (uint32_t)n) != 0) {
            ESP_LOGE(TAG, "fallo escribiendo en el byte %u de %u. Se aborta; el C6 "
                          "sigue con su firmware de antes (se escribia en la otra "
                          "particion).", (unsigned)enviado, (unsigned)total);
            rpc_ota_end();
            free(imagen);
            s_en_curso = false;
            vTaskDelete(NULL);
            return;
        }
        enviado += n;

        int pct = (int)((enviado * 100) / total);
        if (pct / 10 != ultimo_pct / 10) {
            ESP_LOGI(TAG, "  %d%% (%u/%u bytes)", pct, (unsigned)enviado, (unsigned)total);
            ultimo_pct = pct;
        }
        /* Un respiro entre trozos: el bus lo comparten la telemetria y el BLE. */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    free(imagen);

    if (rpc_ota_end() != 0) {
        ESP_LOGE(TAG, "el C6 rechaza la imagen al cerrarla (firma o tamano). "
                      "Sigue con la de antes.");
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==== C6 grabado. Reiniciando los dos en 5 s ====");
    ESP_LOGW(TAG, "==== Si has visto '10%%'..'100%%' sin errores, la radio quedo "
                  "actualizada. Ahora graba el firmware nuevo de la P4. ====");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

void slave_ota_start(void)
{
    if (s_en_curso) {
        ESP_LOGW(TAG, "ya hay una grabacion en marcha");
        return;
    }
    s_en_curso = true;
    /* Fuera del hilo de LVGL: esto tarda minutos y bloquearia la pantalla. */
    xTaskCreate(slave_ota_task, "slave_ota", 4096, NULL, 4, NULL);
}

static void slave_ota_diferido_task(void *arg)
{
    int segundos = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(segundos * 1000));

    if (slave_ota_en_curso()) {
        vTaskDelete(NULL);
        return;
    }
    if (!slave_ota_hay_fichero() && !slave_ota_hay_particion()) {
        ESP_LOGW(TAG, "no hay imagen ni en %s ni en la particion ota_1: no grabo "
                      "nada. Copiala a la SD, o graba la particion con "
                      "'aplicar.sh flash-sin-sd', y reinicia.", RUTA_IMAGEN);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "imagen encontrada: empiezo sola (pasados %d s)", segundos);
    slave_ota_start();
    vTaskDelete(NULL);
}

void slave_ota_start_diferido(int segundos)
{
    xTaskCreate(slave_ota_diferido_task, "slave_ota_dif", 3072,
                (void *)(intptr_t)segundos, 3, NULL);
}
