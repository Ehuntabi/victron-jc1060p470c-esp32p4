#include "camera.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"   /* VIDIOC_S_DQBUF_TIMEOUT (que DQBUF no bloquee infinito) */
#include "esp_cam_sensor_detect.h"
#include <sys/time.h>          /* struct timeval para el timeout de DQBUF */
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
#include "driver/jpeg_encode.h"   /* codec JPEG por HW del ESP32-P4 (esp_driver_jpeg) */
#include "freertos/semphr.h"      /* mutex del anillo de capturas en RAM */
#include "esp_task_wdt.h"         /* vigilar la tarea de camara (DQBUF puede colgarse) */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

#define CAM_DEV_PATH    "/dev/video0"
#define CAM_STREAM_BUFS 2

/* THROTTLE de la camara: NO streamea a tope (su DMA continuo bloquea la SD). Con un
 * STREAMON unico, se consume 1 frame y se duerme: los buffers se llenan y el GDMA de
 * la camara se para por contrapresion -> SD libre entre frames. Tunear aqui. */
#define CAM_IDLE_MS       2000 /* sleep normal entre frames -> GDMA parado -> SD libre */
/* Vigilancia: a 250ms el DMA de la camara era 8x mas frecuente -> chocaba con las
 * escrituras de fondo a SD (datalogger/bathist/log_save) y un comando SD atascado
 * retenia un spinlock >300ms -> INT WDT (reinicio). 1500ms da contension parecida al
 * modo normal (probado estable). Movimiento muestreado cada ~1.5s (vale para detectar
 * que alguien entra; cooldown entre fotos ya son 4s). */
#define CAM_SURV_IDLE_MS  1500

/* Brillo del sensor (controles V4L2, no software). Exposicion en lineas
 * (VTS 2-lane = 2328 -> max util ~2320) y ganancia analogica (rango OV02C10
 * 0x10-0xf8 = 1x-15.5x). Subidos para que la foto no salga oscura. Tunear aqui. */
#define CAM_EXPOSURE  1600
/* Ganancia del OV02C10 = INDICE en su mapa (ov02c10_again_map, 0..58 = 1x..15.5x),
 * se fija con V4L2_CID_GAIN. 24 ~= 7x (sube el sujeto a contraluz). Tunear aqui. */
#define CAM_GAIN_IDX  24
/* En VIGILANCIA la pantalla esta apagada -> escena oscura (brillo ~24). Subimos
 * exposicion y ganancia para fotos mas claras. Exposicion cerca del max (VTS~2320)
 * y un punto mas de ganancia. (Compromiso: algo mas de grano / posible movido). */
#define CAM_EXPOSURE_SURV  2300   /* mas luz via tiempo (no mete ruido como la ganancia) */
#define CAM_GAIN_IDX_SURV  26     /* 26 (no 32): el auto-niveles ya sube brillo -> menos grano */

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

/* ── Thumbnail en COLOR (debayer BGGR -> RGB, para snapshot HTTP) ─────────────
 * El frame es RAW10 MIPI-packed: 4 pixeles en 5 bytes; los 4 primeros bytes de
 * cada grupo son los 8 MSB de 4 pixeles -> los usamos (8 bits limpios).
 * Bayer BGGR: en la celda 2x2 -> B(par,par) G(impar,par)+(par,impar) R(impar,impar).
 * Guardamos en orden BGR (el de BMP), 3 bytes/px, doble buffer en PSRAM. */
#define SRC_W    1928
#define SRC_H    1092
/* Resolucion nativa del debayer 2x2 (cada pixel = una celda Bayer): maxima
 * nitidez sin interpolar. 964x546 = 1928x1092 / 2. */
#define THUMB_W  964
#define THUMB_H  546
static uint8_t      *s_thumb[2]   = { NULL, NULL };   /* BGR, 3 bytes/px */
static volatile int  s_thumb_act  = -1;               /* -1 = aun sin frame */
/* Acumulador para promediado temporal (media movil anti-grano). Guarda el valor
 * de display en 12.4 fijo (valor<<4). La constante = 2^CAM_EMA_SHIFT frames:
 * mas = menos grano pero mas arrastre de movimiento. 8 arrastraba ("movida"),
 * 4 es el compromiso. 1 = sin promediado. Tunear aqui. */
#define CAM_EMA_SHIFT 2
static uint16_t     *s_accum      = NULL;
static volatile bool s_no_temporal = false;  /* en vigilancia: sin promediado (mas nitido) */

