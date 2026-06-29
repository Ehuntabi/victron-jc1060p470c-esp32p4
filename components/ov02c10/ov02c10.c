/*
 * Driver local OV02C10 para esp_cam_sensor (board Guition JC1060P470C).
 *
 * El sensor de camara de esta board es un OmniVision OV02C10 (la doc previa decia
 * SC2336, era incorrecta): SCCB 0x36, chip ID 0x5602 en 0x300A/0x300B, salida
 * RAW10 1928x1092, MIPI-CSI 2 lanes (link 400MHz / 800Mbps por lane), MCLK 19.2MHz.
 *
 * esp_cam_sensor (v2.2.0 y master) NO trae driver OV02C10. Este es un port del
 * driver mainline de Linux (drivers/media/i2c/ov02c10.c) a la estructura de
 * esp_cam_sensor, usando sc2336.c como plantilla. Las secuencias de registros
 * estan en ov02c10_settings.h (copiadas EXACTAS del driver Linux).
 */

#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "ov02c10_settings.h"
#include "ov02c10.h"

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif
#define delay_ms(ms)  vTaskDelay((ms > portTICK_PERIOD_MS ? ms / portTICK_PERIOD_MS : 1))

#define OV02C10_IO_MUX_LOCK(mux)
#define OV02C10_IO_MUX_UNLOCK(mux)
#define OV02C10_ENABLE_OUT_XCLK(pin, clk)
#define OV02C10_DISABLE_OUT_XCLK(pin)

/* Registros OV02C10 (16-bit addr) */
#define OV02C10_REG_CHIP_ID_H   0x300a
#define OV02C10_REG_CHIP_ID_L   0x300b
#define OV02C10_REG_STREAM      0x0100   /* 1 = stream on, 0 = standby */
#define OV02C10_REG_SOFT_RESET  0x0103
#define OV02C10_REG_EXPOSURE_H  0x3501
#define OV02C10_REG_EXPOSURE_L  0x3502
#define OV02C10_REG_ANA_GAIN_H  0x3508
#define OV02C10_REG_ANA_GAIN_L  0x3509

#define OV02C10_EXP_MIN         4
#define OV02C10_EXP_MAX_OFFSET  8        /* exposure_max = VTS - 8 (Linux EXPOSURE_MAX_MARGIN) */

#define EXPOSURE_V4L2_UNIT_US   100
#define EXPOSURE_V4L2_TO_OV02C10(v, sf) \
    ((uint32_t)(((double)v) * (sf)->fps * (sf)->isp_info->isp_v1_info.vts / (1000000 / EXPOSURE_V4L2_UNIT_US) + 0.5))
#define EXPOSURE_OV02C10_TO_V4L2(v, sf) \
    ((int32_t)(((double)v) * 1000000 / (sf)->fps / (sf)->isp_info->isp_v1_info.vts / EXPOSURE_V4L2_UNIT_US + 0.5))

static const char *TAG = "ov02c10";

typedef struct {
    uint32_t exposure_val;
    uint32_t exposure_max;
    uint32_t gain_index;
} ov02c10_para_t;

struct ov02c10_cam {
    ov02c10_para_t para;
};

/*
 * Mapa de ganancia analogica (solo analog gain, sin digital): registro 0x3508/0x3509
 * = gainval << 4, con gainval en [0x10, 0xf8] (1x..15.5x). total_gain = gainval*1000/16.
 * Suficiente para un AE basico; el ajuste fino (incluir digital gain 0x350a) queda
 * pendiente de validacion en hardware. Indices paralelos: again_map[i] <-> total_gain_map[i].
 */
static const uint16_t ov02c10_again_map[] = {
    0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
    0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x68, 0x6c,
    0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c,
    0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc, 0xc0, 0xc4, 0xc8, 0xcc,
    0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8,
};
static const uint32_t ov02c10_total_gain_map[] = {
    1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000, 3250, 3500, 3750,
    4000, 4250, 4500, 4750, 5000, 5250, 5500, 5750, 6000, 6250, 6500, 6750,
    7000, 7250, 7500, 7750, 8000, 8250, 8500, 8750, 9000, 9250, 9500, 9750,
    10000, 10250, 10500, 10750, 11000, 11250, 11500, 11750, 12000, 12250, 12500, 12750,
    13000, 13250, 13500, 13750, 14000, 14250, 14500, 14750, 15000, 15250, 15500,
};
#define OV02C10_GAIN_MAP_COUNT  ARRAY_SIZE(ov02c10_again_map)

