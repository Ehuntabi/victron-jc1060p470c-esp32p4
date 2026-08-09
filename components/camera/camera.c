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
#include "driver/ppa.h"           /* acelerador de imagen del P4: reduce por HW */
#include "esp_cache.h"
#include "driver/jpeg_decode.h"   /* decoder JPEG por HW (galeria: fotos de vigilancia) */
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

/* Ya NO se fija exposicion ni ganancia: las lleva el control automatico del ISP,
 * que se reajusta en cada fotograma (tambien en vigilancia, con la pantalla
 * apagada). Antes habia dos juegos de valores fijos aqui. */

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

/* ── Miniatura en color a partir de la imagen YA REVELADA por el ISP ─────────
 * Antes aqui habia un revelado por SOFTWARE completo (debayer, balance de
 * blancos gray-world, auto-niveles por histograma, gamma y realce de
 * saturacion) sobre el RAW10 del sensor. Ya no hace falta: con el driver oficial
 * del OV02C10 y su perfil de ajuste, el ISP del P4 entrega RGB565 revelado, con
 * balance de blancos, correccion de color, reduccion de ruido y exposicion
 * automatica hechos por HARDWARE. Aqui solo queda reducir a la mitad.
 *
 * Lo que se gana: ~8 MB de PSRAM y el grueso del trabajo de CPU por fotograma,
 * ademas de mejor imagen (el ISP expone bien el contraluz, que era el punto
 * flaco del auto-niveles por software). */
#define SRC_W    1920
#define SRC_H    1080
/* Mitad exacta. La reduccion la hace el PPA (acelerador de imagen del P4), no la
 * CPU: lee el RGB565 del ISP y escribe RGB888 ya reducido. */
#define THUMB_W  960
#define THUMB_H  540
#define THUMB_SZ (((uint32_t)THUMB_W * THUMB_H * 3 + 63) & ~63u)   /* alineado a cache */
static uint8_t      *s_thumb[2]   = { NULL, NULL };   /* RGB888, 3 bytes/px */
static ppa_client_handle_t s_ppa  = NULL;
static volatile int  s_thumb_act  = -1;               /* -1 = aun sin frame */

/* Pixel RGB565 (2 bytes, little endian) del frame del ISP. */
static inline uint16_t rgb565_px(const uint8_t *p, uint32_t bytes, uint32_t stride, int x, int y)
{
    uint32_t off = (uint32_t)y * stride + (uint32_t)x * 2;
    return (off + 1 < bytes) ? (uint16_t)(p[off] | (p[off + 1] << 8)) : 0;
}

/* Brillo (0-255) de un pixel, para el auto-brillo y la deteccion de movimiento. */
static inline uint8_t luma_px(const uint8_t *p, uint32_t bytes, uint32_t stride, int x, int y)
{
    const uint16_t v = rgb565_px(p, bytes, stride, x, y);
    const int r = ((v >> 11) & 0x1f) << 3;
    const int g = ((v >> 5)  & 0x3f) << 2;
    const int b = (v & 0x1f) << 3;
    return (uint8_t)((r + 2 * g + b) / 4);
}

/* Luma media (0-255) del centro del frame, para el auto-brillo de la pantalla. */
static uint8_t frame_luma(const uint8_t *p, uint32_t bytes)
{
    uint32_t stride = bytes / SRC_H;
    if (stride == 0) return 0;
    uint64_t sum = 0;
    uint32_t cnt = 0;
    for (int y = SRC_H / 4; y < SRC_H * 3 / 4; y += 8)
        for (int x = SRC_W / 4; x < SRC_W * 3 / 4; x += 8) {
            sum += luma_px(p, bytes, stride, x, y);
            cnt++;
        }
    return cnt ? (uint8_t)(sum / cnt) : 0;
}

/* RGB565 1920x1080 -> RGB888 960x540 usando el PPA (acelerador de imagen del P4).
 * Antes esto lo hacia la CPU pixel a pixel; ahora solo prepara la orden y espera.
 * Devuelve false si el acelerador no esta disponible (se sigue sin miniatura). */