/* Byte MSB (8 bits) del pixel (x,y) del RAW10 MIPI-packed. */
static inline uint8_t raw_px(const uint8_t *p, uint32_t bytes, uint32_t stride, int x, int y)
{
    uint32_t off = (uint32_t)y * stride + (uint32_t)(x >> 2) * 5 + (uint32_t)(x & 3);
    return (off < bytes) ? p[off] : 0;
}

/* Luma media (0-255) del centro del frame para el auto-brillo. Muestrea los bytes
 * MSB via raw_px (0-255 limpios). OJO: NO leer el RAW10 como 16-bit/px (va packed
 * 4px/5B) -> daria una señal en diente de sierra (mod 256) inservible. */
static uint8_t frame_luma(const uint8_t *p, uint32_t bytes)
{
    uint32_t stride = bytes / SRC_H;
    if (stride == 0) return 0;
    uint64_t sum = 0;
    uint32_t cnt = 0;
    for (int y = SRC_H / 4; y < SRC_H * 3 / 4; y += 8)
        for (int x = SRC_W / 4; x < SRC_W * 3 / 4; x += 8) {
            sum += raw_px(p, bytes, stride, x, y);
            cnt++;
        }
    return cnt ? (uint8_t)(sum / cnt) : 0;
}

/* LUT de gamma para subir las sombras de la foto sin reventar las altas luces.
 * <1 aclara, =1 lineal. 0.5 (raiz) aclaraba demasiado ("muy clara"); 0.72 es un
 * punto intermedio. Solo afecta a la imagen mostrada, NO al frame_luma (que lee
 * el frame crudo) -> el auto-brillo no se ve afectado. Tunear aqui. */
#define CAM_GAMMA 0.55f   /* mas bajo: levanta sombras (sujeto a contraluz menos oscuro) */
static uint8_t s_gamma_lut[256];
static bool    s_gamma_ready = false;
static void gamma_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_gamma_lut[i] = (uint8_t)(powf((float)i / 255.0f, CAM_GAMMA) * 255.0f + 0.5f);
    }
    s_gamma_ready = true;
}

