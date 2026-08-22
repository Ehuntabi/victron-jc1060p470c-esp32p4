/* charts_svg.c — graficos SVG con auto-escala (frigo/bateria historicos) para
 * el portal web. Extraido de config_server.c (2026-08-08): esta pieza no
 * depende de nada del ciclo de vida del AP/auth mas alla de REQUIRE_AUTH, asi
 * que sale limpio a su propio fichero. */

#include "config_server_internal.h"
#include "charts_svg.h"
#include "camera.h"
#include "datalogger.h"
#include "battery_history.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static void get_today_csv_path(const char *subdir, char *out, size_t out_len)
{
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    if (tm_local.tm_year > 100) {
        snprintf(out, out_len, "/sdcard/%s/%04d-%02d-%02d.csv",
                 subdir,
                 (int)(tm_local.tm_year + 1900) & 0xFFFF,
                 (int)(tm_local.tm_mon + 1) & 0xFF,
                 (int)tm_local.tm_mday & 0xFF);
    } else {
        snprintf(out, out_len, "/sdcard/%s/boot.csv", subdir);
    }
}

/* Devuelve la ruta del CSV más reciente en /sdcard/{subdir}/ (por nombre,
 * que sigue formato YYYY-MM-DD.csv y ordena cronológicamente). out[0]=0 si
 * el directorio no existe o no contiene CSVs. */
static void get_latest_csv_path(const char *subdir, char *out, size_t out_len)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/sdcard/%s", subdir);
    /* Sin cerrojo no se toca la SD (pisar el GDMA de la camara = DMA timeout y
     * tarjeta pillada). Se comporta como "no hay CSV". 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) { out[0] = 0; return; }
    DIR *d = opendir(dirpath);
    if (!d) { camera_sd_bus_unlock(); out[0] = 0; return; }
    char latest[24] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t nl = strlen(name);
        if (nl < 5 || strcmp(name + nl - 4, ".csv") != 0) continue;
        if (strcmp(name, latest) > 0 && nl < sizeof(latest)) {
            strncpy(latest, name, sizeof(latest) - 1);
        }
    }
    closedir(d);
    camera_sd_bus_unlock();
    if (latest[0]) snprintf(out, out_len, "%s/%s", dirpath, latest);
    else out[0] = 0;
}

/* Lee fichero entero a buffer malloc'd. Devuelve NULL si falla. */
static char *read_file_to_buf(const char *path, size_t *out_len)
{
    struct stat st;
    /* Serializar el I/O de SD con el GDMA de la camara (no-op si no hay camara).
     * Solo unlock si el lock se consiguio: dar un mutex ajeno = assert de FreeRTOS. */
    /* Sin cerrojo no se toca la SD: se falla la lectura en vez de pisar el GDMA
     * de la camara. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) return NULL;
    bool have_stat = (stat(path, &st) == 0);
    camera_sd_bus_unlock();
    /* Limite 3MB: el CSV de bateria (4 fuentes cada ~10s) llega a ~1,5MB por
     * dia completo; 512KB (limite anterior) se quedaba corto a partir de un
     * tercio del dia y el endpoint devolvia vacio el resto -> "el log de
     * bateria sale vacio" reportado el 09-ago. 2026-08-09. */
    if (!have_stat || st.st_size <= 0 || st.st_size > 3 * 1024 * 1024) return NULL;

    if (!camera_sd_bus_lock(3000)) return NULL;
    FILE *f = fopen(path, "rb");
    camera_sd_bus_unlock();
    if (!f) return NULL;

    /* PSRAM: hasta 3MB no cabe con margen en la RAM interna (compartida con
     * pila LVGL, WiFi, camara...). Mismo criterio que el resto de buffers
     * grandes del proyecto (log_capture, thumbnails de camara). */
    char *buf = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
        fclose(f);
        camera_sd_bus_unlock();
        return NULL;
    }

    /* Leer en trozos de 4 KB soltando el cerrojo entre cada uno: un fread()
     * del fichero entero de una sola vez (hasta 3 MB) retenia el cerrojo
     * tanto tiempo que bloqueaba interrupciones >300ms -> INT WDT. Mismo
     * patron ya arreglado en tar_stream_dir (data_export_tar.c). 2026-08-09. */
    size_t n = 0;
    while (n < (size_t)st.st_size) {
        size_t want = (size_t)st.st_size - n;
        if (want > 4096) want = 4096;
        if (!camera_sd_bus_lock(3000)) break;
        size_t r = fread(buf + n, 1, want, f);
        camera_sd_bus_unlock();
        if (r == 0) break;
        n += r;
        if ((n % (8 * 4096)) == 0) vTaskDelay(1);   /* ceder CPU cada ~32KB */
    }
    while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
    fclose(f);
    camera_sd_bus_unlock();

    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}