static bool downscale_rgb(const uint8_t *p, uint32_t bytes, uint8_t *dst)
{
    if (!s_ppa) {
        const ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&cfg, &s_ppa) != ESP_OK) {
            ESP_LOGW(TAG, "PPA no disponible: sin miniatura");
            return false;
        }
    }
    if (bytes < (uint32_t)SRC_W * SRC_H * 2) return false;

    /* Trocear en franjas horizontales en vez de pasar el frame ENTERO (4,1 MB)
     * al PPA de una tacada. El driver ya trocea su propio esp_cache_msync interno
     * en bloques de 128 KB (CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS, fix del
     * 04-ago), pero entre bloque y bloque solo suelta y vuelve a coger el mismo
     * spinlock en un bucle cerrado: NUNCA cede la CPU de verdad. Si el otro core
     * pierde la carrera por ese spinlock las suficientes veces seguidas, se queda
     * sin entrar en su propia seccion critica el tiempo que dispara el INT WDT
     * (visto el 09-ago en las tareas httpd y nimble_host). Un vTaskDelay(1) real
     * entre franjas SI fuerza un cambio de contexto y deja pasar al otro core. */
    #define THUMB_STRIPS 4
    _Static_assert(SRC_H % THUMB_STRIPS == 0, "SRC_H debe dividirse exacto");
    _Static_assert(THUMB_H % THUMB_STRIPS == 0, "THUMB_H debe dividirse exacto");
    const uint32_t strip_h_in  = SRC_H / THUMB_STRIPS;
    const uint32_t strip_h_out = THUMB_H / THUMB_STRIPS;

    for (int s = 0; s < THUMB_STRIPS; s++) {
        const ppa_srm_oper_config_t op = {
            .in = {
                .buffer         = p,
                .pic_w          = SRC_W,
                .pic_h          = SRC_H,
                .block_w        = SRC_W,
                .block_h        = strip_h_in,
                .block_offset_x = 0,
                .block_offset_y = (uint32_t)s * strip_h_in,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer         = dst,
                .buffer_size    = THUMB_SZ,
                .pic_w          = THUMB_W,
                .pic_h          = THUMB_H,
                .block_offset_x = 0,
                .block_offset_y = (uint32_t)s * strip_h_out,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB888,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x        = 0.5f,
            .scale_y        = 0.5f,
            .mode           = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) {
            ESP_LOGW(TAG, "PPA: fallo al reducir (franja %d/%d)", s + 1, THUMB_STRIPS);
            return false;
        }
        vTaskDelay(1);
    }
    #undef THUMB_STRIPS
    /* El PPA escribe por DMA: invalidar cache antes de leerlo desde la CPU. */
    esp_cache_msync(dst, THUMB_SZ, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* Estirado de niveles + realce de sombras, SOLO si la escena sale muy apagada.
     * De noche, y sobre todo en vigilancia (con la pantalla apagada), no hay luz:
     * el control automatico del ISP se queda al maximo y aun asi la foto sale casi
     * negra. Esto la hace visible.
     *
     * Es lo unico que sigue haciendo la CPU, y sobre la miniatura ya reducida
     * (0,5 Mpx) con una tabla de 256 entradas en una sola pasada. Usa una ganancia
     * comun a los tres canales: NO deshace el balance de blancos del ISP. */
    uint32_t hist[256] = {0};
    const uint32_t npx = (uint32_t)THUMB_W * THUMB_H;
    for (uint32_t i = 0; i < npx; i += 8) {
        const uint8_t *q = &dst[i * 3];             /* RGB888 */
        hist[(q[0] + 2 * q[1] + q[2]) / 4]++;
    }
    const uint32_t muestras = (npx + 7) / 8;
    uint32_t acc = 0;
    int lo = 0, hi = 255;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= muestras / 50) { lo = i; break; } }
    acc = 0;
    for (int i = 255; i >= 0; i--) { acc += hist[i]; if (acc >= muestras / 100) { hi = i; break; } }

    /* Margen: por debajo de 190 de recorrido util, la imagen esta apagada. */
    if (hi - lo < 190 && hi > lo) {
        const int escala = (255 * 256) / (hi - lo);        /* 8.8 fijo */
        const float realce = 0.55f + 0.45f * ((float)(hi - lo) / 190.0f);
        uint8_t lut[256];
        for (int i = 0; i < 256; i++) {
            int v = (i - lo) * escala >> 8;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lut[i] = (uint8_t)(powf((float)v / 255.0f, realce) * 255.0f + 0.5f);
        }
        for (uint32_t i = 0; i < npx * 3; i++) {
            dst[i] = lut[dst[i]];
        }
    }
    return true;
}

/* Suelo de PSRAM libre a preservar para los buffers DMA del SDIO del C6: si se
 * agota, su alloc hace assert -> reboot. Los que reservan en PSRAM (vigilancia)
 * comprueban este margen antes de pedir memoria (mejor no servir que tumbar WiFi). */
#define VIG_PSRAM_FLOOR  (1024 * 1024)   /* 1MB de reserva para el SDIO/sistema */

/* ── JPEG por hardware (ESP32-P4) para las fotos de vigilancia ───────────────
 * El BMP de 1.5MB chocaba con la camara en el bus de la SD (escritura larga).
 * El JPEG por HW codifica en PSRAM (no toca la SD) y deja una salida ~80KB ->
 * escritura corta como las del datalogger -> NO choca. Sigue el ejemplo oficial
 * esp-idf/examples/peripherals/jpeg/jpeg_encode. */
