#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"   /* camera_sd_bus_lock: turnarse el bus SDMMC con el stream */

static const char *TAG = "SCREENSHOT";

/* Detalle del ultimo fallo de escritura (para mostrarlo en la UI sin serie). */
static char s_last_err[48] = "";
const char *screenshot_last_error(void) { return s_last_err; }

/* Escribe un entero de 32 bits little-endian en buf. */
static void put32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)v;
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

esp_err_t screenshot_take_bmp(uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    /* Render COHERENTE de la pantalla activa con lv_snapshot: redibuja el arbol
     * de objetos entero a un buffer propio en PSRAM. A diferencia de leer
     * draw_buf->buf1 (que en render parcial / con doble buffer puede estar a
     * medio dibujar o contener el frame anterior), esto siempre da la pantalla
     * completa y actual. Formato TRUE_COLOR = RGB565 (LV_COLOR_DEPTH_16). */
    lv_img_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));

    if (!lvgl_port_lock(2000)) {
        ESP_LOGW(TAG, "No se pudo tomar el lock de LVGL");
        return ESP_FAIL;
    }
    lv_obj_t *scr = lv_scr_act();
    uint32_t need = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    uint8_t *snap = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM) : NULL;
    if (!snap) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Sin memoria para snapshot (%u bytes)", (unsigned)need);
        return ESP_ERR_NO_MEM;
    }
    lv_refr_now(NULL);  /* procesa layout/animaciones pendientes antes de la foto */
    lv_res_t r = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, snap, need);
    lvgl_port_unlock();
    if (r != LV_RES_OK) {
        heap_caps_free(snap);
        ESP_LOGE(TAG, "lv_snapshot fallo (%d)", (int)r);
        return ESP_FAIL;
    }

    const int w = (int)dsc.header.w;
    const int h = (int)dsc.header.h;
    const uint16_t *fb = (const uint16_t *)dsc.data;  /* RGB565, w*h pixeles */

    const int row_bytes = w * 3;
    const int pad = (4 - (row_bytes & 3)) & 3;  /* filas BMP alineadas a 4 */
    const uint32_t img_size = (uint32_t)(row_bytes + pad) * h;
    const size_t total = 54 + (size_t)img_size;

    uint8_t *bmp = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        heap_caps_free(snap);
        ESP_LOGE(TAG, "Sin memoria para BMP (%u bytes)", (unsigned)total);
        return ESP_ERR_NO_MEM;
    }

    /* Cabecera BMP de 54 bytes: BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40),
     * 24 bpp, sin compresion, filas de abajo a arriba. */
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    put32(&bmp[2], (uint32_t)total);  /* tamano total */
    put32(&bmp[10], 54);              /* offset a los datos */
    put32(&bmp[14], 40);              /* tamano cabecera info */
    put32(&bmp[18], (uint32_t)w);
    put32(&bmp[22], (uint32_t)h);
    bmp[26] = 1;                      /* planos */
    bmp[28] = 24;                     /* bits por pixel */
    put32(&bmp[34], img_size);

    /* RGB565 (little-endian, R en los bits altos) -> BGR888, fila a fila de
     * abajo a arriba (orden nativo del BMP). */
    uint8_t *p = bmp + 54;
    for (int y = h - 1; y >= 0; --y) {
        const uint16_t *src = fb + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            const uint16_t px = src[x];
            const uint8_t r5 = (px >> 11) & 0x1F;
            const uint8_t g6 = (px >> 5) & 0x3F;
            const uint8_t b5 = px & 0x1F;
            *p++ = (uint8_t)((b5 * 255 + 15) / 31);  /* B */
            *p++ = (uint8_t)((g6 * 255 + 31) / 63);  /* G */
            *p++ = (uint8_t)((r5 * 255 + 15) / 31);  /* R */
        }
        for (int k = 0; k < pad; ++k) *p++ = 0;
    }

    heap_caps_free(snap);
    *out_buf = bmp;
    *out_len = total;
    ESP_LOGI(TAG, "Captura en memoria: %dx%d (%u bytes)", w, h, (unsigned)total);
    return ESP_OK;
}

/* Escribe 'len' bytes a 'path' en la SD coordinando con la camara: crea el dir
 * padre, y escribe en trozos pequenos tomando/soltando camera_sd_bus_lock entre
 * cada uno (write() de bajo nivel). Mismo patron probado que la vigilancia: la
 * camara streamea en continuo y escribir sin coordinar choca con su GDMA (era el
 * 0/8); trozos pequenos + yield = la camara recupera su ventana y no se bloquean
 * interrupciones >300ms (sin INT WDT). No se puede parar el stream (STREAMOFF
 * crashea el CSI). Deja detalle del fallo en s_last_err. */
