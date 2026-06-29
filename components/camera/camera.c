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
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

#define CAM_DEV_PATH    "/dev/video0"
#define CAM_STREAM_BUFS 2

/* Brillo del sensor (controles V4L2, no software). Exposicion en lineas
 * (VTS 2-lane = 2328 -> max util ~2320) y ganancia analogica (rango OV02C10
 * 0x10-0xf8 = 1x-15.5x). Subidos para que la foto no salga oscura. Tunear aqui. */
#define CAM_EXPOSURE  1600
#define CAM_AGAIN     0x60

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

/* ── Thumbnail en COLOR (debayer BGGR -> RGB, para snapshot HTTP) ─────────────
 * El frame es RAW10 MIPI-packed: 4 pixeles en 5 bytes; los 4 primeros bytes de
 * cada grupo son los 8 MSB de 4 pixeles -> los usamos (8 bits limpios).
 * Bayer BGGR: en la celda 2x2 -> B(par,par) G(impar,par)+(par,impar) R(impar,impar).
 * Guardamos en orden BGR (el de BMP), 3 bytes/px, doble buffer en PSRAM. */
#define SRC_W    1928
#define SRC_H    1092
#define THUMB_W  482
#define THUMB_H  273
static uint8_t      *s_thumb[2]   = { NULL, NULL };   /* BGR, 3 bytes/px */
static volatile int  s_thumb_act  = -1;               /* -1 = aun sin frame */

/* Byte MSB (8 bits) del pixel (x,y) del RAW10 MIPI-packed. */
static inline uint8_t raw_px(const uint8_t *p, uint32_t bytes, uint32_t stride, int x, int y)
{
    uint32_t off = (uint32_t)y * stride + (uint32_t)(x >> 2) * 5 + (uint32_t)(x & 3);
    return (off < bytes) ? p[off] : 0;
}

/* LUT de gamma (~0.5, raiz cuadrada) para SUBIR las sombras de la foto sin
 * reventar las altas luces. Solo afecta a la imagen mostrada, NO al frame_luma
 * (que lee el frame crudo) -> el auto-brillo no se ve afectado. */
static uint8_t s_gamma_lut[256];
static bool    s_gamma_ready = false;
static void gamma_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_gamma_lut[i] = (uint8_t)(sqrtf((float)i / 255.0f) * 255.0f + 0.5f);
    }
    s_gamma_ready = true;
}

static void downscale_rgb(const uint8_t *p, uint32_t bytes, uint8_t *dst)
{
    if (!s_gamma_ready) gamma_init();
    uint32_t stride = bytes / SRC_H;   /* ~2410 para RAW10 1928 packed */

    /* Pase 1: balance de blancos gray-world. Medias de cada canal (muestra coarse)
     * -> ganancias para igualar R y B al verde. Quita el tinte (rojizo con luz
     * calida). En lineal, ANTES de la gamma. */
    uint64_t sB = 0, sG = 0, sR = 0;
    uint32_t n = 0;
    for (int oy = 0; oy < THUMB_H; oy += 4) {
        int sy = (oy * SRC_H / THUMB_H) & ~1;
        for (int ox = 0; ox < THUMB_W; ox += 4) {
            int sx = (ox * SRC_W / THUMB_W) & ~1;
            sB += raw_px(p, bytes, stride, sx, sy);
            sG += ((int)raw_px(p, bytes, stride, sx + 1, sy) +
                   (int)raw_px(p, bytes, stride, sx, sy + 1)) / 2;
            sR += raw_px(p, bytes, stride, sx + 1, sy + 1);
            n++;
        }
    }
    float mB = n ? (float)sB / n : 1, mG = n ? (float)sG / n : 1, mR = n ? (float)sR / n : 1;
    float gB = (mB > 1.0f) ? mG / mB : 1.0f;
    float gR = (mR > 1.0f) ? mG / mR : 1.0f;
    if (gB > 4.0f) gB = 4.0f;
    if (gB < 0.25f) gB = 0.25f;
    if (gR > 4.0f) gR = 4.0f;
    if (gR < 0.25f) gR = 0.25f;

    /* Pase 2: debayer + ganancias WB + gamma. */
    for (int oy = 0; oy < THUMB_H; oy++) {
        int sy = (oy * SRC_H / THUMB_H) & ~1;   /* alinear al inicio de la celda 2x2 */
        for (int ox = 0; ox < THUMB_W; ox++) {
            int sx = (ox * SRC_W / THUMB_W) & ~1;
            int b = (int)(raw_px(p, bytes, stride, sx, sy) * gB);
            int g = ((int)raw_px(p, bytes, stride, sx + 1, sy) +
                     (int)raw_px(p, bytes, stride, sx, sy + 1)) / 2;
            int r = (int)(raw_px(p, bytes, stride, sx + 1, sy + 1) * gR);
            if (b > 255) b = 255;
            if (r > 255) r = 255;
            uint8_t *d = &dst[((uint32_t)oy * THUMB_W + ox) * 3];
            d[0] = s_gamma_lut[b]; d[1] = s_gamma_lut[g]; d[2] = s_gamma_lut[r];   /* BGR + WB + gamma */
        }
    }
}

