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

static volatile bool s_en_curso = false;

bool slave_ota_en_curso(void) { return s_en_curso; }

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

    if (camera_sd_bus_lock()) {
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

static void slave_ota_task(void *arg)
{
    (void)arg;

    /* Esperar a que la SD este montada (el datalogger la monta al arrancar). */
    int esperado = 0;
    while (!datalogger_sd_montada() && esperado < ESPERA_SD_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esperado += 500;
    }
    if (!datalogger_sd_montada()) {
        ESP_LOGE(TAG, "la SD no ha montado en %d s: no hay de donde leer la imagen",
                 ESPERA_SD_MS / 1000);
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    size_t total = 0;
    uint8_t *imagen = leer_imagen(&total);
    if (!imagen) {
        ESP_LOGE(TAG, "==== NO empiezo: no tengo la imagen en %s ====", RUTA_IMAGEN);
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==== Grabando el firmware del C6: %u bytes ====", (unsigned)total);

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
    if (!slave_ota_hay_fichero()) {
        ESP_LOGW(TAG, "no hay imagen en %s: no grabo nada. Copiala a la SD y "
                      "reinicia, o pulsa el boton de Ajustes -> Wi-Fi.", RUTA_IMAGEN);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "imagen encontrada en la SD: empiezo sola (pasados %d s)", segundos);
    slave_ota_start();
    vTaskDelete(NULL);
}

void slave_ota_start_diferido(int segundos)
{
    xTaskCreate(slave_ota_diferido_task, "slave_ota_dif", 3072,
                (void *)(intptr_t)segundos, 3, NULL);
}
