#include "camera.h"
#include "esp_video_init.h"
#include "esp_cam_sensor_detect.h"
#include "esp_log.h"

static const char *TAG = "camera";

/* SENSOR REAL DE ESTA BOARD: OmniVision OV02C10 (NO es un SC2336; la doc previa
 * estaba equivocada). Identificado por bring-up:
 *   - I2C (SCCB) en 0x36   (confirmado por el scan de abajo)
 *   - chip ID 0x5602 en los registros 0x300A/0x300B
 *   - salida RAW10 1920x1080, MIPI-CSI 2 lanes, ISP del P4
 * El driver SC2336 leia SUS registros (0x3107/0x3108) y obtenia basura (0x2101),
 * por eso fallaba la deteccion.
 *
 * PROBLEMA: esp_cam_sensor NO trae driver OV02C10 (verificado en v2.2.0 y en el
 * master de esp-video-components). Hay que portarlo (sub-proyecto aparte). Cuando
 * exista ov02c10_detect(), se enganchara con el MISMO mecanismo validado aqui:
 *   extern esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *cfg);
 *   ESP_CAM_SENSOR_DETECT_FN(ov02c10_jc036, ESP_CAM_SENSOR_MIPI_CSI, 0x36) {
 *       return ov02c10_detect(config);
 *   }
 * (esp_video itera estos registros y usa detect->sccb_addr para crear el SCCB.)
 * El auto-detect del SC2336 queda desactivado en sdkconfig para no ensuciar logs. */

esp_err_t camera_init(i2c_master_bus_handle_t i2c)
{
    if (i2c == NULL) {
        ESP_LOGE(TAG, "handle I2C NULL (llamar tras bsp_i2c_init)");
        return ESP_ERR_INVALID_ARG;
    }

    /* DIAGNOSTICO Fase 1: escanear el bus I2C. Touch GT911 y RTC responden seguro;
     * si el SC2336 esta vivo aparecera (0x30 o 0x36). Si solo salen touch/RTC,
     * el sensor esta apagado (reset/pwdn/MCLK), no es problema de direccion. */
    ESP_LOGI(TAG, "--- scan I2C ---");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(i2c, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C 0x%02X responde", a);
        }
    }
    ESP_LOGI(TAG, "--- fin scan ---");

    /* Reutilizar el bus I2C del proyecto (init_sccb=false -> usar i2c_handle).
     * SC2336 en SCCB 0x30 sobre ese bus. reset/pwdn -1 (este board no los cablea
     * a GPIO de control; a validar en hardware por la deteccion del chip). */
    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb  = false,
            .i2c_handle = i2c,
            .freq       = 100000,
        },
        .reset_pin     = -1,
        .pwdn_pin      = -1,
        .dont_init_ldo = false,
    };

    const esp_video_init_config_t cfg = {
        .csi = &csi,
    };

    esp_err_t ret = esp_video_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init fallo: %s (sensor no detectado? pines/CSI?)",
                 esp_err_to_name(ret));
        return ret;
    }

    /* OJO: esp_video_init() devuelve OK aunque NO detecte sensor (solo loguea error).
     * La deteccion real se ve en el log "sc2336: Get sensor ID" arriba. */
    ESP_LOGI(TAG, "esp_video_init OK (revisar arriba si el sensor SC2336 fue detectado)");
    return ESP_OK;
}