/* Construye filas tabla y polyline SVG a partir del CSV.
   El CSV tiene header en la primera linea.
   Devuelve dos buffers malloc'd: rows_html y svg_inner. */
/* ── Helpers de gráfico SVG con auto-escala ──────────────────────── */
typedef struct {
    float *x;       /* posición horizontal */
    float *y;       /* valor central / avg */
    float *y_max;   /* opcional: máximo del bin (igual a y si no se usa) */
    float *y_min;   /* opcional: mínimo del bin */
    int n;
    int cap;
} ts_series_t;

static void ts_init(ts_series_t *s, int cap)
{
    s->x     = malloc(sizeof(float) * cap);
    s->y     = malloc(sizeof(float) * cap);
    s->y_max = malloc(sizeof(float) * cap);
    s->y_min = malloc(sizeof(float) * cap);
    if (!s->x || !s->y || !s->y_max || !s->y_min) {
        /* Fallo parcial de malloc: liberar lo reservado y dejar todo NULL
         * para que el guard OOM del caller (if !.x) lo detecte. */
        free(s->x); free(s->y); free(s->y_max); free(s->y_min);
        s->x = s->y = s->y_max = s->y_min = NULL;
    }
    s->n = 0;
    s->cap = cap;
}
static void ts_free(ts_series_t *s)
{
    free(s->x); free(s->y); free(s->y_max); free(s->y_min);
    s->x = s->y = s->y_max = s->y_min = NULL;
    s->n = 0;
}
static void ts_push(ts_series_t *s, float x, float y)
{
    if (s->n < s->cap) {
        s->x[s->n] = x; s->y[s->n] = y;
        s->y_max[s->n] = y; s->y_min[s->n] = y;
        s->n++;
    }
}
static void ts_push3(ts_series_t *s, float x, float avg, float mx, float mn)
{
    if (s->n < s->cap) {
        s->x[s->n] = x; s->y[s->n] = avg;
        s->y_max[s->n] = mx; s->y_min[s->n] = mn;
        s->n++;
    }
}

/* Calcular un buen "step" para la cuadrícula dado un rango */
static float nice_step(float range)
{
    if (range <= 0) return 1.0f;
    float pow10 = 1.0f;
    while (range / pow10 >= 10.0f) pow10 *= 10.0f;
    while (range / pow10 < 1.0f)   pow10 /= 10.0f;
    float n = range / pow10;
    if      (n < 1.5f) return 0.2f * pow10;
    else if (n < 3.0f) return 0.5f * pow10;
    else if (n < 7.0f) return 1.0f * pow10;
    else               return 2.0f * pow10;
}

/* Formatear número con precisión adaptativa */
static void fmt_axis(char *out, size_t cap, float v)
{
    float a = v < 0 ? -v : v;
    if (a >= 100)        snprintf(out, cap, "%.0f", v);
    else if (a >= 10)    snprintf(out, cap, "%.1f", v);
    else                 snprintf(out, cap, "%.2f", v);
}

/* Append seguro con formato a 'buf' (tamaño 'cap') a partir de 'sp'. Devuelve
 * el nuevo 'sp'. Sustituye al idiom inseguro 'sp += snprintf(buf+sp, cap-sp,..)':
 * snprintf devuelve lo que HABRIA escrito, no lo truncado, asi que al llenarse
 * el buffer 'sp' superaba 'cap' y el siguiente 'cap - sp' (size_t) hacia
 * underflow a ~SIZE_MAX -> escritura fuera del heap. Aqui clampamos: una vez
 * lleno se devuelve 'cap' y los appends siguientes no escriben nada. */
