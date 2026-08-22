/* config_server_viaje.c — POST /api/viaje: los apuntes que manda el satelite 3.5"
 *
 * Diseño en 35cabina/docs/superpowers/specs/2026-08-22-viaje-con-nombre-y-carpeta.
 * Hechas las fases 1 (canal + inicio/fin) y 3 (los registros: repostaje, peaje,
 * bombona, mantenimiento y parada, cada uno a su CSV ademas del diario).
 *
 * Lo que cada apunte trae dentro y por que: el sello de tiempo lo pone EL
 * SATELITE, en el momento en que ocurrio, no la P4 al recibirlo -- un apunte
 * puede haber esperado dias en su cola.
 *
 * Por que HTTP y no UDP como la telemetria: por TCP se sabe con CERTEZA que se
 * entrego, y eso es lo que necesita una cola de pendientes. Con UDP habria que
 * reinventar confirmaciones y reintentos.
 *
 * Auth ESTRICTA: esto escribe en la tarjeta.
 */
#include "config_server_internal.h"
#include "config_server_auth.h"
#include "camera.h"          /* camera_sd_bus_lock: serializar con el GDMA */
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "viaje_srv";

#define VIAJES_DIR      "/sdcard/viajes"
#define NVS_NS          "viaje_p4"
#define NVS_CARPETA     "carpeta"     /* ruta del viaje abierto, ausente si no hay */
#define NVS_LAST_ID     "last_id"     /* ultimo id aplicado (idempotencia) */

/* El destino cabe en 20 caracteres (lo limita la 3.5"); la fecha son 10 y la
 * barra 1. Con 64 sobra y no hay que pensar en desbordes al componer rutas. */
#define CARPETA_MAX     64
#define RUTA_MAX        (CARPETA_MAX + 32)

/* ── Estado del viaje abierto, en NVS para sobrevivir a reinicios ─────────── */

static bool viaje_abierto(char *out, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    esp_err_t e = nvs_get_str(h, NVS_CARPETA, out, &len);
    nvs_close(h);
    return e == ESP_OK && out[0];
}

static uint32_t last_id_get(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_LAST_ID, &v);
        nvs_close(h);
    }
    return v;
}

static void estado_set(const char *carpeta, uint32_t id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (carpeta) nvs_set_str(h, NVS_CARPETA, carpeta);
    else         nvs_set_str(h, NVS_CARPETA, "");
    nvs_set_u32(h, NVS_LAST_ID, id);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Utilidades ───────────────────────────────────────────────────────────── */

/* fecha_dias son dias desde 1970 EN HORA LOCAL, tal y como los calculo la P4 y
 * los recibio el satelite (ver epoch_local en mini_proto.h). Se convierten con
 * gmtime a proposito, NO con localtime: el desfase ya esta aplicado, y volver a
 * aplicarlo correria la fecha un dia en los extremos. */
static void fecha_de_dias(uint32_t dias, char *out, size_t n)
{
    time_t t = (time_t)dias * 86400;
    struct tm tm_u;
    gmtime_r(&t, &tm_u);
    strftime(out, n, "%Y-%m-%d", &tm_u);
}

/* El destino llega ya filtrado por la 3.5", pero eso es la palabra de OTRO
 * aparato sobre un dato que acaba siendo una RUTA. Se vuelve a filtrar aqui:
 * solo letras, digitos, guion y guion bajo; el espacio pasa a guion bajo. */
static void destino_seguro(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        char c = in[i];
        if      ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_') out[j++] = c;
        else if (c == ' ')                                       out[j++] = '_';
        /* cualquier otra cosa (/, \, :, *, acentos...) se tira */
    }
    out[j] = 0;
    if (!j) snprintf(out, n, "viaje");   /* nunca una carpeta sin nombre */
}

/* Una linea al diario del viaje. eventos.csv es el que se lee para saber QUE
 * paso; los csv por tipo (fase 3) son para hacer cuentas. */
static void diario_en(const char *carpeta, const char *cuando,
                      const char *que, const char *detalle)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/eventos.csv", carpeta);

    bool nuevo = true;
    struct stat st;
    if (stat(ruta, &st) == 0 && st.st_size > 0) nuevo = false;

    FILE *f = fopen(ruta, "a");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", ruta); return; }
    if (nuevo) fprintf(f, "fecha_hora,evento,detalle\n");

    fprintf(f, "%s,%s,%s\n", cuando, que, detalle ? detalle : "");
    fclose(f);
}

/* Inicio y fin los fecha la P4: ocurren con ella delante (el inicio la exige, y
 * el fin llega por la cola pero su hora real la lleva dentro el propio evento
 * de cierre del satelite... que hoy no la manda, asi que de momento es la de
 * recepcion. Se afina en la fase 4, junto con el resumen). */