static esp_err_t write_buf_to_sd(const char *path, const uint8_t *buf, size_t len)
{
    /* Asegurar el directorio padre bajo el bus (I/O de metadatos). */
    char dir[96];
    const char *slash = strrchr(path, '/');
    if (slash && (size_t)(slash - path) < sizeof(dir)) {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = '\0';
        if (camera_sd_bus_lock(2000)) {
            struct stat stx;
            if (stat(dir, &stx) != 0) mkdir(dir, 0777);
            camera_sd_bus_unlock();
        }
    }

    if (!camera_sd_bus_lock(2000)) {
        snprintf(s_last_err, sizeof(s_last_err), "bus lock (open)");
        ESP_LOGE(TAG, "Bus SD ocupado (camara), no abro %s", path);
        return ESP_FAIL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int open_errno = errno;
    camera_sd_bus_unlock();
    if (fd < 0) {
        snprintf(s_last_err, sizeof(s_last_err), "open errno=%d", open_errno);
        ESP_LOGE(TAG, "No se pudo abrir %s (errno=%d)", path, open_errno);
        return ESP_FAIL;
    }

    bool ok = true;
    size_t wr = 0;
    const size_t CHUNK = 8 * 1024;   /* trozo pequeno: se suelta el bus entre trozos */
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        if (!camera_sd_bus_lock(2000)) { snprintf(s_last_err, sizeof(s_last_err), "bus lock (write)"); ok = false; break; }
        ssize_t w = write(fd, buf + off, n);
        int wr_errno = errno;
        camera_sd_bus_unlock();
        if (w != (ssize_t)n) {
            snprintf(s_last_err, sizeof(s_last_err), "write errno=%d w=%d", wr_errno, (int)w);
            ok = false; break;
        }
        wr += (size_t)w;
        vTaskDelay(pdMS_TO_TICKS(15));   /* ceder a la camara entre trozos */
    }

    /* close() hace la transaccion real (flush + entrada de dir): siempre bajo el
     * bus, reintentando para no solapar la ventana GDMA de la camara. */
    while (!camera_sd_bus_lock(1000)) { vTaskDelay(1); }
    int cerr = close(fd);
    int close_errno = errno;
    camera_sd_bus_unlock();
    if (cerr != 0) { snprintf(s_last_err, sizeof(s_last_err), "close errno=%d", close_errno); ok = false; }

    if (!ok || wr != len) {
        ESP_LOGE(TAG, "Escritura incompleta en %s (%u/%u, close=%d)",
                 path, (unsigned)wr, (unsigned)len, cerr);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Guardado: %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

esp_err_t screenshot_save_bmp(const char *path)
{
    uint8_t *bmp = NULL;
    size_t len = 0;
    esp_err_t e = screenshot_take_bmp(&bmp, &len);
    if (e != ESP_OK) return e;
    e = write_buf_to_sd(path, bmp, len);
    heap_caps_free(bmp);
    return e;
}

esp_err_t screenshot_save_jpeg(const char *path)
{
    /* Snapshot coherente de la pantalla activa a RGB565 (igual que
     * screenshot_take_bmp) y luego encode JPEG por HW (~10x mas pequeno que el
     * BMP -> escritura y lectura mucho mas rapidas). */
    lv_img_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lvgl_port_lock(2000)) {
        snprintf(s_last_err, sizeof(s_last_err), "lock LVGL");
        return ESP_FAIL;
    }
    lv_obj_t *scr = lv_scr_act();
    uint32_t need = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    uint8_t *snap = need ? heap_caps_malloc(need, MALLOC_CAP_SPIRAM) : NULL;
    if (!snap) {
        lvgl_port_unlock();
        snprintf(s_last_err, sizeof(s_last_err), "sin PSRAM snap");
        return ESP_ERR_NO_MEM;
    }
    lv_refr_now(NULL);
    lv_res_t r = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, snap, need);
    lvgl_port_unlock();
    if (r != LV_RES_OK) {
        heap_caps_free(snap);
        snprintf(s_last_err, sizeof(s_last_err), "snapshot %d", (int)r);
        return ESP_FAIL;
    }

    const int w = (int)dsc.header.w;
    const int h = (int)dsc.header.h;
    uint8_t *jpg = NULL;
    size_t jlen = 0;
    bool enc = camera_encode_rgb565_jpeg((const uint16_t *)dsc.data, w, h, 90, &jpg, &jlen);
    heap_caps_free(snap);
    if (!enc) {
        snprintf(s_last_err, sizeof(s_last_err), "encode jpeg %dx%d", w, h);
        ESP_LOGE(TAG, "encode jpeg %dx%d fallo", w, h);
        return ESP_FAIL;
    }

    esp_err_t e = write_buf_to_sd(path, jpg, jlen);
    heap_caps_free(jpg);
    return e;
}