static size_t svg_vappend(char *buf, size_t cap, size_t sp, const char *fmt, ...)
{
    if (sp >= cap) return cap;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + sp, cap - sp, fmt, ap);
    va_end(ap);
    if (w < 0) return sp;
    if ((size_t)w >= cap - sp) return cap;  /* truncado: marcar buffer lleno */
    return sp + w;
}

static void build_frigo_html(const char *csv,
                             char **rows_html, char **svg_inner)
{
    (void)rows_html;
    *rows_html = NULL;

    size_t svg_cap = 32 * 1024;
    char *svg = malloc(svg_cap);
    if (!svg) { *svg_inner = NULL; return; }
    svg[0] = 0;
    size_t sp = 0;

    /* Contar líneas y reservar series */
    const char *p = csv;
    const char *nl = strchr(p, '\n');
    if (nl) p = nl + 1;
    int total = 0;
    for (const char *q = p; *q; q++) if (*q == '\n') total++;
    if (total < 1) total = 1;
    int cap = total + 4;

    ts_series_t s_aletas = {0}, s_cong = {0}, s_ext = {0}, s_fan = {0};
    ts_init(&s_aletas, cap);
    ts_init(&s_cong, cap);
    ts_init(&s_ext, cap);
    ts_init(&s_fan, cap);
    if (!s_aletas.x || !s_cong.x || !s_ext.x || !s_fan.x) {
        ts_free(&s_aletas); ts_free(&s_cong); ts_free(&s_ext); ts_free(&s_fan);
        free(svg); *svg_inner = NULL; return;
    }

    int idx = 0;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len < 5) { if (!line_end) break; p = line_end + 1; continue; }
        char buf[160] = {0};
        size_t cp = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, p, cp); buf[cp] = 0;
        char *fields[5] = {0};
        int fi = 0;
        if (cp > 0) fields[fi++] = buf;
        for (size_t i = 0; i < cp && fi < 5; ++i) {
            if (buf[i] == ',') { buf[i] = 0; if (fi < 5) fields[fi++] = &buf[i + 1]; }
        }
        if (fi >= 5) {
            float ta = (strcmp(fields[1], "---") == 0) ? -200.0f : atof(fields[1]);
            float tc = (strcmp(fields[2], "---") == 0) ? -200.0f : atof(fields[2]);
            float te = (strcmp(fields[3], "---") == 0) ? -200.0f : atof(fields[3]);
            int   fp = atoi(fields[4]);
            float xpos = (float)idx;
            if (ta > -120.0f) ts_push(&s_aletas, xpos, ta);
            if (tc > -120.0f) ts_push(&s_cong, xpos, tc);
            if (te > -120.0f) ts_push(&s_ext, xpos, te);
            ts_push(&s_fan, xpos, (float)fp);
        }
        idx++;
        if (!line_end) break;
        p = line_end + 1;
    }

    /* Calcular min/max de temperatura entre las 3 series */
    float t_min = 1e30f, t_max = -1e30f;
    ts_series_t *ts_temps[3] = { &s_aletas, &s_cong, &s_ext };
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < ts_temps[k]->n; i++) {
            float v = ts_temps[k]->y[i];
            if (v < t_min) t_min = v;
            if (v > t_max) t_max = v;
        }
    }
    if (t_min > t_max) { t_min = -10.0f; t_max = 10.0f; }
    /* Margen 10 % a cada lado, mínimo 2 */
    float margin = (t_max - t_min) * 0.10f;
    if (margin < 2.0f) margin = 2.0f;
    t_min -= margin; t_max += margin;
    float t_range = t_max - t_min;
    if (t_range < 1.0f) { t_max += 0.5f; t_min -= 0.5f; t_range = t_max - t_min; }

    /* Layout SVG */
    const int W = 800, H = 360;
    const int pad_l = 60, pad_r = 60, pad_t = 16, pad_b = 30;
    const int gw = W - pad_l - pad_r;
    const int gh = H - pad_t - pad_b;
    #define X_FRIGO(xv) (pad_l + (int)((float)gw * (xv) / (float)((total > 1) ? (total - 1) : 1)))
    #define Y_TEMP_AS(t) (pad_t + (int)((float)gh * (t_max - (t)) / t_range))
    #define Y_FAN_AS(f)  (pad_t + (int)((float)gh * (100.0f - (f)) / 100.0f))

    /* Cabecera SVG + grid */
    sp = svg_vappend(svg, svg_cap, sp,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' width='100%%' style='max-width:100%%'>"
        "<rect x='0' y='0' width='%d' height='%d' fill='#111' rx='8'/>",
        W, H, W, H);
    /* Gridlines de temperatura con auto-step */
    float step = nice_step(t_range);
    /* Empezar en múltiplo de step <= t_min */
    float start = (float)((int)(t_min / step) - 1) * step;
    char num[16];
    for (float v = start; v <= t_max + step; v += step) {
        if (v < t_min - step * 0.01f) continue;
        int yy = Y_TEMP_AS(v);
        if (yy < pad_t || yy > H - pad_b) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#222'/>",
            pad_l, yy, W - pad_r, yy);
        fmt_axis(num, sizeof(num), v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#888' font-size='12' text-anchor='end'>%s°</text>",
            pad_l - 4, yy + 4, num);
    }
    /* Eje derecho fan 0/50/100 */
    for (int v = 0; v <= 100; v += 25) {
        int yy = Y_FAN_AS(v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#FFAA00' font-size='11'>%d%%</text>",
            W - pad_r + 4, yy + 4, v);
    }
    /* Ejes principales */
    sp = svg_vappend(svg, svg_cap, sp,
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>"
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>",
        pad_l, pad_t, pad_l, H - pad_b,
        pad_l, H - pad_b, W - pad_r, H - pad_b);

    /* Polylines: dibujar cada serie */
    static const char *colors[4]  = { "#00BFFF", "#FF4444", "#44FF44", "#FFAA00" };
    ts_series_t *all[4]            = { &s_aletas, &s_cong, &s_ext, &s_fan };
    for (int k = 0; k < 4; k++) {
        if (all[k]->n == 0) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<polyline fill='none' stroke='%s' stroke-width='2' points='", colors[k]);
        for (int i = 0; i < all[k]->n; i++) {
            int x = X_FRIGO(all[k]->x[i]);
            int y = (k == 3) ? Y_FAN_AS(all[k]->y[i]) : Y_TEMP_AS(all[k]->y[i]);
            if (sp + 24 >= svg_cap) break;
            sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
        }
        sp = svg_vappend(svg, svg_cap, sp, "'/>");
    }
    sp = svg_vappend(svg, svg_cap, sp, "</svg>");

    ts_free(&s_aletas); ts_free(&s_cong); ts_free(&s_ext); ts_free(&s_fan);
    *svg_inner = svg;
}

