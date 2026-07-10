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
#include "camera.h"   /* camera_sd_bus_lock + camera_decode_jpeg_rgb565 */

static const char *TAG = "GALLERY";

/* Fuentes con glifos ES (definidas en main/fonts/, usadas por toda la UI). */
LV_FONT_DECLARE(lv_font_montserrat_20_es);
LV_FONT_DECLARE(lv_font_montserrat_24_es);

#define GAL_MAX_FILES 128
#define GAL_NAME_LEN  48

/* Las dos carpetas navegables. El indice s_folder selecciona una. */
typedef struct {
    const char *dir;
    const char *ext;    /* extension que se lista */
    const char *ext2;   /* segunda extension aceptada (NULL si ninguna) */
    const char *label;  /* texto del boton de carpeta */
    const char *empty;  /* mensaje cuando no hay ficheros */
} gal_folder_t;

/* Solo JPG: el carrusel ya guarda JPG (rapido). Las .bmp viejas que pudiera
 * quedar en la SD NO se listan (cargaban lento y ensuciaban el listado). El
 * decode sigue soportando .bmp por si acaso, pero no se enumeran. */
static const gal_folder_t FOLDERS[] = {
    { "/sdcard/screenshots", ".jpg", NULL, LV_SYMBOL_IMAGE " Carrusel",
      "No hay capturas del carrusel\n(usa el carrusel primero)" },
    { "/sdcard/vigilancia",  ".jpg", NULL, LV_SYMBOL_EYE_OPEN " Vigilancia",
      "No hay fotos de vigilancia" },
};
#define GAL_FOLDER_COUNT ((int)(sizeof(FOLDERS) / sizeof(FOLDERS[0])))

/* Estado del overlay. Todo se toca en el hilo de LVGL o bajo lvgl_port_lock. */
static lv_obj_t   *s_screen     = NULL;   /* overlay raiz (NULL = cerrado) */
static lv_obj_t   *s_img        = NULL;   /* lv_img de la imagen actual */
static lv_obj_t   *s_lbl        = NULL;   /* nombre + contador */
static lv_obj_t   *s_lbl_hint   = NULL;   /* "Cargando..." / errores / vacio */
static lv_obj_t   *s_lbl_folder = NULL;   /* texto del boton de carpeta */
static lv_img_dsc_t s_dsc;
static uint8_t    *s_img_buf    = NULL;   /* RGB565 decodificado (PSRAM) */
static char        s_files[GAL_MAX_FILES][GAL_NAME_LEN];
static int         s_count      = 0;
static int         s_idx        = 0;
static int         s_folder     = 0;
static volatile bool s_loading  = false;  /* hay una tarea de carga en marcha */
static volatile bool s_pending  = false;  /* llego otra peticion durante la carga */
static volatile uint32_t s_gen  = 0;      /* generacion: invalida cargas superadas */

/* ── Listado del directorio de la carpeta activa (bajo el bus de la camara) ─── */
static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void gallery_scan(void)
{
    s_count = 0;
    const gal_folder_t *f = &FOLDERS[s_folder];
    /* No retener el bus de la SD durante TODO el escaneo (congelaba el hilo
     * LVGL hasta 3 s): se toma solo alrededor de cada operacion y se suelta
     * entre entradas, igual que gallery_read_file entre trozos. */
    if (!camera_sd_bus_lock(2000)) return;
    DIR *d = opendir(f->dir);
    camera_sd_bus_unlock();
    if (!d) return;

    while (s_count < GAL_MAX_FILES) {
        if (!camera_sd_bus_lock(1000)) break;
        struct dirent *e = readdir(d);
        if (e == NULL) { camera_sd_bus_unlock(); break; }
        const char *n = e->d_name;
        size_t l = strlen(n);
        bool match = (l > 4) && (strcasecmp(n + l - 4, f->ext) == 0 ||
                                 (f->ext2 && strcasecmp(n + l - 4, f->ext2) == 0));
        if (match) {
            strncpy(s_files[s_count], n, GAL_NAME_LEN - 1);
            s_files[s_count][GAL_NAME_LEN - 1] = '\0';
            s_count++;
        }
        camera_sd_bus_unlock();
    }

    bool cl = camera_sd_bus_lock(1000);
    closedir(d);
    if (cl) camera_sd_bus_unlock();
    if (s_count > 1) qsort(s_files, s_count, GAL_NAME_LEN, cmp_names);
}

/* ── Lectura de un fichero de SD a PSRAM (troceada, soltando el bus entre trozos
 *    igual que la escritura -> coordina con el GDMA de la camara). ──────────── */
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

/* ── Decodifica NUESTRO BMP (24-bit BGR, filas de abajo a arriba) a RGB565
 *    top-down (inverso de screenshot.c). ─────────────────────────────────── */
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

/* ── Carga: lee+decodifica FUERA del hilo de LVGL, luego actualiza el lv_img bajo
 *    el lock. Solo aplica si la generacion sigue vigente (no fue superada). ─── */
static void gallery_start_load(void);   /* fwd */