#define JPEG_W        960   /* el thumbnail ya es 960 (multiplo de 16) */
#define JPEG_H        528   /* recorte del thumbnail 540 -> 528 (multiplo de 16) */
/* 85: mejora sobre 80 sin disparar el tamano. 92+YUV444 daba ~500KB/foto -> el
 * tráfico del encoder (2D-DMA) + servir esas imagenes por WiFi saturaba el bus -> INT WDT. */
#define JPEG_QUALITY  78
/* 4:2:0 en vez de 4:2:2: la mitad de datos de color; el grano de color (el que
 * peor comprime) se promedia y casi desaparece. */
#define JPEG_SUBSAMPLING  JPEG_DOWN_SAMPLING_YUV420
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

/* Mutex del encoder JPEG (s_jpeg_enc/in/out). Protege contra uso concurrente desde
 * la tarea de camara (vigilancia) y el servidor HTTP (/snapshot). */
static SemaphoreHandle_t s_jpeg_mutex = NULL;

/* Decodifica un JPEG (p.ej. una foto de vigilancia leida de la SD) a un buffer
 * RGB565 en PSRAM, listo para un lv_img (mismo formato que el framebuffer). El que
 * llama libera *out con free(). Serializa el codec HW con s_jpeg_mutex (mismo
 * bloque que el encoder) y reutiliza un motor de decode persistente (creado en la
 * primera llamada). Devuelve el ancho (alineado a 16, el paso de fila) y el alto reales.
 * false si el header no parsea, falta memoria o el decode falla. */
bool camera_decode_jpeg_rgb565(const uint8_t *jpg, size_t len,
                               uint8_t **out, int *out_w, int *out_h)
{
    if (out) *out = NULL;
    if (!jpg || !out || !out_w || !out_h || len < 4) return false;

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK) return false;
    const int w  = (int)info.width;
    const int h  = (int)info.height;
    const int aw = (w + 15) & ~15;   /* el decoder trabaja en bloques de 16 */
    const int ah = (h + 15) & ~15;
    if (w <= 0 || h <= 0 || aw > 4096 || ah > 4096) return false;

    jpeg_decode_memory_alloc_cfg_t in_mc  = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_mc = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t in_sz = 0, out_sz = 0;
    uint8_t *in = jpeg_alloc_decoder_mem(len, &in_mc, &in_sz);
    if (!in) return false;
    memcpy(in, jpg, len);
    /* Decodificamos a RGB888 y empaquetamos a RGB565 nosotros: el empaquetado
     * RGB565 del propio HW daba un tinte violeta en los tonos oscuros. */
    uint8_t *ob = jpeg_alloc_decoder_mem((size_t)aw * ah * 3, &out_mc, &out_sz);
    if (!ob) { free(in); return false; }

    bool ok = false;
    if (!s_jpeg_mutex || xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        /* Motor de decode persistente (como el encoder s_jpeg_enc): crear/destruir
         * uno por imagen cargaba descriptores DMA en cada paso de la galeria. Se
         * crea en la primera llamada y se reutiliza, protegido por s_jpeg_mutex. */
        static jpeg_decoder_handle_t s_jpeg_dec = NULL;
        if (!s_jpeg_dec) {
            jpeg_decode_engine_cfg_t eng = { .timeout_ms = 1000 };
            if (jpeg_new_decoder_engine(&eng, &s_jpeg_dec) != ESP_OK) s_jpeg_dec = NULL;
        }
        if (s_jpeg_dec) {
            jpeg_decode_cfg_t dc = {
                .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
                .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
                .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
            };
            uint32_t osize = 0;
            if (jpeg_decoder_process(s_jpeg_dec, &dc, in, len, ob, out_sz, &osize) == ESP_OK)
                ok = true;
        }
        if (s_jpeg_mutex) xSemaphoreGive(s_jpeg_mutex);
    }
    free(in);
    if (!ok) { free(ob); return false; }

    /* RGB888 -> RGB565 (formato del framebuffer). El orden de byte del decoder HW
     * queda invertido respecto al del encoder (naranja<->azul en pantalla), asi
     * que leemos B,G,R: s[0]=B, s[2]=R. Corrige el swap R<->B en carrusel y
     * vigilancia (ambos pasan por aqui). Paso de fila = aw. */
    uint16_t *rgb = heap_caps_malloc((size_t)aw * h * 2, MALLOC_CAP_SPIRAM);
    if (!rgb) { free(ob); return false; }
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = ob + (size_t)y * aw * 3;
        uint16_t *d = rgb + (size_t)y * aw;
        for (int x = 0; x < aw; ++x) {
            uint8_t b = s[0], g = s[1], r = s[2];
            s += 3;
            d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    free(ob);

    *out   = (uint8_t *)rgb;
    *out_w = aw;   /* paso de fila = ancho alineado (== ancho real si ya es multiplo de 16) */
    *out_h = h;
    return true;
}