static const esp_cam_sensor_isp_info_t ov02c10_isp_info_mipi = {
    .isp_v1_info = {
        .version    = SENSOR_ISP_INFO_VERSION_DEFAULT,
        .pclk       = 79617600,  /* aprox = hts*vts*fps (1140*2328*30); a verificar en HW */
        .hts        = 1140,      /* 0x380c/0x380d = 0x0474 (HTS programado en modo 2-lane) */
        .vts        = 2328,      /* 0x380e/0x380f = 0x0918 */
        .exp_def    = 0x046c,    /* 0x3501/0x3502 del init = 1132 lineas */
        .gain_def   = 0,         /* indice de ganancia = 1x */
        .tline_ns   = 14316,     /* 1e9 / (fps*vts) = 1e9/(30*2328) */
        .bayer_type = ESP_CAM_SENSOR_BAYER_GRBG,  /* SGRBG10 del driver Linux */
    },
};

static const esp_cam_sensor_format_t ov02c10_format_info_mipi[] = {
    {
        .name = "MIPI_2lane_19M2input_RAW10_1928x1092_30fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 19200000,  /* MCLK que espera el OV02C10 (la board lo provee por oscilador) */
        .width = 1928,
        .height = 1092,
        .regs = ov02c10_mipi_2lane_1928x1092_raw10_30fps,
        .regs_size = ARRAY_SIZE(ov02c10_mipi_2lane_1928x1092_raw10_30fps),
        .fps = 30,
        .isp_info = &ov02c10_isp_info_mipi,
        .mipi_info = {
            .mipi_clk = 800000000,  /* 800 Mbps/lane (link 400MHz DDR) */
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    },
};

static esp_err_t ov02c10_read(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t *read_buf)
{
    return esp_sccb_transmit_receive_reg_a16v8(sccb_handle, reg, read_buf);
}

static esp_err_t ov02c10_write(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t data)
{
    return esp_sccb_transmit_reg_a16v8(sccb_handle, reg, data);
}

static esp_err_t ov02c10_write_array(esp_sccb_io_handle_t sccb_handle, const ov02c10_reginfo_t *regarray)
{
    int i = 0;
    esp_err_t ret = ESP_OK;
    while ((ret == ESP_OK) && regarray[i].reg != OV02C10_REG_END) {
        if (regarray[i].reg != OV02C10_REG_DELAY) {
            ret = ov02c10_write(sccb_handle, regarray[i].reg, regarray[i].val);
        } else {
            delay_ms(regarray[i].val);
        }
        i++;
    }
    return ret;
}

static esp_err_t ov02c10_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{
    esp_err_t ret;
    uint8_t pid_h, pid_l;

    ret = ov02c10_read(dev->sccb_handle, OV02C10_REG_CHIP_ID_H, &pid_h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ov02c10_read(dev->sccb_handle, OV02C10_REG_CHIP_ID_L, &pid_l);
    if (ret != ESP_OK) {
        return ret;
    }
    id->pid = (pid_h << 8) | pid_l;
    return ret;
}

static esp_err_t ov02c10_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    esp_err_t ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_STREAM, enable ? 0x01 : 0x00);
    dev->stream_status = enable;
    ESP_LOGD(TAG, "Stream=%d", enable);
    return ret;
}

static esp_err_t ov02c10_set_exp_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)
{
    esp_err_t ret;
    struct ov02c10_cam *cam = (struct ov02c10_cam *)dev->priv;
    uint32_t value_buf = MAX(u32_val, (uint32_t)OV02C10_EXP_MIN);
    value_buf = MIN(value_buf, cam->para.exposure_max);

    ESP_LOGD(TAG, "set exposure 0x%" PRIx32, value_buf);
    ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_EXPOSURE_H, (value_buf >> 8) & 0xff);
    ret |= ov02c10_write(dev->sccb_handle, OV02C10_REG_EXPOSURE_L, value_buf & 0xff);
    if (ret == ESP_OK) {
        cam->para.exposure_val = value_buf;
    }
    return ret;
}

static esp_err_t ov02c10_set_total_gain_val(esp_cam_sensor_device_t *dev, uint32_t index)
{
    esp_err_t ret;
    struct ov02c10_cam *cam = (struct ov02c10_cam *)dev->priv;
    uint16_t reg;

    if (index >= OV02C10_GAIN_MAP_COUNT) {
        index = OV02C10_GAIN_MAP_COUNT - 1;
    }
    reg = ov02c10_again_map[index] << 4;   /* Linux: analog gain = val << 4 */
    ESP_LOGD(TAG, "set gain index %" PRIu32 " (reg 0x%04x)", index, reg);
    ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_ANA_GAIN_H, (reg >> 8) & 0xff);
    ret |= ov02c10_write(dev->sccb_handle, OV02C10_REG_ANA_GAIN_L, reg & 0xff);
    if (ret == ESP_OK) {
        cam->para.gain_index = index;
    }
    return ret;
}

