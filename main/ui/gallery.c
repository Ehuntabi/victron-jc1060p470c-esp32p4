#include "gallery.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"   /* camera_sd_bus_lock: leer SD coordinado con la camara */

static const char *TAG = "GALLERY";

/* Fuentes con glifos ES (definidas en main/fonts/, usadas por toda la UI). */
LV_FONT_DECLARE(lv_font_montserrat_20_es);
LV_FONT_DECLARE(lv_font_montserrat_24_es);

#define GAL_DIR       "/sdcard/screenshots"
#define GAL_MAX_FILES 64
#define GAL_NAME_LEN  48

/* Estado del overlay (protegido por el lock de LVGL: todo se toca en el hilo de
 * LVGL o bajo lvgl_port_lock desde la tarea de carga). */
static lv_obj_t   *s_screen   = NULL;   /* overlay raiz (NULL = cerrado) */
static lv_obj_t   *s_img      = NULL;   /* lv_img de la imagen actual */
static lv_obj_t   *s_lbl      = NULL;   /* nombre + contador */
static lv_obj_t   *s_lbl_hint = NULL;   /* "Cargando..." / errores */
static lv_img_dsc_t s_dsc;
static uint8_t    *s_img_buf  = NULL;   /* RGB565 decodificado (PSRAM) */
static char        s_files[GAL_MAX_FILES][GAL_NAME_LEN];
static int         s_count    = 0;
static int         s_idx      = 0;
static volatile bool s_loading = false;
static volatile uint32_t s_gen = 0;     /* generacion: invalida cargas viejas */

/* ── Listado del directorio (opendir bajo el bus de la camara) ─────────────── */
static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void gallery_scan(void)
{
    s_count = 0;
    if (!camera_sd_bus_lock(3000)) return;
    DIR *d = opendir(GAL_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && s_count < GAL_MAX_FILES) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            if (l > 4 && strcasecmp(n + l - 4, ".bmp") == 0) {
                strncpy(s_files[s_count], n, GAL_NAME_LEN - 1);
                s_files[s_count][GAL_NAME_LEN - 1] = '\0';
                s_count++;
            }
        }
        closedir(d);
    }
    camera_sd_bus_unlock();
    if (s_count > 1) qsort(s_files, s_count, GAL_NAME_LEN, cmp_names);
}

/* ── Lectura de un fichero de SD a un buffer PSRAM (troceada, soltando el bus
 *    entre trozos igual que la escritura -> coordina con el GDMA de la camara). */
static uint8_t *gallery_read_file(const char *path, size_t *out_len)
{
    struct stat st;
    if (!camera_sd_bus_lock(2000)) return NULL;
    int sr = stat(path, &st);
    camera_sd_bus_unlock();
    if (sr != 0 || st.st_size <= 0) return NULL;

    size_t len = (size_t)st.st_size;
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    if (!camera_sd_bus_lock(2000)) { heap_caps_free(buf); return NULL; }
    int fd = open(path, O_RDONLY);
    camera_sd_bus_unlock();
    if (fd < 0) { heap_caps_free(buf); return NULL; }

    size_t off = 0;
    bool ok = true;
    const size_t CHUNK = 16 * 1024;
    while (off < len) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        if (!camera_sd_bus_lock(2000)) { ok = false; break; }
        ssize_t r = read(fd, buf + off, n);
        camera_sd_bus_unlock();
        if (r <= 0) { ok = false; break; }
        off += (size_t)r;
        vTaskDelay(pdMS_TO_TICKS(8));   /* ceder a la camara entre trozos */
    }
    while (!camera_sd_bus_lock(1000)) { vTaskDelay(1); }
    close(fd);
    camera_sd_bus_unlock();

    if (!ok || off != len) { heap_caps_free(buf); return NULL; }
    *out_len = len;
    return buf;
}

/* ── Decodifica NUESTRO BMP (24-bit BGR, filas de abajo a arriba) a un buffer
 *    RGB565 top-down (formato del framebuffer). Inverso de screenshot.c. ────── */