static void downscale_rgb(const uint8_t *p, uint32_t bytes, uint8_t *dst)
{
    if (!s_gamma_ready) gamma_init();
    uint32_t stride = bytes / SRC_H;   /* ~2410 para RAW10 1928 packed */

    /* Pase 1: balance de blancos gray-world (medias de canal) + histograma de luma
     * para auto-niveles. Muestra coarse (1 de cada 4x4) y SOLO de la zona CENTRAL
     * (50% central): medicion ponderada al centro -> expone para el sujeto del
     * primer plano e ignora un contraluz/ventana de los bordes. */
    uint64_t sB = 0, sG = 0, sR = 0;
    uint32_t n = 0, nwb = 0;
    uint32_t hist[256] = {0};
    const int cy0 = THUMB_H / 4, cy1 = THUMB_H - THUMB_H / 4;
    const int cx0 = THUMB_W / 4, cx1 = THUMB_W - THUMB_W / 4;
    for (int oy = cy0; oy < cy1; oy += 4) {
        int sy = (oy * SRC_H / THUMB_H) & ~1;
        for (int ox = cx0; ox < cx1; ox += 4) {
            int sx = (ox * SRC_W / THUMB_W) & ~1;
            int b = raw_px(p, bytes, stride, sx, sy);
            int g = ((int)raw_px(p, bytes, stride, sx + 1, sy) +
                     (int)raw_px(p, bytes, stride, sx, sy + 1)) / 2;
            int r = raw_px(p, bytes, stride, sx + 1, sy + 1);
            n++;
            hist[(b + 2 * g + r) / 4]++;   /* luma aprox (todos, para auto-niveles) */
            /* WB gray-world SOLO con pixeles bien expuestos: los quemados (ventana a
             * contraluz) desvian las medias y meten dominante (verde/magenta). */
            if (b < 200 && g < 200 && r < 200) { sB += b; sG += g; sR += r; nwb++; }
        }
    }
    float mB = nwb ? (float)sB / nwb : 1, mG = nwb ? (float)sG / nwb : 1, mR = nwb ? (float)sR / nwb : 1;
    float gB = (mB > 1.0f) ? mG / mB : 1.0f;
    float gR = (mR > 1.0f) ? mG / mR : 1.0f;
    if (gB > 4.0f) gB = 4.0f;
    if (gB < 0.25f) gB = 0.25f;
    if (gR > 4.0f) gR = 4.0f;
    if (gR < 0.25f) gR = 0.25f;

    /* Auto-niveles: percentiles 2% (lo) y 98% (hi) del histograma -> estirar a
     * 0-255. Sube brillo Y contraste adaptandose a la luz de la escena. */
    uint32_t lo_th = n / 50, hi_th = n - n / 50;   /* 2% / 98% */
    int lo = 0, hi = 255;
    uint32_t acc = 0;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= lo_th) { lo = i; break; } }
    acc = 0;
    for (int i = 255; i >= 0; i--) { acc += hist[i]; if (acc >= n - hi_th) { hi = i; break; } }
    if (hi <= lo) hi = lo + 1;
    float lscale = 255.0f / (float)(hi - lo);

    /* Pase 2: debayer + WB + auto-niveles + gamma. */
    for (int oy = 0; oy < THUMB_H; oy++) {
        int sy = (oy * SRC_H / THUMB_H) & ~1;   /* alinear al inicio de la celda 2x2 */
        for (int ox = 0; ox < THUMB_W; ox++) {
            int sx = (ox * SRC_W / THUMB_W) & ~1;
            int b = (int)(raw_px(p, bytes, stride, sx, sy) * gB);
            int g = ((int)raw_px(p, bytes, stride, sx + 1, sy) +
                     (int)raw_px(p, bytes, stride, sx, sy + 1)) / 2;
            int r = (int)(raw_px(p, bytes, stride, sx + 1, sy + 1) * gR);
            /* auto-niveles (estiramiento por percentiles) */
            b = (int)((b - lo) * lscale);
            g = (int)((g - lo) * lscale);
            r = (int)((r - lo) * lscale);
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            uint32_t idx = ((uint32_t)oy * THUMB_W + ox) * 3;
            uint8_t *d = &dst[idx];
            int fb = s_gamma_lut[b], fg = s_gamma_lut[g], fr = s_gamma_lut[r];  /* valor display */
            if (s_accum && !s_no_temporal) {
                /* Promediado temporal (EMA cte 8): accum = val<<4. Quita grano. En
                 * vigilancia se desactiva (s_no_temporal): con movimiento el promediado
                 * deja 'fantasmas'; un frame fresco sale mas nitido (a costa de grano). */
                uint16_t *a = &s_accum[idx];
                a[0] = a[0] - (a[0] >> CAM_EMA_SHIFT) + (uint16_t)(fb << (4 - CAM_EMA_SHIFT));
                a[1] = a[1] - (a[1] >> CAM_EMA_SHIFT) + (uint16_t)(fg << (4 - CAM_EMA_SHIFT));
                a[2] = a[2] - (a[2] >> CAM_EMA_SHIFT) + (uint16_t)(fr << (4 - CAM_EMA_SHIFT));
                d[0] = a[0] >> 4;
                d[1] = a[1] >> 4;
                d[2] = a[2] >> 4;
            } else {
                d[0] = fb;
                d[1] = fg;
                d[2] = fr;
            }
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

    /* Este BMP (~1.58MB) se reserva bajo demanda HTTP. Dejar un SUELO de PSRAM libre
     * para los buffers DMA del SDIO del C6: si se agota, su alloc hace assert -> reboot.
     * Si no hay margen, no servir el snapshot (mejor 503 que tumbar el WiFi). */
    #define VIG_PSRAM_FLOOR  (1024 * 1024)   /* 1MB de reserva para el SDIO/sistema */
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < file_size + VIG_PSRAM_FLOOR) {
        ESP_LOGW(TAG, "snapshot: PSRAM baja, no genero el BMP (protejo el SDIO)");
        return false;
    }
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

/* ── JPEG por hardware (ESP32-P4) para las fotos de vigilancia ───────────────
 * El BMP de 1.5MB chocaba con la camara en el bus de la SD (escritura larga).
 * El JPEG por HW codifica en PSRAM (no toca la SD) y deja una salida ~80KB ->
 * escritura corta como las del datalogger -> NO choca. Sigue el ejemplo oficial
 * esp-idf/examples/peripherals/jpeg/jpeg_encode. */
#define JPEG_W        960   /* recorte del thumbnail 964 -> 960 (multiplo de 16) */
#define JPEG_H        544   /* recorte del thumbnail 546 -> 544 (multiplo de 16) */
/* 85: mejora sobre 80 sin disparar el tamano. 92+YUV444 daba ~500KB/foto -> el
 * tráfico del encoder (2D-DMA) + servir esas imagenes por WiFi saturaba el bus -> INT WDT. */
#define JPEG_QUALITY  85
static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static uint8_t  *s_jpeg_in  = NULL;     /* RGB888 (jpeg_alloc_encoder_mem) */
static uint8_t  *s_jpeg_out = NULL;     /* bitstream JPEG (jpeg_alloc_encoder_mem) */
static size_t    s_jpeg_in_sz = 0, s_jpeg_out_sz = 0;

static bool camera_jpeg_init(void)
{
    if (s_jpeg_enc) return true;        /* ya inicializado */
    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 200 };
    if (jpeg_new_encoder_engine(&eng, &s_jpeg_enc) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg: no pude crear el motor HW");
        s_jpeg_enc = NULL;
        return false;
    }
    jpeg_encode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_jpeg_in  = jpeg_alloc_encoder_mem((size_t)JPEG_W * JPEG_H * 3, &in_cfg,  &s_jpeg_in_sz);
    /* Salida = 1/2 del RGB. A q85 con frames nocturnos ruidosos (ganancia alta) el
     * JPEG comprime poco; 1/3 (~522KB) se quedaba corto y la captura se perdia en
     * silencio. 1/2 (~783KB) da margen amplio. */
    s_jpeg_out = jpeg_alloc_encoder_mem((size_t)JPEG_W * JPEG_H * 3 / 2, &out_cfg, &s_jpeg_out_sz);
    if (!s_jpeg_in || !s_jpeg_out) {
        ESP_LOGE(TAG, "jpeg: sin memoria para buffers");
        /* Dejar estado CONSISTENTE: si no, la guarda 's_jpeg_enc!=NULL' daria por
         * listo el motor y jpeg_encoder_process desreferenciaria un buffer NULL. */
        if (s_jpeg_in)  { free(s_jpeg_in);  s_jpeg_in  = NULL; }
        if (s_jpeg_out) { free(s_jpeg_out); s_jpeg_out = NULL; }
        jpeg_del_encoder_engine(s_jpeg_enc);
        s_jpeg_enc = NULL;
        return false;
    }
    ESP_LOGI(TAG, "jpeg: motor HW listo (in=%u out=%u)", (unsigned)s_jpeg_in_sz, (unsigned)s_jpeg_out_sz);
    return true;
}