esp_err_t handle_data_frigo(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* Intentar SD: primero el CSV de hoy, luego el más reciente */
    char path[96];
    get_today_csv_path("frigo", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) {
        get_latest_csv_path("frigo", path, sizeof path);
        if (path[0]) csv = read_file_to_buf(path, &csv_len);
    }
    bool from_sd = (csv != NULL);

    /* Fallback RAM */
    if (!csv) {
        csv = datalogger_get_csv();
    }
    if (!csv) {
        httpd_resp_send_chunk(req,
            "<!DOCTYPE html><html><head>", -1);
        httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
        httpd_resp_send_chunk(req,
            "</head><body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
            "<h2>Sin datos</h2><p>No hay datos disponibles ni en SD ni en RAM.</p>"
            "<a href='/data' style='color:#00BFFF'>Volver</a></body></html>", -1);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char *rows_html = NULL, *svg_inner = NULL;
    build_frigo_html(csv, &rows_html, &svg_inner);

    /* Construir respuesta por chunks */
    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Frigo</title>", -1);
    httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
    httpd_resp_send_chunk(req,
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:10px}"
        "h2{text-align:center}"
        ".legend{text-align:center;margin:8px 0;font-size:14px}"
        ".legend span{display:inline-block;margin:0 10px}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;vertical-align:middle;margin-right:4px}"
        "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:13px}"
        "th,td{padding:6px;border-bottom:1px solid #333;text-align:center}"
        "th{background:#222}"
        ".bar{text-align:center;margin:10px 0}"
        ".bar a{color:#00BFFF;text-decoration:none;margin:0 8px}"
        "</style></head><body>"
        "<h2>FRIGO</h2>"
        "<div class='bar'><a href='/data'>&larr; Datos</a> <a href='/data/frigo.csv'>Descargar CSV (hoy)</a> <a href='/data/frigo.tar'>Descargar todo (.tar)</a></div>"
        "<div class='legend'>"
        "<span><i class='dot' style='background:#00BFFF'></i>Aletas</span>"
        "<span><i class='dot' style='background:#FF4444'></i>Congelador</span>"
        "<span><i class='dot' style='background:#44FF44'></i>Exterior</span>"
        "<span><i class='dot' style='background:#FFAA00'></i>Fan%</span>"
        "</div>", -1);

    if (svg_inner) httpd_resp_send_chunk(req, svg_inner, -1);

    /* Indicador origen */
    httpd_resp_send_chunk(req,
        from_sd ? "<p style='text-align:center;color:#888;font-size:12px'>Origen: SD</p>"
                : "<p style='text-align:center;color:#888;font-size:12px'>Origen: RAM</p>", -1);

    httpd_resp_send_chunk(req, "</body></html>", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    free(csv);
    free(rows_html);
    free(svg_inner);
    return ESP_OK;
}

esp_err_t handle_data_frigo_csv(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[64];
    get_today_csv_path("frigo", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) csv = datalogger_get_csv();
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=frigo.csv");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        httpd_resp_sendstr(req, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct\n");
    }
    return ESP_OK;
}