/* Codifica el ultimo thumbnail (BGR 960x540) a JPEG (recorte 960x528, RGB888).
 * THREAD-SAFE: serializa el encoder con mutex y devuelve una COPIA nueva en PSRAM
 * (el que llama hace free(*out)). false si no hay frame o falla. */
bool camera_snapshot_jpeg(uint8_t **out, size_t *out_len)
{
    if (s_jpeg_mutex) xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);
    bool ok = false;

    if (camera_jpeg_init()) {
        int act = s_thumb_act;
        if (act >= 0 && s_thumb[act] != NULL) {
            const uint8_t *src = s_thumb[act];   /* RGB888, THUMB_W*3 por fila */
            /* La miniatura ya viene en RGB888 del acelerador: copia directa de
             * filas, sin intercambiar canales (antes venia en BGR). */
            for (int y = 0; y < JPEG_H; y++) {
                memcpy(&s_jpeg_in[(uint32_t)y * JPEG_W * 3],
                       &src[(uint32_t)y * THUMB_W * 3],
                       (size_t)JPEG_W * 3);
            }
            jpeg_encode_cfg_t cfg = {
                .src_type      = JPEG_ENCODE_IN_FORMAT_RGB888,
                .sub_sample    = JPEG_SUBSAMPLING,
                .image_quality = JPEG_QUALITY,
                .width         = JPEG_W,
                .height        = JPEG_H,
            };
            uint32_t jlen = 0;
            esp_err_t e = jpeg_encoder_process(s_jpeg_enc, &cfg, s_jpeg_in, s_jpeg_in_sz,
                                               s_jpeg_out, s_jpeg_out_sz, &jlen);
            if (e == ESP_OK) {
                uint8_t *copy = heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM);
                if (copy) {
                    memcpy(copy, s_jpeg_out, jlen);
                    *out = copy; *out_len = jlen; ok = true;
                }
            } else {
                ESP_LOGW(TAG, "jpeg: encode fallo: %s", esp_err_to_name(e));
            }
        }
    }

    if (s_jpeg_mutex) xSemaphoreGive(s_jpeg_mutex);
    return ok;
}

/* Codifica un framebuffer RGB565 (p.ej. una captura de pantalla 1024x600) a JPEG
 * por HW. Reusa el motor del encoder (serializado con s_jpeg_mutex) con buffers
 * propios del tamano pedido. YUV422 -> exige ancho multiplo de 16 y alto multiplo
 * de 8 (1024x600 cumple). El que llama hace free(*out). false si no cumple el
 * tamano, falta memoria o falla el encoder. */
bool camera_encode_rgb565_jpeg(const uint16_t *rgb, int w, int h, int quality,
                               uint8_t **out, size_t *out_len)
{
    if (out) *out = NULL;
    if (!rgb || !out || !out_len) return false;
    if (w <= 0 || h <= 0 || (w & 15) || (h & 7)) return false;   /* YUV422: w%16, h%8 */

    bool ok = false;
    if (s_jpeg_mutex) xSemaphoreTake(s_jpeg_mutex, portMAX_DELAY);
    if (camera_jpeg_init()) {   /* solo necesitamos el motor s_jpeg_enc */
        jpeg_encode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
        size_t in_sz = 0, out_sz = 0;
        uint8_t *in = jpeg_alloc_encoder_mem((size_t)w * h * 3, &in_cfg, &in_sz);
        uint8_t *ob = jpeg_alloc_encoder_mem((size_t)w * h * 3 / 2, &out_cfg, &out_sz);
        if (in && ob) {
            const int n = w * h;
            /* OJO con el orden: el bufer de LVGL de ESTA pantalla viene con rojo
             * y azul intercambiados respecto al color real (el panel DSI va en
             * BGR y lo compensa al pintar, por eso en pantalla se ve bien). Al
             * volcarlo tal cual salia todo tenido: el fondo UI_COLOR_BG 0x06080C
             * (azulado) se guardaba como 0x21180F (marron) y las tarjetas azules
             * salian amarillentas.
             *
             * El codificador NO tiene la culpa: la camara le pasa RGB888 sin
             * tocar nada y sale bien (ver camera_snapshot_jpeg). Por eso el
             * cambio va aqui, en el unico camino que lee el bufer de LVGL, y no
             * en el encoder. Verificado contra dos colores conocidos:
             *   0x06080C -> (6,8,12)   y  0x37474F -> (55,71,79). 2026-07-26. */
            for (int i = 0; i < n; ++i) {      /* 565 del framebuffer -> RGB888 real */
                uint16_t p = rgb[i];
                in[i*3+0] = (uint8_t)((((p & 0x1F) * 255) + 15) / 31);          /* R real */
                in[i*3+1] = (uint8_t)(((((p >> 5)  & 0x3F) * 255) + 31) / 63);  /* G */
                in[i*3+2] = (uint8_t)(((((p >> 11) & 0x1F) * 255) + 15) / 31);  /* B real */
            }
            jpeg_encode_cfg_t cfg = {
                .src_type      = JPEG_ENCODE_IN_FORMAT_RGB888,
                .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
                .image_quality = quality,
                .width         = w,
                .height        = h,
            };
            uint32_t jlen = 0;
            esp_err_t e = jpeg_encoder_process(s_jpeg_enc, &cfg, in, in_sz, ob, out_sz, &jlen);
            if (e == ESP_OK && jlen) {
                uint8_t *copy = heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM);
                if (copy) { memcpy(copy, ob, jlen); *out = copy; *out_len = jlen; ok = true; }
            } else if (e != ESP_OK) {
                ESP_LOGW(TAG, "jpeg: encode %dx%d fallo: %s", w, h, esp_err_to_name(e));
            }
        }
        if (in) free(in);
        if (ob) free(ob);
    }
    if (s_jpeg_mutex) xSemaphoreGive(s_jpeg_mutex);
    return ok;
}