static esp_err_t ov02c10_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    esp_err_t ret = ESP_OK;
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = OV02C10_EXP_MIN;
        qdesc->number.maximum = dev->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET;
        qdesc->number.step = 1;
        qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.exp_def;
        break;
    case ESP_CAM_SENSOR_EXPOSURE_US:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = EXPOSURE_OV02C10_TO_V4L2(OV02C10_EXP_MIN, dev->cur_format);
        qdesc->number.maximum = EXPOSURE_OV02C10_TO_V4L2((dev->cur_format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET), dev->cur_format);
        qdesc->number.step = MAX(EXPOSURE_OV02C10_TO_V4L2(1, dev->cur_format), 1);
        qdesc->default_value = EXPOSURE_OV02C10_TO_V4L2((dev->cur_format->isp_info->isp_v1_info.exp_def), dev->cur_format);
        break;
    case ESP_CAM_SENSOR_GAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
        qdesc->enumeration.count = OV02C10_GAIN_MAP_COUNT;
        qdesc->enumeration.elements = ov02c10_total_gain_map;
        qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.gain_def;
        break;
    default:
        ESP_LOGD(TAG, "id=%" PRIx32 " is not supported", qdesc->id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t ov02c10_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;
    struct ov02c10_cam *cam = (struct ov02c10_cam *)dev->priv;
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        *(uint32_t *)arg = cam->para.exposure_val;
        break;
    case ESP_CAM_SENSOR_GAIN:
        *(uint32_t *)arg = cam->para.gain_index;
        break;
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    return ret;
}

static esp_err_t ov02c10_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        uint32_t u32_val = *(uint32_t *)arg;
        ret = ov02c10_set_exp_val(dev, u32_val);
        break;
    }
    case ESP_CAM_SENSOR_EXPOSURE_US: {
        uint32_t u32_val = *(uint32_t *)arg;
        uint32_t ori_exp = EXPOSURE_V4L2_TO_OV02C10(u32_val, dev->cur_format);
        ret = ov02c10_set_exp_val(dev, ori_exp);
        break;
    }
    case ESP_CAM_SENSOR_GAIN: {
        uint32_t u32_val = *(uint32_t *)arg;
        ret = ov02c10_set_total_gain_val(dev, u32_val);
        break;
    }
    default:
        ESP_LOGE(TAG, "set id=%" PRIx32 " is not supported", id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t ov02c10_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);

    formats->count = ARRAY_SIZE(ov02c10_format_info_mipi);
    formats->format_array = &ov02c10_format_info_mipi[0];
    return ESP_OK;
}

static esp_err_t ov02c10_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, sensor_cap);

    sensor_cap->fmt_raw = 1;
    return 0;
}

static esp_err_t ov02c10_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    struct ov02c10_cam *cam = (struct ov02c10_cam *)dev->priv;
    esp_err_t ret;

    if (format == NULL) {
        format = &ov02c10_format_info_mipi[0];
    }

    ret = ov02c10_write_array(dev->sccb_handle, (const ov02c10_reginfo_t *)format->regs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set format regs fail");
        return ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT;
    }

    dev->cur_format = format;
    cam->para.exposure_val = format->isp_info->isp_v1_info.exp_def;
    cam->para.gain_index = format->isp_info->isp_v1_info.gain_def;
    cam->para.exposure_max = format->isp_info->isp_v1_info.vts - OV02C10_EXP_MAX_OFFSET;
    return ret;
}

static esp_err_t ov02c10_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);

    esp_err_t ret = ESP_FAIL;
    if (dev->cur_format != NULL) {
        memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
        ret = ESP_OK;
    }
    return ret;
}

static esp_err_t ov02c10_hw_reset(esp_cam_sensor_device_t *dev)
{
    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
    }
    return ESP_OK;
}

static esp_err_t ov02c10_soft_reset(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ov02c10_write(dev->sccb_handle, OV02C10_REG_SOFT_RESET, 0x01);
    delay_ms(5);
    return ret;
}