/* CSV de comparacion NE185 (bytes crudos) vs SmartShunt. Solo descarga: no hay
 * pagina ni grafica, es un log de diagnostico (ver ne185_vlog.h). */
esp_err_t handle_data_ne185v_csv(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[64];
    get_today_csv_path("ne185v", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=ne185v.csv");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        /* Aun sin muestras volcadas hoy: devolver solo la cabecera. */
        httpd_resp_sendstr(req, "timestamp,ne_raw_serv,ne_raw_mot,ne_fresh,"
                                "shunt_centivolts,shunt_fresh\n");
    }
    return ESP_OK;
}


/* Construir HTML+SVG bateria.
   CSV bateria: timestamp,source,milli_amps */
static void build_bateria_html(const char *csv,
                               char **rows_html, char **svg_inner)
{
    (void)rows_html;
    *rows_html = NULL;

    /* Buffer SVG amplio para soportar muchos puntos (8640/24h × 4 series) */
    size_t svg_cap = 192 * 1024;
    char *svg = malloc(svg_cap);
    if (!svg) { *svg_inner = NULL; return; }
    svg[0] = 0;
    size_t sp = 0;

    /* Saltar header */
    const char *p = csv;
    const char *nl = strchr(p, '\n');
    if (nl) p = nl + 1;

    /* Contar líneas */
    int total = 0;
    for (const char *q = p; *q; q++) if (*q == '\n') total++;
    if (total < 1) total = 1;
    int cap = total + 4;

    /* Una serie por fuente */
    ts_series_t s_src[BH_SRC_COUNT] = {0};
    for (int i = 0; i < BH_SRC_COUNT; i++) ts_init(&s_src[i], cap);
    for (int i = 0; i < BH_SRC_COUNT; i++) if (!s_src[i].x) {
        for (int j = 0; j < BH_SRC_COUNT; j++) ts_free(&s_src[j]);
        free(svg); *svg_inner = NULL; return;
    }

    int idx = 0;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len < 5) { if (!line_end) break; p = line_end + 1; continue; }
        char buf[160] = {0};
        size_t cp = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, p, cp); buf[cp] = 0;
        /* CSV nuevo: ts,src,avg,max,min — CSV antiguo: ts,src,milli */
        char *fields[5] = {0};
        int fi = 0;
        if (cp > 0) fields[fi++] = buf;
        for (size_t i = 0; i < cp && fi < 5; ++i) {
            if (buf[i] == ',') { buf[i] = 0; if (fi < 5) fields[fi++] = &buf[i + 1]; }
        }
        if (fi >= 3) {
            const char *src = fields[1];
            float avg = atoi(fields[2]) / 1000.0f;
            float mx  = (fi >= 5) ? atoi(fields[3]) / 1000.0f : avg;
            float mn  = (fi >= 5) ? atoi(fields[4]) / 1000.0f : avg;
            int si = -1;
            for (int k = 0; k < BH_SRC_COUNT; ++k) {
                if (strcmp(src, battery_history_source_name((bh_source_t)k)) == 0) {
                    si = k; break;
                }
            }
            if (si >= 0) ts_push3(&s_src[si], (float)idx, avg, mx, mn);
        }
        idx++;
        if (!line_end) break;
        p = line_end + 1;
    }

    /* Auto-escala usando max/min reales (no solo avg) */
    float a_min = 1e30f, a_max = -1e30f;
    for (int k = 0; k < BH_SRC_COUNT; k++) {
        for (int i = 0; i < s_src[k].n; i++) {
            if (s_src[k].y_max[i] > a_max) a_max = s_src[k].y_max[i];
            if (s_src[k].y_min[i] < a_min) a_min = s_src[k].y_min[i];
        }
    }
    if (a_min > a_max) { a_min = -1.0f; a_max = 1.0f; }
    if (a_min > 0) a_min = 0;
    if (a_max < 0) a_max = 0;
    float margin = (a_max - a_min) * 0.10f;
    if (margin < 0.5f) margin = 0.5f;
    a_min -= margin; a_max += margin;
    float a_range = a_max - a_min;
    if (a_range < 1.0f) { a_max += 0.5f; a_min -= 0.5f; a_range = a_max - a_min; }

    const int W = 800, H = 360;
    const int pad_l = 60, pad_r = 20, pad_t = 16, pad_b = 30;
    const int gw = W - pad_l - pad_r;
    const int gh = H - pad_t - pad_b;
    #define X_BAT_AS(xv) (pad_l + (int)((float)gw * (xv) / (float)((total > 1) ? (total - 1) : 1)))
    #define Y_BAT_AS(a)  (pad_t + (int)((float)gh * (a_max - (a)) / a_range))

    sp = svg_vappend(svg, svg_cap, sp,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' width='100%%' style='max-width:100%%'>"
        "<rect x='0' y='0' width='%d' height='%d' fill='#111' rx='8'/>",
        W, H, W, H);
    /* Gridlines auto */
    float step = nice_step(a_range);
    float start = (float)((int)(a_min / step) - 1) * step;
    char num[16];
    for (float v = start; v <= a_max + step; v += step) {
        if (v < a_min - step * 0.01f) continue;
        int yy = Y_BAT_AS(v);
        if (yy < pad_t || yy > H - pad_b) continue;
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#222'/>",
            pad_l, yy, W - pad_r, yy);
        fmt_axis(num, sizeof(num), v);
        sp = svg_vappend(svg, svg_cap, sp,
            "<text x='%d' y='%d' fill='#888' font-size='12' text-anchor='end'>%s A</text>",
            pad_l - 4, yy + 4, num);
    }
    /* Eje cero destacado */
    if (a_min < 0 && a_max > 0) {
        int y0 = Y_BAT_AS(0);
        sp = svg_vappend(svg, svg_cap, sp,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#666' stroke-dasharray='4,4'/>",
            pad_l, y0, W - pad_r, y0);
    }
    /* Ejes */
    sp = svg_vappend(svg, svg_cap, sp,
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>"
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#555'/>",
        pad_l, pad_t, pad_l, H - pad_b,
        pad_l, H - pad_b, W - pad_r, H - pad_b);

    static const char *colors[BH_SRC_COUNT] = {
        "#4FC3F7", "#FFD54F", "#FF8A65", "#AED581"
    };
    /* Si hay >1500 puntos, agrupar en bins manteniendo max-de-max,
     * min-de-min y avg-de-avg para no saturar el SVG ni perder picos. */
    #define BAT_MAX_RENDER_PTS 1500
    for (int k = 0; k < BH_SRC_COUNT; k++) {
        int n = s_src[k].n;
        if (n == 0) continue;
        int step = (n > BAT_MAX_RENDER_PTS) ? ((n + BAT_MAX_RENDER_PTS - 1) / BAT_MAX_RENDER_PTS) : 1;

        /* Polígono del rango max-min */
        bool has_range = false;
        for (int i = 0; i < n; i++) {
            if (s_src[k].y_max[i] != s_src[k].y_min[i]) { has_range = true; break; }
        }
        if (has_range) {
            sp = svg_vappend(svg, svg_cap, sp,
                "<polygon fill='%s' fill-opacity='0.18' stroke='none' points='",
                colors[k]);
            /* Subida por max (bin) */
            for (int i = 0; i < n; i += step) {
                int end = (i + step < n) ? i + step : n;
                float mx = s_src[k].y_max[i];
                for (int j = i + 1; j < end; j++) if (s_src[k].y_max[j] > mx) mx = s_src[k].y_max[j];
                int x = X_BAT_AS(s_src[k].x[i]);
                int y = Y_BAT_AS(mx);
                if (sp + 24 >= svg_cap) break;
                sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
            }
            /* Bajada por min (bin) en orden inverso */
            int last_bin_start = ((n - 1) / step) * step;
            for (int i = last_bin_start; i >= 0; i -= step) {
                int end = (i + step < n) ? i + step : n;
                float mn = s_src[k].y_min[i];
                for (int j = i + 1; j < end; j++) if (s_src[k].y_min[j] < mn) mn = s_src[k].y_min[j];
                int x = X_BAT_AS(s_src[k].x[i]);
                int y = Y_BAT_AS(mn);
                if (sp + 24 >= svg_cap) break;
                sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
            }
            sp = svg_vappend(svg, svg_cap, sp, "'/>");
        }
        /* Línea principal del avg (bin) */
        sp = svg_vappend(svg, svg_cap, sp,
            "<polyline fill='none' stroke='%s' stroke-width='2' points='", colors[k]);
        for (int i = 0; i < n; i += step) {
            int end = (i + step < n) ? i + step : n;
            float sum = 0; int cnt = 0;
            for (int j = i; j < end; j++) { sum += s_src[k].y[j]; cnt++; }
            float avg = (cnt > 0) ? sum / cnt : s_src[k].y[i];
            int x = X_BAT_AS(s_src[k].x[i]);
            int y = Y_BAT_AS(avg);
            if (sp + 24 >= svg_cap) break;
            sp = svg_vappend(svg, svg_cap, sp, "%d,%d ", x, y);
        }
        sp = svg_vappend(svg, svg_cap, sp, "'/>");
    }
    sp = svg_vappend(svg, svg_cap, sp, "</svg>");

    for (int k = 0; k < BH_SRC_COUNT; k++) ts_free(&s_src[k]);
    *svg_inner = svg;
}