/* ── Anillo de capturas de vigilancia en RAM (PSRAM) ─────────────────────────
 * Buffer entre la captura (rapida) y la SD (lenta). Cada foto JPEG entra aqui,
 * se sirve por HTTP (/vigilancia) y el drenador (vig_sd_drain_task) la vuelca a
 * la SD "sin prisas" tomando camera_sd_bus_lock (escribe solo cuando el GDMA de
 * la camara esta parado; la SD ya esta en SPI3, fuera del bus SDMMC del C6). Al
 * guardar OK se libera el slot (sale de HTTP). Historicamente el volcado directo
 * a SD durante el streaming NO era viable (DMA camara+C6 sobre SDMMC compartido,
 * timeouts 0x107); ahora se sortea con SD-en-SPI3 + cerrojo de bus + volcado
 * diferido a baja prioridad. */
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
    if (!s_vig_mtx) return;   /* creado en camera_init (antes de las tareas); si no, sin vigilancia */
    /* M2: mismo SUELO de PSRAM que el snapshot (VIG_PSRAM_FLOOR). Si el anillo se llena
     * (SD caida -> no drena) una rafaga de movimiento podria agotar la PSRAM y tumbar el
     * SDIO/C6 (assert -> reboot). Mejor descartar la captura que tirar el WiFi. */
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < len + VIG_PSRAM_FLOOR) {
        ESP_LOGW(TAG, "vig: PSRAM baja, descarto captura (protejo el SDIO)");
        return;
    }
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
/* MOT_CELL_DIFF calibrado con datos reales (escena con pantalla apagada, brillo~17):
 * el ruido en reposo da maxdiff~5 y una persona moviendose da maxdiff 13-26. Umbral
 * 10 separa limpiamente (sobre el ruido, bajo el movimiento).
 *
 * MOT_CELL_COUNT recalibrado el 2026-08-09 con 300 fotos reales de una sesion de
 * autocaravana vacia y a oscuras (luz interior encendida y constante): el ruido de
 * sensor en poca luz da picos de diff aislados enormes (celda suelta, maxdiff hasta
 * 151) que con el umbral anterior (4 celdas) disparaban falsos positivos en rafaga
 * -> las 300 fotos de la sesion (tope MOT_MAX_PHOTOS) eran casi todas la misma
 * cocina vacia. Midiendo con la MISMA rejilla sobre pares de fotogramas reales:
 * ruido en reposo cambia como mucho 19 celdas de 576; una persona delante de la
 * camara cambia 296-381. 30 deja margen amplio a los dos lados (~6x sobre el pico
 * de ruido, ~10x por debajo del minimo de persona real). */
#define MOT_CELL_DIFF     10     /* diff por celda (0-255) que cuenta como cambio */
#define MOT_CELL_COUNT    30     /* nº de celdas cambiadas para declarar movimiento */
#define MOT_COOLDOWN_MS   4000   /* min entre fotos */
#define MOT_MAX_PHOTOS    300    /* tope por SESION de vigilancia (anti-runaway) */