static uint8_t *gallery_decode_bmp(const uint8_t *bmp, size_t len, int *out_w, int *out_h)
{
    if (len < 54 || bmp[0] != 'B' || bmp[1] != 'M') return NULL;
    int32_t w = (int32_t)((uint32_t)bmp[18] | ((uint32_t)bmp[19] << 8) |
                          ((uint32_t)bmp[20] << 16) | ((uint32_t)bmp[21] << 24));
    int32_t h = (int32_t)((uint32_t)bmp[22] | ((uint32_t)bmp[23] << 8) |
                          ((uint32_t)bmp[24] << 16) | ((uint32_t)bmp[25] << 24));
    int bpp = (int)((uint32_t)bmp[28] | ((uint32_t)bmp[29] << 8));
    if (w <= 0 || h <= 0 || bpp != 24 || w > 4096 || h > 4096) return NULL;

    const int row_bytes = w * 3;
    const int pad = (4 - (row_bytes & 3)) & 3;
    const size_t need = 54 + (size_t)(row_bytes + pad) * h;
    if (len < need) return NULL;

    uint16_t *out = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;

    for (int y = 0; y < h; ++y) {
        const uint8_t *src = bmp + 54 + (size_t)(h - 1 - y) * (row_bytes + pad); /* bottom-up */
        uint16_t *dst = out + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            uint8_t b = src[0], g = src[1], r = src[2];
            src += 3;
            dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    *out_w = w;
    *out_h = h;
    return (uint8_t *)out;
}

/* ── Tarea de carga: lee+decodifica FUERA del hilo de LVGL (para no congelar la
 *    UI ni disparar el watchdog), luego actualiza el lv_img bajo el lock. ───── */
static void gallery_load_task(void *arg)
{
    uint32_t gen = s_gen;
    int idx = s_idx;
    char path[96];
    snprintf(path, sizeof(path), GAL_DIR "/%s", s_files[idx]);

    size_t flen = 0;
    int w = 0, h = 0;
    uint8_t *img = NULL;
    uint8_t *raw = gallery_read_file(path, &flen);
    if (raw) {
        img = gallery_decode_bmp(raw, flen, &w, &h);
        heap_caps_free(raw);
    }

    if (lvgl_port_lock(2000)) {
        if (s_screen && gen == s_gen) {         /* sigue abierto y vigente */
            if (img) {
                lv_img_set_src(s_img, NULL);    /* soltar referencia anterior */
                if (s_img_buf) heap_caps_free(s_img_buf);
                s_img_buf = img;
                img = NULL;
                memset(&s_dsc, 0, sizeof(s_dsc));
                s_dsc.header.always_zero = 0;
                s_dsc.header.w  = w;
                s_dsc.header.h  = h;
                s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                s_dsc.data_size = (uint32_t)w * h * 2;
                s_dsc.data      = s_img_buf;
                lv_img_set_src(s_img, &s_dsc);
                lv_obj_center(s_img);
                lv_label_set_text_fmt(s_lbl, "%s   (%d/%d)", s_files[idx], idx + 1, s_count);
                lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(s_lbl_hint, "No se pudo cargar la imagen");
                lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lvgl_port_unlock();
    }
    if (img) heap_caps_free(img);   /* cerrado/invalidado mientras cargaba */
    s_loading = false;
    vTaskDelete(NULL);
}

/* Llamada desde el hilo de LVGL (callbacks). */
static void gallery_load_current(void)
{
    if (s_loading || s_count == 0) return;
    s_loading = true;
    lv_label_set_text(s_lbl_hint, "Cargando...");
    lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (xTaskCreate(gallery_load_task, "gal_load", 6144, NULL, 3, NULL) != pdPASS) {
        s_loading = false;
        ESP_LOGE(TAG, "No pude crear la tarea de carga");
    }
}

static void gallery_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0 || s_loading) return;
    s_idx = (s_idx - 1 + s_count) % s_count;
    gallery_load_current();
}

static void gallery_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0 || s_loading) return;
    s_idx = (s_idx + 1) % s_count;
    gallery_load_current();
}

static void gallery_close_cb(lv_event_t *e)
{
    (void)e;
    s_gen++;   /* invalida cualquier carga en vuelo */
    if (s_screen) {
        lv_obj_del(s_screen);   /* arrastra s_img/s_lbl/... */
        s_screen = NULL;
        s_img = NULL; s_lbl = NULL; s_lbl_hint = NULL;
    }
    if (s_img_buf) { heap_caps_free(s_img_buf); s_img_buf = NULL; }
}

void ui_gallery_open(void)
{
    if (s_screen) return;   /* ya abierto */
    gallery_scan();
    s_idx = 0;
    s_gen++;

    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(scr);
    s_screen = scr;

    s_img = lv_img_create(scr);
    lv_obj_center(s_img);

    s_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lbl, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_lbl, 6, 0);
    lv_label_set_text(s_lbl, "");
    lv_obj_align(s_lbl, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(s_lbl_hint, "");
    lv_obj_center(s_lbl_hint);

    lv_obj_t *btn_prev = lv_btn_create(scr);
    lv_obj_set_size(btn_prev, 90, 90);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_40, 0);
    lv_obj_t *l1 = lv_label_create(btn_prev);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(l1, LV_SYMBOL_LEFT);
    lv_obj_center(l1);
    lv_obj_add_event_cb(btn_prev, gallery_prev_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_next = lv_btn_create(scr);
    lv_obj_set_size(btn_next, 90, 90);
    lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_opa(btn_next, LV_OPA_40, 0);
    lv_obj_t *l2 = lv_label_create(btn_next);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(l2, LV_SYMBOL_RIGHT);
    lv_obj_center(l2);
    lv_obj_add_event_cb(btn_next, gallery_next_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lc = lv_label_create(btn_close);
    lv_label_set_text(lc, "Cerrar");
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_close, gallery_close_cb, LV_EVENT_CLICKED, NULL);

    if (s_count == 0) {
        lv_label_set_text(s_lbl_hint, "No hay capturas en la SD\n(usa el carrusel primero)");
    } else {
        gallery_load_current();
    }
}
