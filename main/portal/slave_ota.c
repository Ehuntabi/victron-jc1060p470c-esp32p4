/* slave_ota.c - Actualizar el firmware del C6 (la radio) desde el P4.
 *
 * POR QUE EXISTE ESTO (21-ago-2026):
 *
 * El AP del P4 no se llama como el P4 cree ni va cifrado. Se configura
 * "VictronConfig" + WPA2, esp_wifi_set_config devuelve ESP_OK, el esclavo
 * responde "softap started"... y en el aire aparece "ESP_<MAC>" y ABIERTO.
 * Verificado releyendo la config con esp_wifi_get_config y escaneando desde el
 * satelite. Es un problema conocido de esp_hosted y sigue abierto en su
 * repositorio (esp-hosted-mcu #37, desde marzo de 2025; mismo sintoma en
 * arduino-esp32 #9570).
 *
 * CAUSA, ya confirmada: el C6 llevaba el FIRMWARE DE FABRICA de la placa. El
 * codigo de los dos lados estaba bien -- el host serializa SSID, clave y
 * cifrado (rpc_req.c, case WIFI_IF_AP) y el esclavo los copia y llama a
 * esp_wifi_set_config (slave_control.c), y sus .proto son identicos -- pero el
 * chip no llevaba ESE firmware: ~/esp_hosted_slave es de mayo y no se habia
 * compilado nunca. Con otro protocolo, la config del AP le llegaba VACIA y
 * ESP-IDF levantaba su AP por defecto, que es justo "ESP_<MAC>" y abierto.
 *
 * ARREGLADO el 21-ago-2026 con este mismo boton. Despues:
 *   AP en la radio: ssid='VictronConfig' authmode=3   (WPA2)
 * y escaneando desde el satelite, 'VictronConfig' authmode=3. El ESP_DC078D
 * desaparecio.
 *
 * PENDIENTE: la P4 de REPUESTO (su AP salia como "ESP_F7C849") sigue con el
 * firmware de fabrica en su C6. Mismo procedimiento.
 *
 * COMO SE LE MANDA, sin red ni cables:
 * el binario del esclavo va EMPOTRADO en este firmware (EMBED_FILES en
 * main/CMakeLists.txt) y se empuja por el mismo bus SDIO que ya usan, con las
 * primitivas del propio esp_hosted. No hace falta ni servidor HTTP (el P4 solo
 * hace de AP, no se conecta a ninguna red) ni tocar la SD ni soldar en el C6.
 *
 * RIESGO: la escritura va a la particion INACTIVA del C6, asi que un corte a
 * medias deja el firmware actual intacto. Lo que no se puede deshacer desde
 * aqui es un firmware nuevo que arranque pero no hable: en ese caso haria
 * falta el UART del C6, que en placas hermanas no esta accesible sin soldar.
 * Por eso NO se lanza solo: hay que pedirlo desde Ajustes -> Wi-Fi.
 */
#include "slave_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "slave_ota";

/* Del componente esp_hosted. Son no-static pero solo rpc_ota() esta declarada
 * en su cabecera, y esa quiere una URL o un fichero: aqui el binario ya esta en
 * memoria, asi que se usan las tres primitivas directamente. */
extern int rpc_ota_begin(void);
extern int rpc_ota_write(uint8_t *ota_data, uint32_t ota_data_len);
extern int rpc_ota_end(void);

/* El binario empotrado (EMBED_FILES genera estos simbolos). Es OPCIONAL: no se
 * versiona, asi que un clon recien hecho o el CI compilan sin el. */
#if SLAVE_FW_EMBEBIDO
extern const uint8_t slave_bin_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t slave_bin_end[]   asm("_binary_network_adapter_bin_end");
#endif

/* El mismo tamano de trozo que usa el componente en su propio bucle de OTA. */
#define CHUNK 1400

static volatile bool s_en_curso = false;

bool slave_ota_en_curso(void) { return s_en_curso; }

#if SLAVE_FW_EMBEBIDO
static void slave_ota_task(void *arg)
{
    (void)arg;
    const uint8_t *p = slave_bin_start;
    const size_t total = (size_t)(slave_bin_end - slave_bin_start);

    ESP_LOGW(TAG, "==== Actualizando el firmware del C6: %u bytes ====",
             (unsigned)total);

    if (rpc_ota_begin() != 0) {
        ESP_LOGE(TAG, "El C6 no acepta empezar la actualizacion. Se deja como estaba.");
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    size_t enviado = 0;
    int ultimo_pct = -1;
    while (enviado < total) {
        size_t n = total - enviado;
        if (n > CHUNK) n = CHUNK;

        if (rpc_ota_write((uint8_t *)(p + enviado), (uint32_t)n) != 0) {
            ESP_LOGE(TAG, "Fallo escribiendo en el byte %u de %u. Se aborta; el C6 "
                          "sigue con su firmware de antes (se escribia en la otra "
                          "particion).", (unsigned)enviado, (unsigned)total);
            rpc_ota_end();
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

    if (rpc_ota_end() != 0) {
        ESP_LOGE(TAG, "El C6 rechaza la imagen al cerrarla (firma o tamano). "
                      "Sigue con la de antes.");
        s_en_curso = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==== C6 actualizado. Reiniciando los dos en 5 s ====");
    /* El propio componente reinicia el host tras un OTA del esclavo para no
     * quedarse desincronizados; aqui se hace lo mismo a mano. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}
#endif /* SLAVE_FW_EMBEBIDO */

void slave_ota_start(void)
{
#if !SLAVE_FW_EMBEBIDO
    ESP_LOGE(TAG, "Este firmware NO lleva el del C6 empotrado. Compila "
                  "~/esp_hosted_slave (idf.py -B build_c6 build), copia su "
                  "network_adapter.bin a main/slave_fw/ y recompila.");
#else
    if (s_en_curso) {
        ESP_LOGW(TAG, "Ya hay una actualizacion en marcha");
        return;
    }
    s_en_curso = true;
    /* Fuera del hilo de LVGL: esto tarda minutos y bloquearia la pantalla. */
    xTaskCreate(slave_ota_task, "slave_ota", 4096, NULL, 4, NULL);
#endif
}
