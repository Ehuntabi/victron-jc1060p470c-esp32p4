/* config_server_vigilancia.c — galeria de vigilancia (snapshot + capturas de
 * movimiento) para el portal web. Extraido de config_server.c (2026-08-14):
 * esta pieza no depende de nada del ciclo de vida del AP/auth mas alla de
 * REQUIRE_AUTH, asi que sale limpio a su propio fichero (mismo patron que
 * charts_svg.c y data_export_tar.c).
 */
#include "config_server_internal.h"
#include "config_server_vigilancia.h"
#include "camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>

// GET /snapshot -> foto JPEG del ultimo frame de la camara. Requiere auth (expone la
// camara). JPEG por HW (~80-150KB) en vez de BMP 1.58MB: ~10-20x menos latencia
// sobre el AP y sin el malloc de 1.58MB por peticion (que rozaba el suelo de PSRAM).
esp_err_t handle_snapshot(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    uint8_t *jpg = NULL;
    size_t   len = 0;
    if (!camera_snapshot_jpeg(&jpg, &len)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "camara sin frame todavia");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return r;
}

// GET /vigilancia -> lista las capturas de movimiento; /vigilancia/<id|fichero> -> sirve el JPEG.
/* Galeria de vigilancia.
 *
 * OJO al historial de esto: la galeria nacio leyendo SOLO el anillo en RAM
 * (PSRAM) de camera.c, cuando las capturas no llegaban a la tarjeta. Despues se
 * anadio vig_sd_drain_task, que las vuelca a /sdcard/vigilancia Y LIBERA EL SLOT
 * DEL ANILLO al conseguirlo (camera.c). Nadie actualizo la galeria: cada captura
 * desaparecia de la lista ~300 ms despues de hacerse, asi que la pagina salia
 * casi siempre vacia ("Aun no hay capturas") aunque los JPEG estuvieran
 * perfectamente guardados en la tarjeta. Parecia que la vigilancia no grababa.
 *
 * Ahora se listan las DOS: los ficheros de la tarjeta (el historial de verdad) y
 * lo que siga pendiente de volcar en el anillo. */
#define VIG_MAX     16        /* capturas del anillo en RAM (pendientes de volcar) */
#define VIG_SD_MAX  24        /* ficheros de la tarjeta que se muestran (los mas nuevos) */
#define VIG_SD_DIR_PATH "/sdcard/vigilancia"
/* "AAAAMMDD_HHMMSS/AAAAMMDD_HHMMSS_nnn.jpg" (carpeta de sesion + fichero) = 39 con el NUL */
#define VIG_NAME_LEN 48

/* Insercion ordenada en `names`, quedandose con las `max` MAS RECIENTES (el
 * mas nuevo al final). Compartida por vig_sd_list para cada .jpg encontrado,
 * ya en formato "sesion/fichero.jpg". */
static void vig_sd_list_insert(char names[][VIG_NAME_LEN], int max, int *n, int *total,
                               const char *combined)
{
    (*total)++;
    if (*n == max) {
        if (strcmp(combined, names[0]) <= 0) return;   /* mas viejo que todos */
        memmove(names[0], names[1], (size_t)(max - 1) * VIG_NAME_LEN);
        (*n)--;
    }
    int pos = *n;
    while (pos > 0 && strcmp(names[pos - 1], combined) > 0) {
        memcpy(names[pos], names[pos - 1], VIG_NAME_LEN);
        pos--;
    }
    snprintf(names[pos], VIG_NAME_LEN, "%s", combined);
    (*n)++;
}

/* Nombres de las capturas de la tarjeta como "sesion/fichero.jpg", ascendente
 * (la mas nueva al final): VIG_SD_DIR_PATH tiene una subcarpeta por sesion
 * (AAAAMMDD_HHMMSS, o AAAAMMDD para lo migrado de antes de este cambio), y
 * dentro los .jpg. Ambos niveles usan nombres con fecha, asi que ordenar
 * "sesion/fichero" por texto ES ordenar por fecha. Devuelve cuantos hay en la
 * lista; *total_out son los que hay en toda la tarjeta, para poder decir
 * cuantos quedan fuera en vez de truncar en silencio. */