static void gallery_load_task(void *arg)
{
    (void)arg;
    uint32_t gen    = s_gen;
    int      idx    = s_idx;
    int      folder = s_folder;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", FOLDERS[folder].dir, s_files[idx]);

    size_t flen = 0;
    int w = 0, h = 0;
    uint8_t *img = NULL;
    uint8_t *raw = gallery_read_file(path, &flen);
    if (raw) {
        size_t nl = strlen(s_files[idx]);
        bool is_bmp = (nl > 4) && (strcasecmp(s_files[idx] + nl - 4, ".bmp") == 0);
        if (is_bmp) img = gallery_decode_bmp(raw, flen, &w, &h);
        else        camera_decode_jpeg_rgb565(raw, flen, &img, &w, &h);
        heap_caps_free(raw);
    }

    if (lvgl_port_lock(2000)) {
        if (s_screen && gen == s_gen) {          /* sigue abierto y vigente */
            if (img) {
                lv_img_set_src(s_img, NULL);     /* soltar referencia anterior */
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
        /* Encadenar la siguiente peticion pendiente (cambio de imagen/carpeta
         * durante la carga) sin dejar tareas concurrentes pisandose. */
        s_loading = false;
        if (s_pending && s_screen) {
            s_pending = false;
            gallery_start_load();
        }
        lvgl_port_unlock();
    }
    if (img) heap_caps_free(img);   /* cerrado/superado mientras cargaba */
    vTaskDelete(NULL);
}

/* Arranca una tarea de carga (bajo lock de LVGL: toca s_lbl_hint). */
static void gallery_start_load(void)
{
    s_loading = true;
    lv_label_set_text(s_lbl_hint, "Cargando...");
    lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (xTaskCreate(gallery_load_task, "gal_load", 6144, NULL, 3, NULL) != pdPASS) {
        s_loading = false;
        ESP_LOGE(TAG, "No pude crear la tarea de carga");
    }
}

/* Pide cargar s_files[s_idx]. Si ya hay una carga, la encola (una sola). */
static void gallery_request_load(void)
{
    s_gen++;   /* invalida cualquier carga en vuelo que ya no sea la ultima */
    if (s_count == 0) return;
    if (s_loading) { s_pending = true; return; }
    gallery_start_load();
}

/* Muestra el estado de la carpeta activa tras (re)escanear. */
static void gallery_after_scan(void)
{
    s_idx = 0;
    if (s_lbl_folder) lv_label_set_text(s_lbl_folder, FOLDERS[s_folder].label);
    if (s_count == 0) {
        s_gen++;   /* invalida cargas en vuelo */
        lv_img_set_src(s_img, NULL);
        if (s_img_buf) { heap_caps_free(s_img_buf); s_img_buf = NULL; }
        lv_label_set_text(s_lbl, "");
        lv_label_set_text(s_lbl_hint, FOLDERS[s_folder].empty);
        lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        gallery_request_load();
    }
}

static void gallery_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    s_idx = (s_idx - 1 + s_count) % s_count;
    gallery_request_load();
}

static void gallery_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    s_idx = (s_idx + 1) % s_count;
    gallery_request_load();
}

static void gallery_folder_cb(lv_event_t *e)
{
    (void)e;
    s_folder = (s_folder + 1) % GAL_FOLDER_COUNT;
    gallery_scan();
    gallery_after_scan();
}

static void gallery_close_cb(lv_event_t *e)
{
    (void)e;
    s_gen++;   /* invalida cualquier carga en vuelo */
    if (s_screen) {
        lv_obj_del(s_screen);   /* arrastra s_img/s_lbl/... */
        s_screen = NULL;
        s_img = NULL; s_lbl = NULL; s_lbl_hint = NULL; s_lbl_folder = NULL;
    }
    if (s_img_buf) { heap_caps_free(s_img_buf); s_img_buf = NULL; }
}

/* Boton translucido cuadrado con un simbolo centrado. */
static lv_obj_t *make_nav_btn(lv_obj_t *parent, const char *sym,
                              lv_align_t align, int dx, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 90, 90);
    lv_obj_align(b, align, dx, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void ui_gallery_open(void)
{
    if (s_screen) return;   /* ya abierto */
    s_folder = 1;           /* por defecto: Vigilancia (no el carrusel) */
    gallery_scan();

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
    lv_obj_set_style_text_align(s_lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_hint, "");
    lv_obj_center(s_lbl_hint);

    make_nav_btn(scr, LV_SYMBOL_LEFT,  LV_ALIGN_LEFT_MID,   8, gallery_prev_cb);
    make_nav_btn(scr, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -8, gallery_next_cb);

    /* Boton de carpeta (Carrusel <-> Vigilancia) abajo al centro. */
    lv_obj_t *btn_folder = lv_btn_create(scr);
    lv_obj_set_height(btn_folder, 50);
    lv_obj_align(btn_folder, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(btn_folder, lv_color_hex(0x0288D1), 0);
    s_lbl_folder = lv_label_create(btn_folder);
    lv_obj_set_style_text_font(s_lbl_folder, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(s_lbl_folder, FOLDERS[s_folder].label);
    lv_obj_center(s_lbl_folder);
    lv_obj_add_event_cb(btn_folder, gallery_folder_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_close = lv_btn_create(scr);
    lv_obj_set_size(btn_close, 100, 50);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x882222), 0);
    lv_obj_t *lc = lv_label_create(btn_close);
    lv_label_set_text(lc, "Cerrar");
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_close, gallery_close_cb, LV_EVENT_CLICKED, NULL);

    gallery_after_scan();
}