esp_err_t handle_data_bateria(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char path[96];
    get_today_csv_path("bateria", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    if (!csv) {
        get_latest_csv_path("bateria", path, sizeof path);
        if (path[0]) csv = read_file_to_buf(path, &csv_len);
    }
    bool from_sd = (csv != NULL);

    if (!csv) {
        /* Sin RAM accesible para bateria multi-source: avisamos */
        httpd_resp_send_chunk(req,
            "<!DOCTYPE html><html><head>", -1);
        httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
        httpd_resp_send_chunk(req,
            "</head><body style='font-family:sans-serif;background:#111;color:#eee;padding:20px'>"
            "<h2>Sin datos</h2><p>No hay CSV de bateria en SD todavia.</p>"
            "<a href='/data' style='color:#00BFFF'>Volver</a></body></html>", -1);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    char *rows_html = NULL, *svg_inner = NULL;
    build_bateria_html(csv, &rows_html, &svg_inner);

    httpd_resp_send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Bateria</title>", -1);
    httpd_resp_send_chunk(req, SETTIME_SCRIPT, -1);
    httpd_resp_send_chunk(req,
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:10px}"
        "h2{text-align:center}"
        ".legend{text-align:center;margin:8px 0;font-size:14px}"
        ".legend span{display:inline-block;margin:0 10px}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;vertical-align:middle;margin-right:4px}"
        "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:13px}"
        "th,td{padding:6px;border-bottom:1px solid #333;text-align:center}"
        "th{background:#222}"
        ".bar{text-align:center;margin:10px 0}"
        ".bar a{color:#00BFFF;text-decoration:none;margin:0 8px}"
        "</style></head><body>"
        "<h2>BATERIA</h2>"
        "<div class='bar'><a href='/data'>&larr; Datos</a> <a href='/data/bateria.csv'>Descargar CSV (hoy)</a> <a href='/data/bateria.tar'>Descargar todo (.tar)</a></div>"
        "<div class='legend'>"
        "<span><i class='dot' style='background:#4FC3F7'></i>BatteryMonitor</span>"
        "<span><i class='dot' style='background:#FFD54F'></i>SolarCharger</span>"
        "<span><i class='dot' style='background:#FF8A65'></i>OrionXS</span>"
        "<span><i class='dot' style='background:#AED581'></i>ACCharger</span>"
        "</div>", -1);

    if (svg_inner) httpd_resp_send_chunk(req, svg_inner, -1);

    httpd_resp_send_chunk(req,
        from_sd ? "<p style='text-align:center;color:#888;font-size:12px'>Origen: SD</p>"
                : "<p style='text-align:center;color:#888;font-size:12px'>Origen: RAM</p>", -1);

    httpd_resp_send_chunk(req, "</body></html>", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    free(csv);
    free(rows_html);
    free(svg_inner);
    return ESP_OK;
}

esp_err_t handle_data_bateria_csv(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char path[64];
    get_today_csv_path("bateria", path, sizeof path);
    size_t csv_len = 0;
    char *csv = read_file_to_buf(path, &csv_len);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=bateria.csv");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        httpd_resp_sendstr(req, "timestamp,source,milli_amps\n");
    }
    return ESP_OK;
}

esp_err_t handle_data_index(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Logs</title>"
        "<script>"
        "(function(){try{var i=new Image();i.src='/settime?timestamp='"
        "+Math.floor(Date.now()/1000)+'&_='+Math.random();}catch(e){}})();"
        "</script>"
        "<style>"
        "body{background:#06080C;color:#fff;font-family:system-ui,sans-serif;margin:0;padding:16px}"
        "h1{color:#FF9800;margin:0 0 12px}"
        "nav{margin-bottom:16px;display:flex;gap:10px;flex-wrap:wrap}"
        "nav a{color:#4FC3F7;text-decoration:none;padding:6px 12px;border:1px solid #2D3340;border-radius:8px;font-size:14px}"
        "nav a:hover{background:#141821}"
        ".btn{display:block;margin:14px auto;padding:24px;font-size:20px;"
        "background:#141821;color:#eee;border:2px solid #2D3340;border-radius:14px;"
        "text-decoration:none;max-width:420px;text-align:center}"
        ".btn:active{background:#2D3340}"
        "h2{color:#FF9800;font-size:16px;margin:24px auto 8px;max-width:420px}"
        ".dl{display:flex;flex-wrap:wrap;gap:10px;max-width:420px;margin:0 auto}"
        ".dl a{flex:1 1 40%;color:#4FC3F7;text-decoration:none;text-align:center;"
        "padding:14px;background:#141821;border:1px solid #2D3340;border-radius:10px}"
        ".dl a:active{background:#2D3340}"
        "</style></head><body>"
        "<nav>"
          "<a href='/dashboard'>Dashboard</a>"
          "<a href='/data'><b>Logs</b></a>"
          "<a href='/keys'>Keys</a>"
        "</nav>"
        "<h1>Logs historicos</h1>"
        "<a class='btn' href='/data/frigo'>FRIGO</a>"
        "<a class='btn' href='/data/bateria'>BATERIA</a>"
        "<h2>Volcado por WiFi: elige la carpeta a descargar (.tar)</h2>"
        "<div class='dl'>"
          "<a href='/data/frigo.tar'>frigo</a>"
          "<a href='/data/bateria.tar'>bateria</a>"
          "<a href='/data/solar.tar'>solar</a>"
          "<a href='/data/capturas.tar'>capturas</a>"
          "<a href='/data/vigilancia.tar'>vigilancia</a>"
          "<a href='/data/config.tar'>config</a>"
          "<a href='/data/logs.tar'>logs</a>"
          /* "viaje" ahora lleva a la LISTA de viajes, que es donde se elige
           * cual bajarse y donde se ve si alguno esta incompleto. El paquete de
           * siempre (todo el historico) se llama ya por lo que es. */
          "<a href='/data/viajes'><b>viajes</b></a>"
          "<a href='/data/historico.tar'>historico</a>"
        "</div>"
        "</body></html>";
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}
