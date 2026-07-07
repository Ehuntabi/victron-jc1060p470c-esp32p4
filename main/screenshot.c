#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SCREENSHOT";

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

esp_err_t screenshot_save_bmp(const char *path)
{
    uint8_t *bmp = NULL;
    size_t len = 0;
    esp_err_t e = screenshot_take_bmp(&bmp, &len);
    if (e != ESP_OK) return e;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s", path);
        heap_caps_free(bmp);
        return ESP_FAIL;
    }
    /* Escritura TROCEADA cediendo CPU: en SD-SPI (1-bit) escribir 1,8 MB de una
     * sola vez tarda >5s y no libera la CPU -> dispara el Task Watchdog (IDLE
     * hambriento) y reinicia a mitad del tour. Con trozos de 32 KB + un yield
     * entre ellos el idle respira y el WDT no salta. */
    size_t wr = 0;
    const size_t CHUNK = 32 * 1024;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        size_t w = fwrite(bmp + off, 1, n, f);
        wr += w;
        if (w != n) break;
        vTaskDelay(1);  /* cede CPU: alimenta el idle/WDT durante la escritura lenta */
    }
    int cerr = fclose(f);   /* el flush final a la SD ocurre aqui: si falla, el fichero quedo truncado */
    heap_caps_free(bmp);
    if (wr != len || cerr != 0) {
        ESP_LOGE(TAG, "Escritura incompleta en %s (%u/%u, close=%d)", path, (unsigned)wr, (unsigned)len, cerr);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Captura guardada: %s", path);
    return ESP_OK;
}
