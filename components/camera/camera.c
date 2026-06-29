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

#define CAM_DEV_PATH "/dev/video0"
#define CAM_SELFTEST_BUFS 2

/* Self-test de bring-up: abre /dev/video0, arranca el stream y cuenta frames
 * durante 2s. Usa select() con timeout para NO colgar el boot si el stream no
 * engancha (p.ej. mipi_clk/timing mal). Loguea tamano medio de frame y una
 * estimacion de luminosidad (proxy para el auto-brillo). One-shot de validacion. */
static void camera_capture_selftest(void)
{
    int fd = open(CAM_DEV_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        ESP_LOGE(TAG, "selftest: no abre %s", CAM_DEV_PATH);
        return;
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "selftest: driver=%s card=%s", cap.driver, cap.card);
    }

    /* Enumerar todos los formatos de salida y preferir uno RAW: bypassa el camino
     * ISP/IPA (que no tenemos para el OV02C10) y da datos crudos del sensor, que
     * es justo lo que necesitamos para medir luminosidad (auto-brillo). */
    uint32_t pick_fmt = 0, raw10_fmt = 0, raw_any_fmt = 0;
    char pick_desc[32] = {0}, raw10_desc[32] = {0}, raw_any_desc[32] = {0};
    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc fdsc = { .index = i, .type = type };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) != 0) break;
        ESP_LOGI(TAG, "selftest: fmt[%d]=%s pixfmt=0x%08lx", i,
                 (char *)fdsc.description, (unsigned long)fdsc.pixelformat);
        if (raw10_fmt == 0 && strstr((char *)fdsc.description, "RAW10") != NULL) {
            raw10_fmt = fdsc.pixelformat;
            strncpy(raw10_desc, (char *)fdsc.description, sizeof(raw10_desc) - 1);
        }
        if (raw_any_fmt == 0 && strstr((char *)fdsc.description, "RAW") != NULL) {
            raw_any_fmt = fdsc.pixelformat;
            strncpy(raw_any_desc, (char *)fdsc.description, sizeof(raw_any_desc) - 1);
        }
    }
    /* Preferir RAW10 (salida nativa del sensor, ISP en bypass, sin IPA). */
    if (raw10_fmt) {
        pick_fmt = raw10_fmt;
        strncpy(pick_desc, raw10_desc, sizeof(pick_desc) - 1);
    } else if (raw_any_fmt) {
        pick_fmt = raw_any_fmt;
        strncpy(pick_desc, raw_any_desc, sizeof(pick_desc) - 1);
    }

    struct v4l2_format fmt = { .type = type };
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "selftest: G_FMT falla");
        close(fd);
        return;
    }
    if (pick_fmt == 0) {
        pick_fmt = fmt.fmt.pix.pixelformat;  /* sin RAW: usar el por defecto */
        strncpy(pick_desc, "default", sizeof(pick_desc) - 1);
    }
    fmt.fmt.pix.pixelformat = pick_fmt;

    /* S_FMT dispara ov02c10_set_format() -> escribe las secuencias de init del
     * sensor. Imprescindible para que el sensor transmita. */
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "selftest: S_FMT falla (%s)", pick_desc);
        close(fd);
        return;
    }
    ESP_LOGI(TAG, "selftest: S_FMT ok -> %s %lux%lu pixfmt=0x%08lx", pick_desc,
             (unsigned long)fmt.fmt.pix.width, (unsigned long)fmt.fmt.pix.height,
             (unsigned long)pick_fmt);

    struct v4l2_requestbuffers req = { .count = CAM_SELFTEST_BUFS, .type = type, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "selftest: REQBUFS falla");
        close(fd);
        return;
    }
    ESP_LOGI(TAG, "selftest: REQBUFS ok (count=%lu)", (unsigned long)req.count);

    uint8_t *buf[CAM_SELFTEST_BUFS] = {0};
    uint32_t len[CAM_SELFTEST_BUFS] = {0};
    for (int i = 0; i < CAM_SELFTEST_BUFS; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) { ESP_LOGE(TAG, "selftest: QUERYBUF"); close(fd); return; }
        buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        len[i] = b.length;
        if (buf[i] == MAP_FAILED) { ESP_LOGE(TAG, "selftest: mmap"); close(fd); return; }
        if (ioctl(fd, VIDIOC_QBUF, &b) != 0) { ESP_LOGE(TAG, "selftest: QBUF"); close(fd); return; }
    }
    ESP_LOGI(TAG, "selftest: %d buffers mapeados (%lu bytes c/u)", CAM_SELFTEST_BUFS, (unsigned long)len[0]);

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "selftest: STREAMON falla");
        close(fd);
        return;
    }
    ESP_LOGI(TAG, "selftest: STREAMON ok, capturando 2s (nonblock poll)...");

    int frames = 0;
    uint64_t total = 0, luma = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 2000000) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
            if (errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            ESP_LOGW(TAG, "selftest: DQBUF err errno=%d", errno);
            break;
        }
        if (b.flags & V4L2_BUF_FLAG_DONE) {
            frames++;
            total += b.bytesused;
            uint8_t *p = buf[b.index];
            uint32_t n = b.bytesused < 8192 ? b.bytesused : 8192;
            uint64_t s = 0;
            for (uint32_t k = 0; k < n; k++) s += p[k];
            luma = n ? (s / n) : 0;
        }
        if (ioctl(fd, VIDIOC_QBUF, &b) != 0) break;
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < CAM_SELFTEST_BUFS; i++) {
        if (buf[i] && buf[i] != MAP_FAILED) munmap(buf[i], len[i]);
    }
    close(fd);

    if (frames > 0) {
        ESP_LOGI(TAG, "selftest OK: %d frames/2s (~%d fps), avg %llu bytes, luma~%llu",
                 frames, frames / 2, (unsigned long long)(total / frames), (unsigned long long)luma);
    } else {
        ESP_LOGW(TAG, "selftest: 0 frames - stream no produce datos (revisar mipi_clk/timing/lanes)");
    }
}

static void camera_selftest_task(void *arg)
{
    camera_capture_selftest();
    vTaskDelete(NULL);
}

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
        /* El DSI ya enciende el LDO MIPI compartido (ch3 @ 2.5V). Que esp_video NO
         * lo reinicialice (evita doble-acquire del LDO del CSI D-PHY). */
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

    /* Si el sensor se detecto, arriba aparece "ov02c10: Detected ... PID=0x5602"
     * y /dev/video0 queda creado. esp_video_init devuelve OK. */
    ESP_LOGI(TAG, "esp_video_init OK - camara OV02C10 lista (/dev/video0)");

    /* Self-test de captura (validacion de bring-up de streaming). */
    xTaskCreate(camera_selftest_task, "cam_selftest", 6144, NULL, 4, NULL);

    return ESP_OK;
}