static esp_err_t ov02c10_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    esp_err_t ret = ESP_OK;
    uint8_t regval;
    esp_cam_sensor_reg_val_t *sensor_reg;
    OV02C10_IO_MUX_LOCK(mux);

    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_HW_RESET:
        ret = ov02c10_hw_reset(dev);
        break;
    case ESP_CAM_SENSOR_IOC_SW_RESET:
        ret = ov02c10_soft_reset(dev);
        break;
    case ESP_CAM_SENSOR_IOC_S_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = ov02c10_write(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
        break;
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ret = ov02c10_set_stream(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_G_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = ov02c10_read(dev->sccb_handle, sensor_reg->regaddr, &regval);
        if (ret == ESP_OK) {
            sensor_reg->value = regval;
        }
        break;
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
        ret = ov02c10_get_sensor_id(dev, arg);
        break;
    default:
        break;
    }

    OV02C10_IO_MUX_UNLOCK(mux);
    return ret;
}

static esp_err_t ov02c10_power_on(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    if (dev->xclk_pin >= 0) {
        OV02C10_ENABLE_OUT_XCLK(dev->xclk_pin, dev->xclk_freq_hz);
    }

    if (dev->pwdn_pin >= 0) {
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << dev->pwdn_pin;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);
        gpio_set_level(dev->pwdn_pin, 1);
        delay_ms(10);
        gpio_set_level(dev->pwdn_pin, 0);
        delay_ms(10);
    }

    if (dev->reset_pin >= 0) {
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << dev->reset_pin;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
    }

    return ret;
}

static esp_err_t ov02c10_power_off(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    if (dev->xclk_pin >= 0) {
        OV02C10_DISABLE_OUT_XCLK(dev->xclk_pin);
    }

    if (dev->pwdn_pin >= 0) {
        gpio_set_level(dev->pwdn_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->pwdn_pin, 1);
        delay_ms(10);
    }

    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
    }

    return ret;
}

static esp_err_t ov02c10_delete(esp_cam_sensor_device_t *dev)
{
    ESP_LOGD(TAG, "del ov02c10 (%p)", dev);
    if (dev) {
        if (dev->priv) {
            free(dev->priv);
            dev->priv = NULL;
        }
        free(dev);
    }
    return ESP_OK;
}

static const esp_cam_sensor_ops_t ov02c10_ops = {
    .query_para_desc = ov02c10_query_para_desc,
    .get_para_value = ov02c10_get_para_value,
    .set_para_value = ov02c10_set_para_value,
    .query_support_formats = ov02c10_query_support_formats,
    .query_support_capability = ov02c10_query_support_capability,
    .set_format = ov02c10_set_format,
    .get_format = ov02c10_get_format,
    .priv_ioctl = ov02c10_priv_ioctl,
    .del = ov02c10_delete
};

esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config)
{
    esp_cam_sensor_device_t *dev = NULL;
    struct ov02c10_cam *cam;

    if (config == NULL) {
        return NULL;
    }

    dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "No memory for camera");
        return NULL;
    }

    cam = heap_caps_calloc(1, sizeof(struct ov02c10_cam), MALLOC_CAP_DEFAULT);
    if (!cam) {
        ESP_LOGE(TAG, "failed to calloc cam");
        free(dev);
        return NULL;
    }

    dev->name = (char *)OV02C10_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &ov02c10_ops;
    dev->priv = cam;
    dev->cur_format = &ov02c10_format_info_mipi[0];

    if (ov02c10_power_on(dev) != ESP_OK) {
        ESP_LOGE(TAG, "Camera power on failed");
        goto err_free_handler;
    }

    if (ov02c10_get_sensor_id(dev, &dev->id) != ESP_OK) {
        ESP_LOGE(TAG, "Get sensor ID failed");
        goto err_free_handler;
    } else if (dev->id.pid != OV02C10_PID) {
        ESP_LOGE(TAG, "Camera sensor is not OV02C10, PID=0x%x", dev->id.pid);
        goto err_free_handler;
    }
    ESP_LOGI(TAG, "Detected Camera sensor OV02C10 PID=0x%x", dev->id.pid);

    return dev;

err_free_handler:
    ov02c10_power_off(dev);
    free(dev->priv);
    free(dev);
    return NULL;
}

/* Registro del auto-detect a 0x36. Sin guard de Kconfig: este componente local
 * existe solo para esta board, asi que el detect siempre se registra. esp_video
 * itera estos registros y usa detect->sccb_addr (0x36) para crear el SCCB. */
ESP_CAM_SENSOR_DETECT_FN(ov02c10_detect, ESP_CAM_SENSOR_MIPI_CSI, OV02C10_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return ov02c10_detect(config);
}