static int vig_sd_list(char names[][VIG_NAME_LEN], int max, int *total_out)
{
    if (total_out) *total_out = 0;
    if (!camera_sd_bus_lock(2000)) return 0;
    DIR *dtop = opendir(VIG_SD_DIR_PATH);
    camera_sd_bus_unlock();
    if (!dtop) return 0;

    int n = 0, total = 0;
    for (;;) {
        if (!camera_sd_bus_lock(1000)) break;
        struct dirent *dses = readdir(dtop);
        camera_sd_bus_unlock();
        if (!dses) break;
        if (dses->d_name[0] == '.') continue;

        char sesdir[64];
        snprintf(sesdir, sizeof(sesdir), "%s/%s", VIG_SD_DIR_PATH, dses->d_name);
        if (!camera_sd_bus_lock(1000)) break;
        DIR *dsub = opendir(sesdir);
        camera_sd_bus_unlock();
        if (!dsub) continue;

        for (;;) {
            if (!camera_sd_bus_lock(1000)) break;
            struct dirent *ent = readdir(dsub);
            camera_sd_bus_unlock();
            if (!ent) break;
            const char *nm = ent->d_name;
            const size_t l = strlen(nm);
            if (l < 5 || strcmp(nm + l - 4, ".jpg") != 0) continue;
            char combined[VIG_NAME_LEN];
            int cl = snprintf(combined, sizeof(combined), "%s/%s", dses->d_name, nm);
            if (cl < 0 || (size_t)cl >= sizeof(combined)) continue;
            vig_sd_list_insert(names, max, &n, &total, combined);
        }
        while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
        closedir(dsub);
        camera_sd_bus_unlock();
    }
    while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
    closedir(dtop);
    camera_sd_bus_unlock();
    if (total_out) *total_out = total;
    return n;
}

/* Sirve un JPEG de la tarjeta en trozos, SOLTANDO el cerrojo del bus entre cada
 * uno: si el httpd retiene la SD durante todo el fichero, el GDMA de la camara
 * se queda parado y se acaba en INT WDT. Mismo patron que vig_write_jpeg_sd. */
static esp_err_t vig_sd_send(httpd_req_t *req, const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), VIG_SD_DIR_PATH "/%s", name);
    if (!camera_sd_bus_lock(2000)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "tarjeta ocupada, reintenta");
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "rb");
    camera_sd_bus_unlock();
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "image/jpeg");
    static char buf[4096];   /* estatico: la pila del httpd la comparten mas handlers */
    for (;;) {
        if (!camera_sd_bus_lock(2000)) break;
        const size_t r = fread(buf, 1, sizeof(buf), f);
        camera_sd_bus_unlock();
        if (r == 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
            fclose(f);
            camera_sd_bus_unlock();
            return ESP_FAIL;
        }
        vTaskDelay(1);   /* ceder al GDMA de la camara entre trozos */
    }
    while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
    fclose(f);
    camera_sd_bus_unlock();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
/* El id de un fichero de tarjeta es ahora "sesion/fichero.jpg" (antes solo
 * "fichero.jpg"): valida por caracteres permitidos en vez de solo rechazar
 * '/' y "..", que ahora SI puede llevar (como separador de un nivel, nunca
 * mas de uno). Nuestros propios nombres (strftime + contador) nunca usan otra
 * cosa que digitos, '_', '.', la extension en minusculas (".jpg") y esa
 * unica '/'.
 *
 * BUG encontrado 2026-08-14 (presente desde 998ce712, 2026-08-10): la lista
 * blanca no permitia letras, asi que CUALQUIER nombre acabado en ".jpg"
 * caia por la 'j' -> handle_vigilancia devolvia 403 para todas las
 * miniaturas de la tarjeta desde que existe el esquema "sesion/fichero.jpg".
 * El listado (que solo hace opendir/readdir) nunca paso por aqui, por eso
 * los nombres/fechas se veian bien y solo las imagenes salian rotas. */
static bool vig_sd_name_safe(const char *s)
{
    if (strstr(s, "..")) return false;
    int slashes = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '/') { if (++slashes > 1) return false; continue; }
        if (isdigit((unsigned char)*p) || *p == '_' || *p == '.' ||
            (*p >= 'a' && *p <= 'z')) continue;
        return false;
    }
    return true;
}