/* Codifica el ultimo thumbnail (BGR 964x546) a JPEG (recorte 960x544, RGB888).
 * Devuelve puntero al buffer PERSISTENTE s_jpeg_out (NO hacer free) y su tamano. */
bool camera_snapshot_jpeg(uint8_t **out, size_t *out_len)
{
    if (!camera_jpeg_init()) return false;
    int act = s_thumb_act;
    if (act < 0 || s_thumb[act] == NULL) return false;
    const uint8_t *src = s_thumb[act];   /* BGR, THUMB_W*3 por fila */

    /* Copiar el recorte 960x544 a la entrada RGB888 (swap B<->R). */
    for (int y = 0; y < JPEG_H; y++) {
        const uint8_t *s = &src[(uint32_t)y * THUMB_W * 3];
        uint8_t *d = &s_jpeg_in[(uint32_t)y * JPEG_W * 3];
        for (int x = 0; x < JPEG_W; x++) {
            d[x*3 + 0] = s[x*3 + 2];   /* R <- src.R (BGR[2]) */
            d[x*3 + 1] = s[x*3 + 1];   /* G */
            d[x*3 + 2] = s[x*3 + 0];   /* B <- src.B (BGR[0]) */
        }
    }

    jpeg_encode_cfg_t cfg = {
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,   /* equilibrio calidad/tamano (croma 4:2:2) */
        .image_quality = JPEG_QUALITY,
        .width         = JPEG_W,
        .height        = JPEG_H,
    };
    uint32_t jlen = 0;
    esp_err_t e = jpeg_encoder_process(s_jpeg_enc, &cfg, s_jpeg_in, s_jpeg_in_sz,
                                       s_jpeg_out, s_jpeg_out_sz, &jlen);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "jpeg: encode fallo: %s", esp_err_to_name(e));
        return false;
    }
    *out = s_jpeg_out;
    *out_len = jlen;
    return true;
}

/* ── Anillo de capturas de vigilancia en RAM (PSRAM) ─────────────────────────
 * Escribir las fotos a la SD durante la vigilancia NO es viable en esta placa: el
 * DMA de la camara + el sondeo SDIO del C6 saturan el controlador SDMMC compartido
 * (timeouts 0x107; probado a fondo). Solucion: guardar las ultimas N fotos JPEG en
 * PSRAM y servirlas por HTTP (/vigilancia). No se toca la SD -> cero conflicto. Se
 * pierden al reiniciar (encaja con "modo ausente ahora"). */
