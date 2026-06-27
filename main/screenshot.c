#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "SCREENSHOT";

/* Escribe un entero de 32 bits little-endian en buf. */
static void put32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)v;
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

esp_err_t screenshot_save_bmp(const char *path)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp || !disp->driver || !disp->driver->draw_buf ||
        !disp->driver->draw_buf->buf1) {
        ESP_LOGW(TAG, "Sin framebuffer");
        return ESP_FAIL;
    }

    const int w = disp->driver->hor_res;
    const int h = disp->driver->ver_res;
    const size_t fb_bytes = (size_t)w * h * sizeof(lv_color_t);

    /* Copia del framebuffer en PSRAM bajo el lock de LVGL: asi no lo leemos
     * mientras la tarea LVGL esta dibujando, y soltamos el lock antes de la
     * escritura (lenta) en la SD. */
    uint16_t *fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM);
    if (!fb) {
        ESP_LOGE(TAG, "Sin memoria para copia (%u bytes)", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    if (lvgl_port_lock(1000)) {
        lv_refr_now(NULL);
        memcpy(fb, disp->driver->draw_buf->buf1, fb_bytes);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "No se pudo tomar el lock de LVGL");
        free(fb);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s", path);
        free(fb);
        return ESP_FAIL;
    }

    const int row_bytes = w * 3;
    const int pad = (4 - (row_bytes & 3)) & 3;  /* filas BMP alineadas a 4 */
    const uint32_t img_size = (uint32_t)(row_bytes + pad) * h;

    /* Cabecera BMP de 54 bytes: BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40),
     * 24 bpp, sin compresion, filas de abajo a arriba. */
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(&hdr[2], 54 + img_size);   /* tamano total */
    put32(&hdr[10], 54);             /* offset a los datos */
    put32(&hdr[14], 40);             /* tamano cabecera info */
    put32(&hdr[18], (uint32_t)w);
    put32(&hdr[22], (uint32_t)h);
    hdr[26] = 1;                     /* planos */
    hdr[28] = 24;                    /* bits por pixel */
    put32(&hdr[34], img_size);
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *row = malloc((size_t)row_bytes + pad);
    if (!row) {
        fclose(f);
        free(fb);
        return ESP_ERR_NO_MEM;
    }
    memset(row, 0, (size_t)row_bytes + pad);

    /* RGB565 (little-endian, R en los bits altos) -> BGR888, fila a fila de
     * abajo a arriba (orden nativo del BMP). */
    for (int y = h - 1; y >= 0; --y) {
        const uint16_t *src = fb + (size_t)y * w;
        uint8_t *dst = row;
        for (int x = 0; x < w; ++x) {
            const uint16_t p = src[x];
            const uint8_t r5 = (p >> 11) & 0x1F;
            const uint8_t g6 = (p >> 5) & 0x3F;
            const uint8_t b5 = p & 0x1F;
            *dst++ = (uint8_t)((b5 * 255 + 15) / 31);  /* B */
            *dst++ = (uint8_t)((g6 * 255 + 31) / 63);  /* G */
            *dst++ = (uint8_t)((r5 * 255 + 15) / 31);  /* R */
        }
        fwrite(row, 1, (size_t)row_bytes + pad, f);
    }

    free(row);
    fclose(f);
    free(fb);
    ESP_LOGI(TAG, "Captura guardada: %s (%dx%d)", path, w, h);
    return ESP_OK;
}