esp_err_t handle_vigilancia(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    const char *uri = req->uri;
    const char *idstr = NULL;
    if (strncmp(uri, "/vigilancia/", 12) == 0 && uri[12] != '\0') idstr = uri + 12;

    if (idstr) {
        /* Un nombre acabado en .jpg es un fichero de la tarjeta; un numero, una
         * captura del anillo en RAM aun sin volcar. */
        const size_t il = strlen(idstr);
        if (il >= 5 && il < VIG_NAME_LEN && strcmp(idstr + il - 4, ".jpg") == 0) {
            if (!vig_sd_name_safe(idstr)) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
                return ESP_FAIL;
            }
            return vig_sd_send(req, idstr);
        }
        uint32_t id = (uint32_t)strtoul(idstr, NULL, 10);
        uint8_t *jpg = NULL; size_t jlen = 0;
        if (id == 0 || !camera_vig_fetch(id, &jpg, &jlen)) { httpd_resp_send_404(req); return ESP_FAIL; }
        httpd_resp_set_type(req, "image/jpeg");
        esp_err_t r = httpd_resp_send(req, (const char *)jpg, jlen);
        free(jpg);
        return r;
    }

    /* Listado HTML con miniaturas en linea: primero lo pendiente en RAM (lo mas
     * reciente, aun sin volcar) y luego el historial de la tarjeta. */
    uint32_t ids[VIG_MAX]; time_t ts[VIG_MAX]; size_t lens[VIG_MAX];
    int n = camera_vig_list(ids, ts, lens, VIG_MAX);
    static char sd_names[VIG_SD_MAX][VIG_NAME_LEN];
    int sd_total = 0;
    const int sd_n = vig_sd_list(sd_names, VIG_SD_MAX, &sd_total);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Vigilancia</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;"
        "padding:12px}h2{margin:8px 0}a{color:#4FC3F7;text-decoration:none}"
        "img{max-width:100%;display:block;margin:6px 0;border:1px solid #333}"
        ".cap{padding:8px 0;border-bottom:1px solid #333}.t{color:#9e9e9e;font-size:13px}"
        "</style></head><body><h2>Capturas de vigilancia</h2>");
    if (n == 0 && sd_n == 0) {
        httpd_resp_sendstr_chunk(req, "<p>Aun no hay capturas. Activa el modo ausente y muevete "
                                      "delante de la camara.</p>");
    } else {
        char line[400];
        for (int i = 0; i < n; i++) {
            struct tm tmv; localtime_r(&ts[i], &tmv);
            char when[40];
            /* R6: si no hay hora fiable (sin RTC/NTP, año <2020) la fecha es basura;
             * mostrarlo claro en vez de un "1970-..." enganoso. */
            if (tmv.tm_year < 120)
                snprintf(when, sizeof(when), "captura #%u (hora no fijada)", (unsigned)ids[i]);
            else
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
            snprintf(line, sizeof(line),
                     "<div class=cap><div class=t>%s &middot; %u KB</div>"
                     "<a href='/vigilancia/%u'><img src='/vigilancia/%u' loading=lazy></a></div>",
                     when, (unsigned)(lens[i] / 1024), (unsigned)ids[i], (unsigned)ids[i]);
            httpd_resp_sendstr_chunk(req, line);
        }
        /* Historial de la tarjeta, del mas nuevo al mas viejo. nm es
         * "sesion/fichero.jpg"; el FICHERO lleva la fecha exacta de la foto
         * (AAAAMMDD_HHMMSS), asi que se extrae de ahi y no de la sesion (que
         * es solo el inicio de la sesion, puede ser un rato antes). */
        for (int i = sd_n - 1; i >= 0; i--) {
            const char *nm = sd_names[i];
            const char *slash = strrchr(nm, '/');
            const char *fn = slash ? slash + 1 : nm;
            snprintf(line, sizeof(line),
                     "<div class=cap><div class=t>%.4s-%.2s-%.2s %.2s:%.2s:%.2s &middot; tarjeta</div>"
                     "<a href='/vigilancia/%s'><img src='/vigilancia/%s' loading=lazy></a></div>",
                     fn, fn + 4, fn + 6, fn + 9, fn + 11, fn + 13, nm, nm);
            httpd_resp_sendstr_chunk(req, line);
        }
        char foot[240];
        snprintf(foot, sizeof(foot),
                 "<p class=t>%d en la tarjeta (se muestran las %d mas recientes)"
                 " &middot; %d pendientes de volcar en RAM.</p>",
                 sd_total, sd_n, n);
        httpd_resp_sendstr_chunk(req, foot);
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