#define VIG_RING 8    /* 8 capturas basta para "quien entro"; ahorra ~0.5MB PSRAM */
typedef struct { uint8_t *buf; size_t len; time_t ts; uint32_t id; } vig_slot_t;
static vig_slot_t       s_vig[VIG_RING];
static int              s_vig_head = 0;        /* proxima posicion de escritura */
static uint32_t         s_vig_seq  = 0;        /* contador global -> id unico */
static SemaphoreHandle_t s_vig_mtx = NULL;

/* Guarda una copia del JPEG en el anillo (la mas antigua se libera). Llamada desde
 * la tarea de camara. Copia los bytes -> s_jpeg_out se puede reusar luego. */
static void camera_vig_store(const uint8_t *jpg, size_t len, time_t ts)
{
    if (!s_vig_mtx) { s_vig_mtx = xSemaphoreCreateMutex(); if (!s_vig_mtx) return; }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) { ESP_LOGW(TAG, "vig: sin PSRAM para la captura"); return; }
    memcpy(copy, jpg, len);
    xSemaphoreTake(s_vig_mtx, portMAX_DELAY);
    vig_slot_t *sl = &s_vig[s_vig_head];
    if (sl->buf) free(sl->buf);
    sl->buf = copy; sl->len = len; sl->ts = ts; sl->id = ++s_vig_seq;
    s_vig_head = (s_vig_head + 1) % VIG_RING;
    xSemaphoreGive(s_vig_mtx);
}

/* Lista las capturas (mas NUEVA primero). Rellena ids/ts/lens hasta max. -> count. */
int camera_vig_list(uint32_t *ids, time_t *ts, size_t *lens, int max)
{
    if (!s_vig_mtx) return 0;
    int n = 0;
    xSemaphoreTake(s_vig_mtx, portMAX_DELAY);
    for (int k = 0; k < VIG_RING && n < max; k++) {
        int idx = (s_vig_head - 1 - k + VIG_RING) % VIG_RING;
        if (s_vig[idx].buf) {
            if (ids)  ids[n]  = s_vig[idx].id;
            if (ts)   ts[n]   = s_vig[idx].ts;
            if (lens) lens[n] = s_vig[idx].len;
            n++;
        }
    }
    xSemaphoreGive(s_vig_mtx);
    return n;
}

/* Copia el JPEG de la captura 'id' a un buffer nuevo (PSRAM); el que llama hace
 * free(*out). false si ya no existe (rotada). */
