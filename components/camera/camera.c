#include "camera.h"
#include "esp_video_init.h"
#include "esp_cam_sensor_detect.h"
#include "esp_log.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "linux/videodev2.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

#define CAM_DEV_PATH    "/dev/video0"
#define CAM_STREAM_BUFS 2

/* Luminosidad media del frame (0-255), suavizada. La actualiza camera_stream_task
 * y la consume el auto-brillo. s_luma_valid=true cuando hay al menos un frame. */
static volatile uint8_t s_luma       = 0;
static volatile bool    s_luma_valid = false;

bool camera_get_luma(uint8_t *out_luma)
{
    if (!s_luma_valid) return false;
    if (out_luma) *out_luma = s_luma;
    return true;
}

/* Calcula la luma media (0-255) de una muestra del centro del frame RAW10.
 * RAW10 va empaquetado en 16 bits/pixel little-endian (valor 0-1023). */
static uint8_t frame_luma(const uint8_t *p, uint32_t bytes)
{
    const uint32_t sample_px = 4096;
    uint32_t start = 0;
    if (bytes / 2 > sample_px) {
        start = ((bytes / 2 - sample_px) / 2) * 2;  /* centrado, alineado a 2 */
    }
    uint64_t sum = 0;
    uint32_t cnt = 0;
    for (uint32_t k = start; k + 1 < bytes && cnt < sample_px; k += 2, cnt++) {
        uint16_t px = (uint16_t)p[k] | ((uint16_t)p[k + 1] << 8);  /* 0-1023 */
        sum += px;
    }
    return cnt ? (uint8_t)((sum / cnt) >> 2) : 0;  /* 0-1023 -> 0-255 */
}

/* Tarea de streaming continuo: mantiene el stream RAW10 abierto y mide la
 * luminosidad media por frame (con suavizado EMA). Es la base del auto-brillo y
 * (proxima iteracion) de la deteccion de movimiento del modo vigilancia. */
static void camera_stream_task(void *arg)
{
    int fd = open(CAM_DEV_PATH, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "stream: no abre %s", CAM_DEV_PATH);
        vTaskDelete(NULL);
        return;
    }
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* Preferir RAW10 (salida nativa del sensor, ISP en bypass, sin IPA). */
    uint32_t pick_fmt = 0;
    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc fdsc = { .index = i, .type = type };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) != 0) break;
        if (strstr((char *)fdsc.description, "RAW10") != NULL) {
            pick_fmt = fdsc.pixelformat;
            break;
        }
    }
    struct v4l2_format fmt = { .type = type };
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    if (pick_fmt) fmt.fmt.pix.pixelformat = pick_fmt;
    /* S_FMT dispara ov02c10_set_format() -> escribe el init del sensor. */
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "stream: S_FMT falla");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_requestbuffers req = { .count = CAM_STREAM_BUFS, .type = type, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "stream: REQBUFS falla");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    uint8_t *buf[CAM_STREAM_BUFS] = {0};
    uint32_t len[CAM_STREAM_BUFS] = {0};
    for (int i = 0; i < CAM_STREAM_BUFS; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) { close(fd); vTaskDelete(NULL); return; }
        buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        len[i] = b.length;
        if (buf[i] == MAP_FAILED) { close(fd); vTaskDelete(NULL); return; }
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "stream: STREAMON falla");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "stream: capturando RAW10 para luma/vigilancia");

    uint32_t ema = 0;
    bool first = true;
    while (1) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (b.flags & V4L2_BUF_FLAG_DONE) {
            uint8_t luma = frame_luma(buf[b.index], b.bytesused);
            /* EMA fuerte (cte ~32 frames ~0.9s): el brillo ambiente cambia lento;
             * evita parpadeos por ruido o por una mano que pasa. */
            if (first) { ema = luma; first = false; }
            else       { ema = (ema * 31 + luma) / 32; }
            s_luma = (uint8_t)ema;
            s_luma_valid = true;
        }
        ioctl(fd, VIDIOC_QBUF, &b);
    }
}

/* SENSOR DE ESTA BOARD: OmniVision OV02C10 (no SC2336). Detectado en SCCB 0x36,
 * chip ID 0x5602 (regs 0x300A/0x300B), RAW10 1928x1092, MIPI-CSI 2 lanes.
 * Driver propio en components/ov02c10/ (portado del kernel Linux). El detect se
 * registra alli via ESP_CAM_SENSOR_DETECT_FN a 0x36; el auto-detect del SC2336
 * queda desactivado en sdkconfig. Streaming OK (~37fps) con mipi_clk=800Mbps. */

esp_err_t camera_init(i2c_master_bus_handle_t i2c)
{
    if (i2c == NULL) {
        ESP_LOGE(TAG, "handle I2C NULL (llamar tras bsp_i2c_init)");
        return ESP_ERR_INVALID_ARG;
    }

    /* Reutilizar el bus I2C del proyecto (init_sccb=false -> usar i2c_handle).
     * OV02C10 en SCCB 0x36. reset/pwdn -1 (la board no los cablea a GPIO; la
     * camara lleva oscilador propio). dont_init_ldo=true: el LDO MIPI ch3 ya lo
     * enciende el DSI y es compartido. */
    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb  = false,
            .i2c_handle = i2c,
            .freq       = 100000,
        },
        .reset_pin     = -1,
        .pwdn_pin      = -1,
        .dont_init_ldo = true,
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

    ESP_LOGI(TAG, "esp_video_init OK - camara OV02C10 lista (/dev/video0)");

    /* Tarea de streaming continuo: mide luminosidad (auto-brillo) y servira para
     * la vigilancia por movimiento. */
    xTaskCreate(camera_stream_task, "cam_stream", 6144, NULL, 4, NULL);

    return ESP_OK;
}