/* Rafaga de calibracion: el control automatico de exposicion del ISP ajusta UN
 * paso por fotograma, y aqui vamos a 1 fotograma cada 1,5-2 s (throttle para no
 * bloquear la SD). Al arrancar o al cambiar de modo eso son minutos hasta que se
 * asienta -> las fotos salian oscuras. Con esta rafaga corta a ritmo normal se
 * calibra en ~1 s y luego se vuelve al ritmo lento. */
#define CAM_WARMUP_FRAMES  40
#define CAM_WARMUP_MS      25    /* ~30 fps durante la rafaga */
static volatile int s_warmup = CAM_WARMUP_FRAMES;   /* tambien al arrancar */

static volatile bool s_surveillance = false;
static volatile bool s_mot_reset    = false;
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

/* ── Drenador anillo PSRAM -> SD ("sin prisas") ──────────────────────────────
 * Escribe cada JPEG del anillo a /sdcard/vigilancia SIN parar el stream (parar
 * STREAMON crashea el CSI): toma camera_sd_bus_lock alrededor de la escritura,
 * como datalogger -> escribe en la ventana en que el GDMA de la camara esta
 * parado. El JPEG es pequeno (~50-100KB) -> escritura corta, no ahoga a LVGL.
 * Al guardar OK libera el slot del anillo. Tarea de baja prioridad. */
#define VIG_SD_DIR   "/sdcard/vigilancia"
#define VIG_SD_CHUNK (8 * 1024)   /* trozo pequeno: se SUELTA el bus entre trozos */

static bool vig_write_jpeg_sd(uint32_t id, time_t ts, const uint8_t *jpg, size_t len)
{
    char path[96];
    struct tm tmv;
    localtime_r(&ts, &tmv);
    size_t p = strftime(path, sizeof(path), VIG_SD_DIR "/%Y%m%d_%H%M%S", &tmv);
    snprintf(path + p, sizeof(path) - p, "_%03lu.jpg", (unsigned long)(id % 1000));

    /* Crear dir + abrir bajo el bus (I/O de metadatos). */
    if (!camera_sd_bus_lock(2000)) return false;
    struct stat stx;
    if (stat(VIG_SD_DIR, &stx) != 0) mkdir(VIG_SD_DIR, 0777);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    camera_sd_bus_unlock();
    if (fd < 0) { ESP_LOGW(TAG, "vig: no abre SD (%s)", path); return false; }

    /* Escribir en trozos pequenos SOLTANDO el bus entre cada uno: la camara (GDMA)
     * recupera su ventana y no se bloquean interrupciones >300ms -> sin INT WDT. Con
     * write() de bajo nivel (sin buffer de stdio) cada trozo cae de verdad en su
     * ventana con el bus tomado. Mismo patron seguro que el datalogger (writes cortos). */
    bool ok = true;
    for (size_t off = 0; off < len; off += VIG_SD_CHUNK) {
        size_t n = (len - off < VIG_SD_CHUNK) ? (len - off) : VIG_SD_CHUNK;
        if (!camera_sd_bus_lock(2000)) { ok = false; break; }
        ssize_t w = write(fd, jpg + off, n);
        camera_sd_bus_unlock();
        if (w != (ssize_t)n) { ok = false; break; }
        vTaskDelay(pdMS_TO_TICKS(20));   /* ceder a la camara entre trozos */
    }

    /* close() hace transaccion SD real (flush + entrada de dir): SIEMPRE bajo el bus,
     * reintentando (nunca rendirse) para no solapar la ventana GDMA de la camara -> INT
     * WDT. La tarea drain no esta suscrita al TWDT, asi que basta ceder con vTaskDelay. */
    while (!camera_sd_bus_lock(1000)) { vTaskDelay(1); }
    if (close(fd) != 0) ok = false;   /* el flush/f_sync real a la SD ocurre en close() */
    camera_sd_bus_unlock();
    if (!ok) {
        while (!camera_sd_bus_lock(1000)) { vTaskDelay(1); }
        unlink(path);
        camera_sd_bus_unlock();
    }

    if (ok) ESP_LOGI(TAG, "vig: guardada en SD %s (%u B, troceada)", path, (unsigned)len);
    else    ESP_LOGW(TAG, "vig: fallo guardando en SD (%s)", path);
    return ok;
}

