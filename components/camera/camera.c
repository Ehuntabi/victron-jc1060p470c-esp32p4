#include "camera.h"
#include "esp_video_init.h"
#include "esp_log.h"

static const char *TAG = "camera";

esp_err_t camera_init(i2c_master_bus_handle_t i2c)
{
    if (i2c == NULL) {
        ESP_LOGE(TAG, "handle I2C NULL (llamar tras bsp_i2c_init)");
        return ESP_ERR_INVALID_ARG;
    }

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

    ESP_LOGI(TAG, "camara MIPI-CSI inicializada (esp_video OK, SC2336 detectado)");
    return ESP_OK;
}