static void diario(const char *carpeta, const char *que, const char *detalle)
{
    time_t now = time(NULL);
    struct tm tm_l;
    localtime_r(&now, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l);
    diario_en(carpeta, cuando, que, detalle);
}

/* ── Las operaciones ──────────────────────────────────────────────────────── */

static esp_err_t op_inicio(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jd = cJSON_GetObjectItem(j, "destino");
    const cJSON *jf = cJSON_GetObjectItem(j, "fecha_dias");
    if (!cJSON_IsString(jd) || !cJSON_IsNumber(jf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "faltan destino o fecha_dias");
        return ESP_OK;
    }

    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        /* No se pisa un viaje abierto: seria perder la carpeta anterior sin que
         * nadie se enterara. El satelite tiene que cerrarlo primero. */
        ESP_LOGW(TAG, "inicio con viaje ya abierto en %s", abierto);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "ya hay un viaje abierto");
        return ESP_OK;
    }

    char destino[CARPETA_MAX], fecha[12];
    destino_seguro(jd->valuestring, destino, sizeof(destino));
    fecha_de_dias((uint32_t)jf->valuedouble, fecha, sizeof(fecha));

    char carpeta[CARPETA_MAX];
    snprintf(carpeta, sizeof(carpeta), VIAJES_DIR "/%s_%s", fecha, destino);

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    mkdir(VIAJES_DIR, 0777);
    int mk = mkdir(carpeta, 0777);
    struct stat st;
    bool hay = (mk == 0) || (stat(carpeta, &st) == 0);
    if (hay) diario(carpeta, "inicio", destino);
    camera_sd_bus_unlock();

    if (!hay) {
        ESP_LOGE(TAG, "no puedo crear %s", carpeta);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo crear la carpeta");
        return ESP_OK;
    }

    estado_set(carpeta, id);
    ESP_LOGI(TAG, "VIAJE ABIERTO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

static esp_err_t op_fin(httpd_req_t *req, uint32_t id)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto el fin ya no tiene nada que cerrar. Se responde OK
         * y no error: casi siempre sera un reintento de un fin que SI se
         * aplico, y devolver error dejaria al satelite atascado reintentando
         * algo que ya esta hecho. */
        ESP_LOGW(TAG, "fin sin viaje abierto: lo doy por hecho");
        httpd_resp_sendstr(req, "no habia viaje abierto");
        return ESP_OK;
    }

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    diario(carpeta, "fin", "");
    /* resumen.txt con los totales es la fase 4 del diseño; de momento se deja
     * la marca de cerrado, que es lo que distingue un viaje terminado de uno
     * cortado a lo bruto. */
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/resumen.txt", carpeta);
    FILE *f = fopen(ruta, "w");
    if (f) {
        time_t now = time(NULL);
        struct tm tm_l;
        localtime_r(&now, &tm_l);
        char cuando[20];
        strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M", &tm_l);
        fprintf(f, "Viaje cerrado el %s\n", cuando);
        fprintf(f, "Totales pendientes (fase 4 del diseno).\n");
        fclose(f);
    }
    camera_sd_bus_unlock();

    estado_set(NULL, id);
    ESP_LOGI(TAG, "VIAJE CERRADO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

/* Una fila al CSV del tipo. Se escribe ADEMAS del diario porque sirven para
 * cosas distintas: eventos.csv se lee para saber que paso, y estos para hacer
 * cuentas en una hoja de calculo.
 *
 * La cabecera sale de las CLAVES que manda el satelite, no de una lista fija
 * aqui: tenerla en los dos sitios seria garantizar que algun dia dejan de
 * coincidir, y las columnas se desalinearian en silencio. Por eso, al añadir,
 * se relee la primera linea y se compara: si el firmware del satelite ha
 * cambiado los campos, se escribe una cabecera nueva en vez de meter los
 * valores bajo las columnas de antes. Cuesta leer ~200 bytes por apunte, y los
 * apuntes son unos pocos al dia. */
static void csv_por_tipo(const char *carpeta, const char *tipo,
                         const cJSON *datos, const char *cuando)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/%ss.csv", carpeta, tipo);

    /* Cabecera esperada a partir de las claves recibidas. */
    char cab[512];
    int p = snprintf(cab, sizeof(cab), "fecha_hora");
    for (const cJSON *it = datos ? datos->child : NULL; it; it = it->next) {
        if (!it->string) continue;
        p += snprintf(cab + p, sizeof(cab) - p, ",%s", it->string);
        if (p >= (int)sizeof(cab) - 1) break;
    }

    bool poner_cab = true;
    FILE *f = fopen(ruta, "r");
    if (f) {
        char primera[512] = {0};
        if (fgets(primera, sizeof(primera), f)) {
            primera[strcspn(primera, "\r\n")] = 0;
            if (strcmp(primera, cab) == 0) poner_cab = false;
        }
        fclose(f);
    }

    f = fopen(ruta, "a");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", ruta); return; }
    if (poner_cab) fprintf(f, "%s\n", cab);

    fprintf(f, "%s", cuando);
    for (const cJSON *it = datos ? datos->child : NULL; it; it = it->next) {
        if (cJSON_IsString(it))      fprintf(f, ",%s", it->valuestring);
        else if (cJSON_IsNumber(it)) fprintf(f, ",%g", it->valuedouble);
        else                         fprintf(f, ",");
    }
    fprintf(f, "\n");
    fclose(f);
}