static void vig_sd_drain_task(void *arg)
{
    for (;;) {
        uint8_t *copy = NULL; size_t clen = 0; uint32_t cid = 0; time_t cts = 0;

        /* Coger el pendiente mas ANTIGUO (menor id) y copiarlo fuera del anillo,
         * para no retener el mutex durante la escritura lenta a SD. */
        if (s_vig_mtx) {
            xSemaphoreTake(s_vig_mtx, portMAX_DELAY);
            int best = -1; uint32_t bestid = 0xFFFFFFFFu;
            for (int i = 0; i < VIG_RING; i++) {
                if (s_vig[i].buf && s_vig[i].id < bestid) { bestid = s_vig[i].id; best = i; }
            }
            if (best >= 0) {
                copy = heap_caps_malloc(s_vig[best].len, MALLOC_CAP_SPIRAM);
                if (copy) {
                    memcpy(copy, s_vig[best].buf, s_vig[best].len);
                    clen = s_vig[best].len; cid = s_vig[best].id; cts = s_vig[best].ts;
                }
            }
            xSemaphoreGive(s_vig_mtx);
        }

        if (!copy) { vTaskDelay(pdMS_TO_TICKS(1500)); continue; }  /* nada pendiente */

        bool ok = vig_write_jpeg_sd(cid, cts, copy, clen);
        free(copy);

        if (ok && s_vig_mtx) {
            /* Borrar del anillo (si una captura nueva no reciclo ya ese slot). */
            xSemaphoreTake(s_vig_mtx, portMAX_DELAY);
            for (int i = 0; i < VIG_RING; i++) {
                if (s_vig[i].buf && s_vig[i].id == cid) {
                    free(s_vig[i].buf); s_vig[i].buf = NULL; break;
                }
            }
            xSemaphoreGive(s_vig_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? 300 : 3000));  /* sin prisas; si falla, mas espera */
    }
}