bool camera_vig_fetch(uint32_t id, uint8_t **out, size_t *out_len)
{
    if (!s_vig_mtx) return false;
    bool ok = false;
    xSemaphoreTake(s_vig_mtx, portMAX_DELAY);
    for (int i = 0; i < VIG_RING; i++) {
        if (s_vig[i].buf && s_vig[i].id == id) {
            uint8_t *copy = heap_caps_malloc(s_vig[i].len, MALLOC_CAP_SPIRAM);
            if (copy) {
                memcpy(copy, s_vig[i].buf, s_vig[i].len);
                *out = copy; *out_len = s_vig[i].len; ok = true;
            }
            break;
        }
    }
    xSemaphoreGive(s_vig_mtx);
    return ok;
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

/* ── Vigilancia por movimiento (modo ausente) ───────────────────────────────
 * Rejilla de luma muestreada del frame; se compara con la anterior contando
 * celdas que cambian mas de MOT_CELL_DIFF (robusto al grano). Si bastantes
 * celdas cambian -> movimiento -> foto a SD (rate-limited por cooldown). */
#define MOT_GW            32
#define MOT_GH            18
/* Calibrado con datos reales (escena con pantalla apagada, brillo~17): el ruido en
 * reposo da maxdiff~5 y una persona moviendose da maxdiff 13-26. Umbral 10 separa
 * limpiamente (sobre el ruido, bajo el movimiento). 35 era inalcanzable -> mov=0. */
#define MOT_CELL_DIFF     10     /* diff por celda (0-255) que cuenta como cambio */
#define MOT_CELL_COUNT    4      /* nº de celdas cambiadas para declarar movimiento */
#define MOT_COOLDOWN_MS   4000   /* min entre fotos */
#define MOT_MAX_PHOTOS    300    /* tope por SESION de vigilancia (anti-runaway) */

static volatile bool s_surveillance = false;
static volatile bool s_mot_reset    = false;
static volatile bool s_ctrl_dirty   = true;   /* re-aplicar exposicion/ganancia en la tarea */
static volatile int  s_photo_count  = 0;      /* capturas de la sesion actual (la tarea lo usa) */

/* Cerrojo de bus camara<->SD. El DMA de la camara (GDMA) contiende con el
 * controlador SDMMC (SD + SDIO del C6); una escritura a SD que pille la ventana de
 * DMA activo se atasca con interrupciones bloqueadas >300ms -> INT WDT -> reinicio.
 * La tarea de camara RETIENE este mutex en su ventana de DMA activo y lo SUELTA en
 * su ventana ociosa; los escritores de SD (datalogger/battery_history/log) lo toman
 * antes de su I/O -> escriben solo cuando el GDMA esta parado. */
static SemaphoreHandle_t s_sd_bus = NULL;

/* Para escritores de SD externos: tomar/soltar el bus alrededor de su I/O a SD.
 * lock devuelve false si no lo consigue en timeout_ms (el que llama debe omitir la
 * escritura y reintentar luego). Si la camara no ha arrancado (mutex NULL) no hay
 * contencion -> permitir siempre. */
bool camera_sd_bus_lock(uint32_t timeout_ms)
{
    if (!s_sd_bus) return true;
    return xSemaphoreTake(s_sd_bus, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void camera_sd_bus_unlock(void)
{
    if (s_sd_bus) xSemaphoreGive(s_sd_bus);
}

void camera_set_surveillance(bool on)
{
    s_surveillance = on;
    s_no_temporal = on;   /* vigilancia: sin promediado temporal (mas nitido con movimiento) */
    s_mot_reset = true;   /* descartar el frame anterior para no disparar al entrar */
    s_ctrl_dirty = true;  /* aplicar exposicion/ganancia de vigilancia (mas brillo) o normal */
    if (on) s_photo_count = 0;  /* nueva sesion: el tope MOT_MAX_PHOTOS es POR sesion,
                                 * no acumulado de por vida (si no, dejaba de capturar) */
    ESP_LOGI(TAG, "vigilancia %s", on ? "ON (movimiento->foto)" : "OFF");
}

/* Arranca/para el stream RAW10. En modo A DEMANDA se ciclan para que el DMA de la
 * camara NO este siempre activo: el DMA continuo de la camara bloquea el bus DMA de
 * la SD -> timeouts 0x107 en lecturas/escrituras (root cause de "la SD no va con la
 * camara"). Entre frames el stream se para y la SD queda libre. */
static bool cam_stream_start(int fd, int type)
{
    for (int i = 0; i < CAM_STREAM_BUFS; i++) {
        struct v4l2_buffer qb = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        ioctl(fd, VIDIOC_QBUF, &qb);
    }
    return ioctl(fd, VIDIOC_STREAMON, &type) == 0;
}

/* Limpieza en caminos de error de la tarea de camara: libera los mmap V4L2 y los
 * thumbnails/accum en PSRAM (~6MB). Sin esto, un fallo de init fugaba esa memoria y
 * realimentaba la presion de PSRAM que dispara el assert del SDIO. */
static void cam_task_cleanup(int fd, uint8_t **buf, uint32_t *len, int nbuf)
{
    for (int i = 0; i < nbuf; i++)
        if (buf[i] && buf[i] != MAP_FAILED) munmap(buf[i], len[i]);
    if (s_thumb[0]) { free(s_thumb[0]); s_thumb[0] = NULL; }
    if (s_thumb[1]) { free(s_thumb[1]); s_thumb[1] = NULL; }
    if (s_accum)    { free(s_accum);    s_accum = NULL; }
    if (fd >= 0) close(fd);
}

/* Tarea de captura A DEMANDA: en modo normal coge una rafaga corta de frames cada
 * ~2s (mide luminosidad para auto-brillo + refresca thumbnail) y PARA el stream para
 * dejar la SD libre. En modo vigilancia (ausente) si streamea seguido para detectar
 * movimiento, parando el stream solo para guardar cada foto a SD. */
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
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) { cam_task_cleanup(fd, buf, len, i); vTaskDelete(NULL); return; }
        buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        len[i] = b.length;
        if (buf[i] == MAP_FAILED) { cam_task_cleanup(fd, buf, len, i); vTaskDelete(NULL); return; }
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    ESP_LOGI(TAG, "stream: modo A DEMANDA (no continuo) para no bloquear la SD");

    /* Thumbnails RGB (BGR) para el snapshot HTTP (doble buffer en PSRAM). Si falla
     * la reserva, seguimos solo con la luma. */
    s_thumb[0] = heap_caps_malloc(THUMB_W * THUMB_H * 3, MALLOC_CAP_SPIRAM);
    s_thumb[1] = heap_caps_malloc(THUMB_W * THUMB_H * 3, MALLOC_CAP_SPIRAM);
    /* Acumulador para promediado temporal (quita grano). Si falla, seguimos sin el. */
    s_accum = heap_caps_calloc(THUMB_W * THUMB_H * 3, sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    /* UN solo STREAMON. Ciclar STREAMON/STREAMOFF crashea el driver CSI de esp_video
     * (esp_cam_ctlr_csi_start -> dw_gdma set_src_addr sobre canal ya liberado -> Store
     * access fault). En su lugar: THROTTLE. Con 2 buffers, si consumimos despacio
     * (DQBUF/QBUF + sleep), los buffers se llenan y el GDMA de la camara se PARA solo
     * por contrapresion -> la SD queda libre entre frames. Mismo efecto, sin crash. */
    if (!cam_stream_start(fd, type)) {
        ESP_LOGE(TAG, "stream: STREAMON falla");
        cam_task_cleanup(fd, buf, len, CAM_STREAM_BUFS);
        vTaskDelete(NULL);
        return;
    }

    /* Que DQBUF NO bloquee infinito: timeout de 3s. Asi un cuelgue del CSI/sensor
     * hace que DQBUF retorne error (el bucle lo trata: vTaskDelay+continue -> resetea
     * el WDT) en vez de bloquear la tarea para siempre. El Task WDT (5s, PANIC) queda
     * como RESPALDO real para otros cuelgues, no como gatillo de un DQBUF normal. */
    struct timeval dqbuf_to = { .tv_sec = 3, .tv_usec = 0 };
    ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_to);

    /* Suscribir esta tarea al Task WDT (respaldo): si TODA la iteracion se cuelga
     * (no solo DQBUF), el cuelgue se detecta -> reinicio recuperable. Reset por
     * iteracion. Mantener CAM_IDLE_MS/CAM_SURV_IDLE_MS < 5s (TWDT timeout). */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK)   /* si fallara, el reset() seria no-op -> sin proteccion */
        ESP_LOGW(TAG, "stream: esp_task_wdt_add fallo (%s) - sin watchdog en la tarea",
                 esp_err_to_name(wdt_err));

    uint32_t ema = 0;
    bool first = true;

    /* Estado de vigilancia (persiste entre frames). */
    static uint8_t  prev_grid[MOT_GW * MOT_GH];
    static bool     have_prev = false;
    static int64_t  last_photo_us = 0;
    /* photo_count -> ahora s_photo_count (file-static), para que camera_set_surveillance
     * lo resetee al iniciar cada sesion (el tope es por sesion, no de por vida). */

    while (1) {
        esp_task_wdt_reset();   /* la tarea sigue viva (si DQBUF se cuelga, salta el WDT) */
        bool surv = s_surveillance;

        /* Tomar el bus para la ventana de DMA activo. Si un escritor de SD lo tiene,
         * esperar (con tope) -> si no se consigue, saltar el frame (GDMA sigue parado,
         * sin problema). Asi la captura y las escrituras a SD nunca solapan. */
        if (!camera_sd_bus_lock(1000)) {
            continue;   /* esp_task_wdt_reset al inicio mantiene viva la tarea */
        }

        /* Re-aplicar exposicion/ganancia al arranque y en cada cambio de modo:
         * vigilancia usa valores mas altos (escena oscura con pantalla apagada). */
        if (s_ctrl_dirty) {
            s_ctrl_dirty = false;
            cam_set_ctrl(fd, V4L2_CID_EXPOSURE, surv ? CAM_EXPOSURE_SURV : CAM_EXPOSURE, "exposure");
            cam_set_ctrl(fd, V4L2_CID_GAIN, surv ? CAM_GAIN_IDX_SURV : CAM_GAIN_IDX, "gain(idx)");
        }

        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
            camera_sd_bus_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (b.flags & V4L2_BUF_FLAG_DONE) {
            uint8_t luma = frame_luma(buf[b.index], b.bytesused);
            /* EMA suave: el brillo ambiente cambia lento. Una muestra cada ~2s ->
             * cte ~8 estabiliza en unos segundos. */
            if (first) { ema = luma; first = false; }
            else       { ema = (ema * 7 + luma) / 8; }
            s_luma = (uint8_t)ema;
            s_luma_valid = true;

            if (s_thumb[0] && s_thumb[1]) {
                int back = (s_thumb_act == 0) ? 1 : 0;
                downscale_rgb(buf[b.index], b.bytesused, s_thumb[back]);
                s_thumb_act = back;   /* publicar */
            }

            /* Vigilancia (modo ausente): deteccion de movimiento + foto a SD. */
            if (surv) {
                uint32_t stride = b.bytesused / SRC_H;
                uint8_t grid[MOT_GW * MOT_GH];
                for (int gy = 0; gy < MOT_GH; gy++) {
                    int y = gy * SRC_H / MOT_GH;
                    for (int gx = 0; gx < MOT_GW; gx++) {
                        int x = gx * SRC_W / MOT_GW;
                        grid[gy * MOT_GW + gx] = raw_px(buf[b.index], b.bytesused, stride, x, y);
                    }
                }
                if (s_mot_reset) { have_prev = false; s_mot_reset = false; }
                if (have_prev) {
                    int changed = 0, maxd = 0, sumv = 0;
                    for (int i = 0; i < MOT_GW * MOT_GH; i++) {
                        int d = (int)grid[i] - (int)prev_grid[i];
                        if (d < 0) d = -d;
                        if (d > MOT_CELL_DIFF) changed++;
                        if (d > maxd) maxd = d;
                        sumv += grid[i];
                    }
                    /* DIAGNOSTICO rico (cada frame de vigilancia ~1.5s): mov=celdas que
                     * cambian, maxdiff=mayor cambio de una celda (vs umbral MOT_CELL_DIFF),
                     * brillo=valor medio (si ~0 la escena esta oscura). */
                    ESP_LOGI(TAG, "vig: mov=%d(>=%d capta) maxdiff=%d(umbral %d) brillo=%d",
                             changed, MOT_CELL_COUNT, maxd, MOT_CELL_DIFF,
                             sumv / (MOT_GW * MOT_GH));
                    int64_t now = esp_timer_get_time();
                    if (changed >= MOT_CELL_COUNT &&
                        (now - last_photo_us) > (int64_t)MOT_COOLDOWN_MS * 1000 &&
                        s_photo_count < MOT_MAX_PHOTOS) {
                        last_photo_us = now;
                        /* Codificar el JPEG por HW (PSRAM) y guardarlo en el anillo en
                         * RAM. NO se escribe a la SD: el DMA camara+C6 la satura durante
                         * la vigilancia (probado). Se ven por HTTP en /vigilancia. */
                        uint8_t *jpg = NULL;
                        size_t jlen = 0;
                        if (camera_snapshot_jpeg(&jpg, &jlen)) {
                            camera_vig_store(jpg, jlen, time(NULL));
                            s_photo_count++;
                            ESP_LOGI(TAG, "vigilancia: movimiento (%d celdas) -> captura %d en RAM (%u B)",
                                     changed, s_photo_count, (unsigned)jlen);
                        }
                        /* TODO(video): grabacion H.264 de N seg aqui (esp_h264). */
                    }
                }
                memcpy(prev_grid, grid, sizeof(grid));
                have_prev = true;
            }
        }
        ioctl(fd, VIDIOC_QBUF, &b);

        /* Dejar que el GDMA acabe de rellenar el buffer recien encolado y se PARE por
         * contrapresion (ambos buffers llenos) ANTES de soltar el bus -> al soltarlo el
         * DMA de la camara ya no toca el SDMMC. */
        vTaskDelay(pdMS_TO_TICKS(80));
        camera_sd_bus_unlock();   /* ventana ociosa: los escritores de SD pueden ir */

        /* Resto del THROTTLE con el bus libre. Normal ~2s; vigilancia ~1.5s. */
        int idle = (surv ? CAM_SURV_IDLE_MS : CAM_IDLE_MS) - 80;
        if (idle > 0) vTaskDelay(pdMS_TO_TICKS(idle));
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

    /* Cerrojo de bus camara<->SD (creado ANTES de arrancar la tarea). */
    s_sd_bus = xSemaphoreCreateMutex();

    /* Tarea de streaming continuo: mide luminosidad (auto-brillo) y servira para
     * la vigilancia por movimiento. */
    xTaskCreate(camera_stream_task, "cam_stream", 6144, NULL, 4, NULL);

    return ESP_OK;
}