/* El sello de tiempo lo pone el SATELITE, en el momento en que ocurrio, y no
 * la P4 al recibirlo: un apunte puede haber esperado dias en su cola. Solo si
 * viene marcado como aproximado (el satelite nunca vio la hora) se usa la de
 * recepcion, y queda dicho en la columna 'hora_aprox'. */
static esp_err_t op_registro(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jt = cJSON_GetObjectItem(j, "tipo");
    const cJSON *jd = cJSON_GetObjectItem(j, "datos");
    if (!cJSON_IsString(jt)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "falta tipo");
        return ESP_OK;
    }

    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* 409 y no 200: el satelite no debe darlo por guardado. Pero tampoco
         * es un apunte invalido que haya que tirar -- puede que el inicio del
         * viaje siga en su cola por delante de este. Su repartidor reintenta
         * los 409 (ver viaje_cola.c). */
        ESP_LOGW(TAG, "registro '%s' sin viaje abierto", jt->valuestring);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "no hay viaje abierto");
        return ESP_OK;
    }

    /* Cuando ocurrio. */
    const cJSON *jts = cJSON_GetObjectItem(j, "ts");
    const cJSON *ja  = cJSON_GetObjectItem(j, "aprox");
    bool aprox = cJSON_IsTrue(ja) || !cJSON_IsNumber(jts) || jts->valuedouble < 1e9;
    time_t t = aprox ? time(NULL) : (time_t)jts->valuedouble;
    struct tm tm_l;
    /* El ts del satelite ya viene en hora LOCAL (deriva de epoch_local), asi
     * que gmtime; la hora de recepcion, en cambio, es un epoch de verdad y
     * necesita localtime. Mezclarlos correria una de las dos dos horas. */
    if (aprox) localtime_r(&t, &tm_l); else gmtime_r(&t, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l);

    const cJSON *jr = cJSON_GetObjectItem(j, "resumen");
    char detalle[128];
    snprintf(detalle, sizeof(detalle), "%s%s",
             cJSON_IsString(jr) ? jr->valuestring : "",
             aprox ? " (hora aproximada)" : "");

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    diario_en(carpeta, cuando, jt->valuestring, detalle);
    csv_por_tipo(carpeta, jt->valuestring, jd, cuando);
    camera_sd_bus_unlock();

    estado_set(carpeta, id);
    ESP_LOGI(TAG, "apunte '%s' guardado en %s", jt->valuestring, carpeta);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── El handler ───────────────────────────────────────────────────────────── */

esp_err_t handle_api_viaje(httpd_req_t *req)
{
    REQUIRE_AUTH_STRICT(req);

    /* 1 KB: el cuerpo mas grande de la fase 1 son unos 90 bytes. Los registros
     * de la fase 3 llevaran mas campos, pero siguen siendo cuatro numeros. */
    char body[1024];
    int total = req->content_len < (int)sizeof(body) - 1
              ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "json invalido");
        return ESP_OK;
    }

    const cJSON *jop = cJSON_GetObjectItem(j, "op");
    const cJSON *jid = cJSON_GetObjectItem(j, "id");
    if (!cJSON_IsString(jop) || !cJSON_IsNumber(jid)) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "faltan op o id");
        return ESP_OK;
    }
    uint32_t id = (uint32_t)jid->valuedouble;

    /* IDEMPOTENCIA. Con reintentos lo mismo puede llegar dos veces (se entrego
     * pero se perdio la respuesta). Se responde OK igualmente para que el
     * satelite lo de por entregado y no se atasque reintentando para siempre. */
    if (id != 0 && id <= last_id_get()) {
        ESP_LOGI(TAG, "id %lu ya aplicado, lo descarto", (unsigned long)id);
        cJSON_Delete(j);
        httpd_resp_sendstr(req, "duplicado");
        return ESP_OK;
    }

    esp_err_t ret;
    if      (!strcmp(jop->valuestring, "inicio")) ret = op_inicio(req, j, id);
    else if (!strcmp(jop->valuestring, "fin"))    ret = op_fin(req, id);
    else if (!strcmp(jop->valuestring, "registro")) ret = op_registro(req, j, id); else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "op? (inicio|fin|registro)");
        ret = ESP_OK;
    }

    cJSON_Delete(j);
    return ret;
}