void camera_set_surveillance(bool on)
{
    s_surveillance = on;
    s_mot_reset = true;   /* descartar el frame anterior para no disparar al entrar */
    s_warmup = CAM_WARMUP_FRAMES;   /* recalibrar la exposicion para la escena nueva */
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

    /* Pedir RGB565: la imagen YA REVELADA por el ISP (color, balance de blancos,
     * ruido y exposicion automatica, todo por hardware). Antes se pedia RAW10 y
     * se revelaba por software en downscale_rgb(), que costaba CPU y memoria y
     * exponia peor los contraluces. */
    struct v4l2_format fmt = { .type = type };
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
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
    s_thumb[0] = heap_caps_aligned_alloc(64, THUMB_SZ, MALLOC_CAP_SPIRAM);
    s_thumb[1] = heap_caps_aligned_alloc(64, THUMB_SZ, MALLOC_CAP_SPIRAM);
    /* Ya NO se reserva el acumulador de promediado temporal (eran 3,1 MB de PSRAM):
     * el ruido lo quita ahora el filtro del ISP por hardware. */

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
    int dqbuf_fails = 0;                 /* M3: DQBUF fallidos consecutivos */
    const int CAM_DQBUF_FAIL_LIMIT = 10; /* tras N seguidos, la camara esta muerta */

    while (1) {
        /* M3: alimentar el WDT SOLO si la camara responde. Si DQBUF falla N veces
         * seguidas (0 buffers en el driver) dejamos de resetearlo -> salta el TWDT y
         * reinicia. Sin esto el bucle giraria para siempre con la camara muerta EN
         * SILENCIO (auto-brillo congelado, vigilancia ciega, el WDT nunca salta). */
        if (dqbuf_fails < CAM_DQBUF_FAIL_LIMIT) esp_task_wdt_reset();
        bool surv = s_surveillance;

        /* NOTA: el GDMA de la camara solo se reactiva al QBUF (que libera un buffer);
         * mientras tenemos un buffer fuera (DQBUF->proceso) el GDMA esta PARADO por
         * contrapresion. Por eso NO tomamos el bus aqui: DQBUF, proceso, debayer y el
         * encode JPEG (todo PSRAM/SCCB, sin tocar el SDMMC) corren sin el cerrojo. El
         * bus solo se toma alrededor del QBUF+settle (abajo) -> ventana minima, no
         * bloquea la tarea esp_timer durante el encode. */

        /* NO se fijan exposicion ni ganancia a mano: las lleva el control
         * automatico del ISP, que se reajusta en cada fotograma. Ponerlas a pelo
         * peleaba con el y dejaba la imagen quemada o negra segun la escena. */

        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
            if (dqbuf_fails < CAM_DQBUF_FAIL_LIMIT) {
                if (++dqbuf_fails >= CAM_DQBUF_FAIL_LIMIT)
                    ESP_LOGE(TAG, "stream: DQBUF fallo %d veces -> camara muerta, dejo saltar el WDT", dqbuf_fails);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        dqbuf_fails = 0;
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
                if (downscale_rgb(buf[b.index], b.bytesused, s_thumb[back])) {
                    s_thumb_act = back;   /* publicar */
                }
            }

            /* Vigilancia (modo ausente): deteccion de movimiento + foto a SD. */
            if (surv) {
                uint32_t stride = b.bytesused / SRC_H;
                uint8_t grid[MOT_GW * MOT_GH];
                for (int gy = 0; gy < MOT_GH; gy++) {
                    int y = gy * SRC_H / MOT_GH;
                    for (int gx = 0; gx < MOT_GW; gx++) {
                        int x = gx * SRC_W / MOT_GW;
                        grid[gy * MOT_GW + gx] = luma_px(buf[b.index], b.bytesused, stride, x, y);
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
                         * RAM. Se sirve por HTTP (/vigilancia) y el drenador
                         * (vig_sd_drain_task) lo vuelca a la SD sin prisas, tomando el
                         * bus en la ventana en que el GDMA esta parado. */
                        uint8_t *jpg = NULL;
                        size_t jlen = 0;
                        if (camera_snapshot_jpeg(&jpg, &jlen)) {
                            camera_vig_store(jpg, jlen, time(NULL));
                            free(jpg);   /* ahora devuelve una copia -> liberarla */
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
        /* El QBUF reactiva el GDMA (libera un buffer). SOLO aqui se toca el bus que
         * comparte con la SD -> tomar el cerrojo justo alrededor del QBUF + el settle
         * (que el GDMA acabe de rellenar y pare por contrapresion). Ventana ~50ms ->
         * los escritores de SD (tarea esp_timer) esperan como mucho eso, no el encode.
         * El buffer b sigue retenido mientras esperamos -> GDMA parado, espera segura
         * (reseteando el WDT por si un escritor tarda). */
        /* Acotar reintentos (~5s) para no auto-alimentar el WDT eternamente si el
         * bus SD esta muerto de verdad. */
        int sd_lock_tries = 0;
        bool got_sd_lock = false;
        while (!(got_sd_lock = camera_sd_bus_lock(1000))) {
            if (++sd_lock_tries >= 5) {
                ESP_LOGW(TAG, "stream: bus SD no disponible tras %ds, reencolo SIN cerrojo",
                         sd_lock_tries);
                break;
            }
            esp_task_wdt_reset();
        }
        /* El QBUF se hace SIEMPRE, con cerrojo o sin el. Saltarlo perdia el buffer
         * de forma IRREVERSIBLE: con CAM_STREAM_BUFS=2, dos episodios de contencion
         * dejaban el driver sin buffers -> DQBUF fallaba para siempre -> a los 10
         * fallos se deja de alimentar el TWDT -> panic + reinicio. Una ventana
         * puntual de contencion con el GDMA es mucho mas barata que matar la
         * camara hasta el siguiente arranque. */
        if (ioctl(fd, VIDIOC_QBUF, &b) != 0)
            ESP_LOGW(TAG, "stream: QBUF fallo (idx=%lu, cerrojo=%d)",
                     (unsigned long)b.index, (int)got_sd_lock);
        if (got_sd_lock) {
            vTaskDelay(pdMS_TO_TICKS(50));   /* GDMA rellena el buffer y para */
            camera_sd_bus_unlock();
        }

        /* Resto del THROTTLE con el bus libre. Normal ~2s; vigilancia ~1.5s.
         * Durante la rafaga de calibracion vamos a ritmo normal para que el
         * control automatico del ISP se asiente. */
        if (s_warmup > 0) {
            s_warmup--;
            vTaskDelay(pdMS_TO_TICKS(CAM_WARMUP_MS));
        } else {
            int idle = (surv ? CAM_SURV_IDLE_MS : CAM_IDLE_MS) - 50;
            if (idle > 0) vTaskDelay(pdMS_TO_TICKS(idle));
        }
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

    /* Cerrojo de bus camara<->SD + mutex del encoder JPEG (ANTES de arrancar la tarea). */
    s_sd_bus = xSemaphoreCreateMutex();
    s_jpeg_mutex = xSemaphoreCreateMutex();
    s_vig_mtx = xSemaphoreCreateMutex();   /* anillo de vigilancia: crear ANTES de arrancar las tareas */
    if (!s_sd_bus || !s_jpeg_mutex || !s_vig_mtx) {
        ESP_LOGE(TAG, "no se pudieron crear los mutex de camara (sin heap) -> sin camara");
        return ESP_ERR_NO_MEM;
    }

    /* Tarea de streaming continuo: mide luminosidad (auto-brillo) y servira para
     * la vigilancia por movimiento. */
    xTaskCreate(camera_stream_task, "cam_stream", 6144, NULL, 4, NULL);

    /* Drenador del anillo de vigilancia -> SD (baja prioridad, "sin prisas").
     * Idle si no hay capturas pendientes; con SD no montada reintenta. */
    xTaskCreate(vig_sd_drain_task, "vig_drain", 6144, NULL, 2, NULL);

    return ESP_OK;
}