bool camera_snapshot_bmp(uint8_t **out, size_t *out_len)
{
    int act = s_thumb_act;
    if (act < 0 || s_thumb[act] == NULL) return false;
    const uint8_t *src = s_thumb[act];   /* BGR, THUMB_W*3 bytes por fila */

    const uint32_t row_bytes  = (uint32_t)THUMB_W * 3;
    const uint32_t row_padded = (row_bytes + 3) & ~3u;    /* filas a multiplo de 4 */
    const uint32_t off_bits   = 14 + 40;                  /* 24-bit: sin paleta */
    const uint32_t img_size   = row_padded * THUMB_H;
    const uint32_t file_size  = off_bits + img_size;

    uint8_t *bmp = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!bmp) return false;
    memset(bmp, 0, file_size);

    /* BITMAPFILEHEADER */
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = file_size & 0xff; bmp[3] = (file_size >> 8) & 0xff;
    bmp[4] = (file_size >> 16) & 0xff; bmp[5] = (file_size >> 24) & 0xff;
    bmp[10] = off_bits & 0xff; bmp[11] = (off_bits >> 8) & 0xff;
    bmp[12] = (off_bits >> 16) & 0xff; bmp[13] = (off_bits >> 24) & 0xff;
    /* BITMAPINFOHEADER */
    bmp[14] = 40;
    bmp[18] = THUMB_W & 0xff; bmp[19] = (THUMB_W >> 8) & 0xff;
    bmp[22] = THUMB_H & 0xff; bmp[23] = (THUMB_H >> 8) & 0xff;
    bmp[26] = 1;                 /* planes */
    bmp[28] = 24;                /* bpp (color BGR) */
    bmp[34] = img_size & 0xff; bmp[35] = (img_size >> 8) & 0xff;
    bmp[36] = (img_size >> 16) & 0xff; bmp[37] = (img_size >> 24) & 0xff;
    /* clrUsed = 0 (24-bit no usa paleta) */

    /* Pixeles, bottom-up, BGR */
    for (int y = 0; y < THUMB_H; y++) {
        uint8_t *row = &bmp[off_bits + (uint32_t)(THUMB_H - 1 - y) * row_padded];
        memcpy(row, &src[(uint32_t)y * row_bytes], row_bytes);
    }

    *out = bmp;
    *out_len = file_size;
    return true;
}

/* Fija un control V4L2 del sensor (exposicion/ganancia) y loguea el resultado. */
static void cam_set_ctrl(int fd, uint32_t id, int32_t val, const char *name)
{
    struct v4l2_ext_control c = { .id = id, .value = val };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &c };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) == 0) {
        ESP_LOGI(TAG, "ctrl %s=%ld OK", name, (long)val);
    } else {
        ESP_LOGW(TAG, "ctrl %s=%ld no aplicado (errno=%d)", name, (long)val, errno);
    }
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

    /* Subir exposicion y ganancia del sensor para que la imagen no salga oscura. */
    cam_set_ctrl(fd, V4L2_CID_EXPOSURE, CAM_EXPOSURE, "exposure");
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &(struct v4l2_ext_controls){
                  .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1,
                  .controls = &(struct v4l2_ext_control){ .id = V4L2_CID_ANALOGUE_GAIN, .value = CAM_AGAIN } }) == 0) {
        ESP_LOGI(TAG, "ctrl anag=0x%x OK", CAM_AGAIN);
    } else {
        /* Algunos pipelines exponen la ganancia como V4L2_CID_GAIN. */
        cam_set_ctrl(fd, V4L2_CID_GAIN, CAM_AGAIN, "gain");
    }

    /* Thumbnails RGB (BGR) para el snapshot HTTP (doble buffer en PSRAM). Si falla
     * la reserva, seguimos solo con la luma. */
    s_thumb[0] = heap_caps_malloc(THUMB_W * THUMB_H * 3, MALLOC_CAP_SPIRAM);
    s_thumb[1] = heap_caps_malloc(THUMB_W * THUMB_H * 3, MALLOC_CAP_SPIRAM);

    uint32_t ema = 0;
    bool first = true;
    uint32_t fcount = 0;
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

            /* Refrescar el thumbnail cada 4 frames (~9 Hz, de sobra para snapshot). */
            if (s_thumb[0] && s_thumb[1] && (++fcount & 3) == 0) {
                int back = (s_thumb_act == 0) ? 1 : 0;
                downscale_rgb(buf[b.index], b.bytesused, s_thumb[back]);
                s_thumb_act = back;   /* publicar */
            }

            /* DIAGNOSTICO one-shot: a los ~40 frames, log de stats del thumbnail
             * + guardar BMP a SD. Si min<max la captura trae imagen real. */
            static bool s_snap_done = false;
            if (!s_snap_done && s_thumb_act >= 0 && fcount >= 40) {
                s_snap_done = true;
                const uint8_t *t = s_thumb[s_thumb_act];
                const int n = THUMB_W * THUMB_H * 3;   /* BGR */
                uint8_t mn = 255, mx = 0;
                uint32_t sum = 0;
                for (int i = 0; i < n; i++) {
                    if (t[i] < mn) mn = t[i];
                    if (t[i] > mx) mx = t[i];
                    sum += t[i];
                }
                ESP_LOGI(TAG, "snapshot stats(color): %dx%d min=%u max=%u mean=%u",
                         THUMB_W, THUMB_H, mn, mx, (unsigned)(sum / n));
                uint8_t *bmp = NULL;
                size_t blen = 0;
                if (camera_snapshot_bmp(&bmp, &blen)) {
                    FILE *f = fopen("/sdcard/snapshot_boot.bmp", "wb");
                    if (f) {
                        fwrite(bmp, 1, blen, f);
                        fclose(f);
                        ESP_LOGI(TAG, "snapshot guardado /sdcard/snapshot_boot.bmp (%u B)", (unsigned)blen);
                    } else {
                        ESP_LOGW(TAG, "no pude abrir /sdcard/snapshot_boot.bmp");
                    }
                    free(bmp);
                }
            }
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
