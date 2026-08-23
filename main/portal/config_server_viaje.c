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
 *
 * Operaciones: inicio | fin | registro | descartar.
 *
 * "descartar" (23-ago-2026) APARTA el viaje abierto en vez de cerrarlo: su
 * carpeta pasa a DESCARTADO_<nombre> y deja de contar. Existe porque la 3.5"
 * puede encontrarse con que aqui hay un viaje abierto que ella no conoce, y el
 * unico aparato que puede cerrarlo es ella -- esta P4 vive en la parte de
 * atras de la autocaravana y no se conduce desde alli.
 */
#include "config_server_internal.h"
#include "config_server_auth.h"
#include "camera.h"          /* camera_sd_bus_lock: serializar con el GDMA */
#include "cJSON.h"
#include "data/dashboard_state.h"
#include "data/trip_computer.h"
#include "frigo.h"
#include "ne185/ne185.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <dirent.h>

static const char *TAG = "viaje_srv";

#define VIAJES_DIR      "/sdcard/viajes"
/* Donde van los apuntes que NO son de un viaje: el historial del vehiculo.
 * Repostajes de camino al taller, la ITV, una bombona... cosas que le pasan a
 * la autocaravana, no a un viaje concreto. */
#define VEHICULO_DIR    "/sdcard/vehiculo"
#define NVS_NS          "viaje_p4"
#define NVS_CARPETA     "carpeta"     /* ruta del viaje abierto, ausente si no hay */
#define NVS_LAST_ID     "last_id"     /* ultimo id aplicado (idempotencia) */
/* Totales del viaje, acumulados SEGUN LLEGAN los apuntes en vez de sumando los
 * CSV al cerrar. Dos motivos: parsear CSV en el P4 seria bastante codigo para
 * algo que se puede ir sumando, y si el viaje se corta a lo bruto (bateria,
 * averia) los totales ya estan puestos al dia en vez de perderse. */
#define NVS_T_LITROS    "t_litros"
#define NVS_T_COMBUS    "t_combus"
#define NVS_T_PEAJE     "t_peaje"
#define NVS_T_BOMBONA   "t_bombona"
#define NVS_T_MANTEN    "t_manten"
#define NVS_T_PARADAS   "t_paradas"
#define NVS_T_MONEDA    "t_moneda"    /* la primera vista */
#define NVS_T_VARIAS    "t_varias"    /* 1 = hubo mas de una moneda */
#define NVS_T_INICIO    "t_inicio"    /* epoch del inicio, para contar los dias */
#define NVS_T_APLIC     "t_aplic"     /* apuntes APLICADOS de este viaje */

/* Marca de que al viaje le faltan apuntes. Un fichero y no una linea dentro de
 * otro: la lista de descargas tiene que decidir el estado de cada viaje con un
 * stat(), sin abrir y parsear nada. */
#define MARCA_INCOMPLETO "INCOMPLETO.txt"
/* Delante del nombre de la carpeta de un viaje apartado. */
#define MARCA_DESCARTADO "DESCARTADO_"

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

/* ── Totales del viaje ────────────────────────────────────────────────────── */

static double nvs_get_d(nvs_handle_t h, const char *k)
{
    /* NVS no guarda double: se mete el patron de bits en un u64. Es exacto y
     * evita redondeos que en dinero se notan. */
    uint64_t raw = 0;
    if (nvs_get_u64(h, k, &raw) != ESP_OK) return 0.0;
    double v; memcpy(&v, &raw, sizeof(v));
    return v;
}

static void nvs_set_d(nvs_handle_t h, const char *k, double v)
{
    uint64_t raw; memcpy(&raw, &v, sizeof(raw));
    nvs_set_u64(h, k, raw);
}

/* Suma lo que trae el apunte a los totales del viaje.
 *
 * OJO CON LAS MONEDAS: se suman los numeros tal cual, sin convertir. Un viaje
 * que cruza a Suiza mezclaria euros y francos en el mismo total y el resultado
 * no significaria nada. No se convierte (haria falta un cambio, que este
 * aparato no tiene y quedaria desfasado), pero SI se detecta: si aparece mas de
 * una moneda, el resumen lo dice en vez de dar un total que parece bueno. */
static void totales_sumar(const char *tipo, const cJSON *datos)
{
    if (!datos) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    const cJSON *jm = cJSON_GetObjectItem(datos, "moneda");
    if (cJSON_IsString(jm) && jm->valuestring[0]) {
        char vista[8] = {0};
        size_t n = sizeof(vista);
        if (nvs_get_str(h, NVS_T_MONEDA, vista, &n) != ESP_OK || !vista[0]) {
            nvs_set_str(h, NVS_T_MONEDA, jm->valuestring);
        } else if (strcmp(vista, jm->valuestring) != 0) {
            nvs_set_u8(h, NVS_T_VARIAS, 1);
        }
    }

    /* atof de un campo de texto; los importes viajan como cadena porque es lo
     * que hay tecleado, con su coma o su punto tal cual. */
    const cJSON *ji;
    #define IMPORTE_DE(campo) ( (ji = cJSON_GetObjectItem(datos, campo)) && \
                                cJSON_IsString(ji) ? atof(ji->valuestring) : 0.0 )

    if (!strcmp(tipo, "repostaje")) {
        nvs_set_d(h, NVS_T_LITROS, nvs_get_d(h, NVS_T_LITROS) + IMPORTE_DE("litros"));
        nvs_set_d(h, NVS_T_COMBUS, nvs_get_d(h, NVS_T_COMBUS) + IMPORTE_DE("importe"));
    } else if (!strcmp(tipo, "peaje")) {
        nvs_set_d(h, NVS_T_PEAJE, nvs_get_d(h, NVS_T_PEAJE) + IMPORTE_DE("importe"));
    } else if (!strcmp(tipo, "bombona")) {
        nvs_set_d(h, NVS_T_BOMBONA, nvs_get_d(h, NVS_T_BOMBONA) + IMPORTE_DE("precio"));
    } else if (!strcmp(tipo, "mantenimiento")) {
        nvs_set_d(h, NVS_T_MANTEN, nvs_get_d(h, NVS_T_MANTEN) + IMPORTE_DE("coste"));
    } else if (!strcmp(tipo, "parada")) {
        uint32_t np = 0;
        nvs_get_u32(h, NVS_T_PARADAS, &np);
        nvs_set_u32(h, NVS_T_PARADAS, np + 1);
    }
    #undef IMPORTE_DE

    nvs_commit(h);
    nvs_close(h);
}

static void totales_borrar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_T_LITROS);  nvs_erase_key(h, NVS_T_COMBUS);
    nvs_erase_key(h, NVS_T_PEAJE);   nvs_erase_key(h, NVS_T_BOMBONA);
    nvs_erase_key(h, NVS_T_MANTEN);  nvs_erase_key(h, NVS_T_PARADAS);
    nvs_erase_key(h, NVS_T_MONEDA);  nvs_erase_key(h, NVS_T_VARIAS);
    nvs_commit(h);
    nvs_close(h);
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

    totales_borrar();
    {   /* Numeracion nueva: ver el comentario de la idempotencia en el handler. */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, NVS_LAST_ID, 0);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    {   /* El epoch del inicio, para contar los dias al cerrar. Se guarda en vez
         * de deducirlo del nombre de la carpeta: ahi solo esta la fecha, y un
         * viaje de una noche saldria como "0 dias". */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u64(h, NVS_T_INICIO, (uint64_t)time(NULL));
            nvs_set_u32(h, NVS_T_APLIC, 1);   /* el inicio ya cuenta */
            nvs_commit(h);
            nvs_close(h);
        }
    }
    estado_set(carpeta, id);
    ESP_LOGI(TAG, "VIAJE ABIERTO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

/* resumen.txt: lo que uno quiere saber al volver, en castellano llano y no en
 * columnas. Los CSV ya estan ahi para las cuentas finas. */
static void escribir_resumen(const char *carpeta)
{
    nvs_handle_t h;
    double litros = 0, combus = 0, peaje = 0, bombona = 0, manten = 0;
    uint32_t paradas = 0;
    uint64_t inicio = 0;
    uint8_t varias = 0;
    char moneda[8] = "EUR";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        litros  = nvs_get_d(h, NVS_T_LITROS);
        combus  = nvs_get_d(h, NVS_T_COMBUS);
        peaje   = nvs_get_d(h, NVS_T_PEAJE);
        bombona = nvs_get_d(h, NVS_T_BOMBONA);
        manten  = nvs_get_d(h, NVS_T_MANTEN);
        nvs_get_u32(h, NVS_T_PARADAS, &paradas);
        nvs_get_u64(h, NVS_T_INICIO, &inicio);
        nvs_get_u8(h, NVS_T_VARIAS, &varias);
        size_t n = sizeof(moneda);
        nvs_get_str(h, NVS_T_MONEDA, moneda, &n);
        nvs_close(h);
    }

    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/resumen.txt", carpeta);
    FILE *f = fopen(ruta, "w");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", ruta); return; }

    time_t now = time(NULL);
    struct tm tm_l;
    localtime_r(&now, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M", &tm_l);

    fprintf(f, "RESUMEN DEL VIAJE\n");
    fprintf(f, "=================\n\n");
    fprintf(f, "Cerrado el %s\n", cuando);
    if (inicio > 1000000000ULL) {
        /* Dias de calendario, no periodos de 24 h: salir un viernes y volver el
         * domingo son tres dias de viaje, aunque no sean 72 horas. */
        long dias = (long)((now / 86400) - ((time_t)inicio / 86400)) + 1;
        fprintf(f, "Duracion: %ld dia%s\n", dias, dias == 1 ? "" : "s");
    }
    fprintf(f, "\nGASTOS (%s)\n", moneda);
    fprintf(f, "  Combustible ... %8.2f   (%.1f litros)\n", combus, litros);
    fprintf(f, "  Peajes ........ %8.2f\n", peaje);
    fprintf(f, "  Gas ........... %8.2f\n", bombona);
    fprintf(f, "  Mantenimiento . %8.2f\n", manten);
    fprintf(f, "  ----------------------\n");
    fprintf(f, "  TOTAL ......... %8.2f\n", combus + peaje + bombona + manten);
    if (varias) {
        /* Sumar euros con francos da un numero que parece bueno y no lo es. */
        fprintf(f, "\n  OJO: hubo mas de una moneda en este viaje.\n");
        fprintf(f, "  Los totales estan SUMADOS SIN CONVERTIR y no valen.\n");
        fprintf(f, "  Mira los csv de cada tipo, que llevan su moneda.\n");
    }
    if (litros > 0.01 && combus > 0.01) {
        fprintf(f, "\n  Precio medio del litro: %.3f %s\n", combus / litros, moneda);
    }
    fprintf(f, "\nParadas anotadas: %lu\n", (unsigned long)paradas);

    trip_computer_t t;
    trip_computer_get(&t);
    fprintf(f, "\nENERGIA\n");
    fprintf(f, "  Cargados ...... %8.0f Wh\n", t.wh_charged);
    fprintf(f, "  Gastados ...... %8.0f Wh\n", t.wh_discharged);
    fprintf(f, "  De la placa ... %8.0f Wh   (%.1f h de sol)\n",
            t.wh_solar, (double)t.solar_seconds / 3600.0);

    /* Sin kilometros: esta pantalla no tiene GPS ni cuentakilometros. Se dice en
     * el propio fichero en vez de omitirlo, para que no parezca que el viaje fue
     * de 0 km. */
    fprintf(f, "\nKilometros: no disponibles (la P4 aun no tiene GPS).\n");
    fclose(f);
}

/* Compara lo que dice el satelite que genero con lo que esta P4 ha aplicado.
 *
 * Sin esto, un viaje al que le falte un repostaje se descargaria con pinta de
 * estar entero y el analizador del PC se lo tragaria como bueno. Es justo el
 * fallo que mas cuesta detectar despues: no falta el fichero, falta UNA LINEA.
 *
 * Devuelve cuantos faltan (0 = entero). */
static uint32_t comprobar_completo(const cJSON *j, const char *carpeta)
{
    const cJSON *je = cJSON_GetObjectItem(j, "eventos");
    if (!cJSON_IsNumber(je)) return 0;      /* satelite viejo: no se puede saber */

    uint32_t esperados = (uint32_t)je->valuedouble;
    uint32_t aplicados = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_T_APLIC, &aplicados);
        nvs_close(h);
    }
    aplicados++;                            /* este mismo fin */

    if (aplicados >= esperados) return 0;
    uint32_t faltan = esperados - aplicados;

    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/" MARCA_INCOMPLETO, carpeta);
    FILE *f = fopen(ruta, "w");
    if (f) {
        fprintf(f, "A este viaje le faltan %lu apunte%s.\n\n",
                (unsigned long)faltan, faltan == 1 ? "" : "s");
        fprintf(f, "La pantalla de la cabina dice haber generado %lu y aqui han\n"
                   "llegado %lu. Se perdieron por el camino: la cola se lleno, o\n"
                   "hubo un reinicio en mal momento.\n\n",
                (unsigned long)esperados, (unsigned long)aplicados);
        fprintf(f, "Lo demas es correcto. Este aviso esta para que no des el\n"
                   "viaje por entero al analizarlo.\n");
        fclose(f);
    }
    ESP_LOGW(TAG, "viaje INCOMPLETO: esperados %lu, aplicados %lu",
             (unsigned long)esperados, (unsigned long)aplicados);
    return faltan;
}

static esp_err_t op_fin(httpd_req_t *req, const cJSON *j, uint32_t id)
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
    uint32_t faltan = comprobar_completo(j, carpeta);
    escribir_resumen(carpeta);
    if (faltan) {
        FILE *rf; char rr[RUTA_MAX];
        snprintf(rr, sizeof(rr), "%s/resumen.txt", carpeta);
        rf = fopen(rr, "a");
        if (rf) {
            fprintf(rf, "\n*** VIAJE INCOMPLETO: faltan %lu apuntes. ***\n",
                    (unsigned long)faltan);
            fprintf(rf, "Los totales de arriba NO los incluyen. Ver "
                        MARCA_INCOMPLETO ".\n");
            fclose(rf);
        }
    }
    camera_sd_bus_unlock();

    estado_set(NULL, id);
    ESP_LOGI(TAG, "VIAJE CERRADO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

/* APARTAR el viaje abierto, sin cerrarlo como bueno.
 *
 * Para cuando se empezo un viaje por error o de prueba: la carpeta pasa a
 * llamarse DESCARTADO_<nombre> y deja de contar como viaje. NO se borra --
 * un viaje entero perdido por un dedazo no se recupera, y renombrar cuesta lo
 * mismo. Quien quiera el hueco de la tarjeta, que lo borre a mano.
 *
 * Lo pide la 3.5" desde la cabina: cuando va a empezar un viaje y esta pantalla
 * responde 409, ofrece guardarlo o apartarlo ahi mismo. La P4 vive en la parte
 * de atras y levantarse del asiento del conductor para pulsar un boton no es
 * una opcion. */
static esp_err_t op_descartar(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    (void)j;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Mismo criterio que op_fin: casi siempre es un reintento de algo que
         * ya se aplico, y un error dejaria al satelite atascado. */
        ESP_LOGW(TAG, "descartar sin viaje abierto: lo doy por hecho");
        httpd_resp_sendstr(req, "no habia viaje abierto");
        return ESP_OK;
    }

    const char *nombre = strrchr(carpeta, '/');
    nombre = nombre ? nombre + 1 : carpeta;

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }

    char destino[RUTA_MAX + 32];
    snprintf(destino, sizeof(destino), VIAJES_DIR "/" MARCA_DESCARTADO "%s", nombre);
    /* rename() falla si el destino ya existe, y con dos pruebas del mismo dia
     * eso pasa a la primera. Se numera en vez de dar error. */
    struct stat st;
    for (int k = 2; k < 100 && stat(destino, &st) == 0; k++) {
        snprintf(destino, sizeof(destino), VIAJES_DIR "/" MARCA_DESCARTADO "%s_%d",
                 nombre, k);
    }

    diario(carpeta, "descartado", "");
    int r = rename(carpeta, destino);
    camera_sd_bus_unlock();

    if (r != 0) {
        ESP_LOGE(TAG, "no puedo apartar %s -> %s", carpeta, destino);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo apartar la carpeta");
        return ESP_OK;
    }

    estado_set(NULL, id);
    ESP_LOGW(TAG, "VIAJE DESCARTADO: %s -> %s", carpeta, destino);
    httpd_resp_sendstr(req, destino);
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

    /* Sin viaje abierto el apunte NO se rechaza: es de una salida puntual y va
     * al historial del vehiculo.
     *
     * Antes esto era un 409 "no hay viaje abierto", pensado para cuando el
     * inicio del viaje seguia en la cola del satelite por delante de este
     * apunte. Con el cuaderno reorganizado por SALIDAS ya no vale: una salida
     * puntual no tiene viaje y nunca lo va a tener, asi que el 409 dejaba a la
     * cola reintentando el mismo apunte para siempre (viaje_cola.c reintenta
     * los 409 a proposito) y detras se atascaba todo lo demas. */
    char carpeta[CARPETA_MAX];
    bool en_viaje = viaje_abierto(carpeta, sizeof(carpeta));
    if (!en_viaje) snprintf(carpeta, sizeof(carpeta), VEHICULO_DIR);

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
    if (!en_viaje) mkdir(VEHICULO_DIR, 0777);
    diario_en(carpeta, cuando, jt->valuestring, detalle);
    csv_por_tipo(carpeta, jt->valuestring, jd, cuando);
    camera_sd_bus_unlock();

    /* Los totales y el contador de apuntes son DEL VIAJE: un repostaje del
     * historial del vehiculo no cuenta en el resumen de ningun viaje, y
     * sumarlo al contador haria que el viaje siguiente pareciese completo con
     * un apunte de menos. */
    if (en_viaje) {
        totales_sumar(jt->valuestring, jd);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            uint32_t n = 0;
            nvs_get_u32(h, NVS_T_APLIC, &n);
            nvs_set_u32(h, NVS_T_APLIC, n + 1);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    /* NULL y no 'carpeta' si no hay viaje: estado_set guarda cual es el viaje
     * ABIERTO, y el historial del vehiculo no lo es. Pasarlo abriria un viaje
     * fantasma en /sdcard/vehiculo. El id si se guarda: es la idempotencia. */
    estado_set(en_viaje ? carpeta : NULL, id);
    ESP_LOGI(TAG, "apunte '%s' guardado en %s", jt->valuestring, carpeta);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── Telemetria y contadores mientras dura el viaje ───────────────────────── */

/* Cada 5 min una fila de telemetria; cada hora una de contadores. Cinco minutos
 * dan 288 filas al dia, que es lo mismo que ya usa el historico del frigo y
 * pinta una grafica decente sin llenar la tarjeta. */
#define TELEMETRIA_MIN   5
#define CONTADORES_MIN  60

static void fila_telemetria(const char *carpeta, const struct tm *tm_l)
{
    char ruta[RUTA_MAX];
    char dia[12];
    strftime(dia, sizeof(dia), "%Y-%m-%d", tm_l);
    snprintf(ruta, sizeof(ruta), "%s/telemetria_%s.csv", carpeta, dia);

    struct stat st;
    bool nuevo = !(stat(ruta, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(ruta, "a");
    if (!f) return;
    if (nuevo) fprintf(f, "hora,soc,bateria_v,solar_w,frigo_c,exterior_c,fan_pct,agua_limpia,agua_grises\n");

    dashboard_snapshot_t d;
    dashboard_state_snapshot(&d);
    frigo_state_t fr;
    frigo_get_state_copy(&fr);
    ne185_data_t ne;
    ne185_get(&ne);

    char hora[9];
    strftime(hora, sizeof(hora), "%H:%M:%S", tm_l);
    fprintf(f, "%s,", hora);
    if (d.bat_has) fprintf(f, "%u.%u,%u.%02u,", d.soc_deci / 10, d.soc_deci % 10,
                           d.bat_v_centi / 100, d.bat_v_centi % 100);
    else           fprintf(f, ",,");
    if (d.solar_has) fprintf(f, "%u,", d.pv_w); else fprintf(f, ",");
    fprintf(f, "%.1f,%.1f,%u,", fr.T_Congelador, fr.T_Exterior, fr.fan_percent);
    if (ne.fresh) fprintf(f, "%u,%u\n", ne.s1, ne.r1); else fprintf(f, ",\n");
    fclose(f);
}

static void fila_contadores(const char *carpeta, const struct tm *tm_l)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/contadores.csv", carpeta);

    struct stat st;
    bool nuevo = !(stat(ruta, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(ruta, "a");
    if (!f) return;
    /* Sin kilometros: la P4 no tiene GPS ni cuentakilometros todavia. Cuando lo
     * tenga, la columna se añade AQUI y al final, para no descolocar lo ya
     * escrito de viajes anteriores. */
    if (nuevo) fprintf(f, "fecha_hora,horas_activo,wh_cargados,wh_gastados,wh_solar,horas_sol\n");

    trip_computer_t t;
    trip_computer_get(&t);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", tm_l);
    fprintf(f, "%s,%.2f,%.0f,%.0f,%.0f,%.2f\n", cuando,
            (double)t.seconds_running / 3600.0, t.wh_charged, t.wh_discharged,
            t.wh_solar, (double)t.solar_seconds / 3600.0);
    fclose(f);
}

/* Corre desde un esp_timer, no desde la tarea del portal: la telemetria tiene
 * que seguir cayendo aunque nadie toque el portal en todo el viaje. */
static void tick_viaje_cb(void *arg)
{
    (void)arg;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return;

    time_t now = time(NULL);
    if (now < 1000000000L) return;          /* sin hora buena no se apunta nada */
    struct tm tm_l;
    localtime_r(&now, &tm_l);

    static int64_t ultima_tel = 0, ultimo_cont = 0;
    int64_t ahora_us = esp_timer_get_time();

    bool toca_tel  = (ultima_tel  == 0) || (ahora_us - ultima_tel  >= (int64_t)TELEMETRIA_MIN * 60 * 1000000LL);
    bool toca_cont = (ultimo_cont == 0) || (ahora_us - ultimo_cont >= (int64_t)CONTADORES_MIN * 60 * 1000000LL);
    if (!toca_tel && !toca_cont) return;

    if (!camera_sd_bus_lock(3000)) return;   /* ya se apuntara al siguiente */
    if (toca_tel)  { fila_telemetria(carpeta, &tm_l); ultima_tel  = ahora_us; }
    if (toca_cont) { fila_contadores(carpeta, &tm_l); ultimo_cont = ahora_us; }
    camera_sd_bus_unlock();
}

void viaje_telemetria_start(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = tick_viaje_cb, .name = "viaje_tick" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        /* Se despierta cada minuto y el propio callback decide si toca escribir.
         * Mas simple que dos timers, y el coste de mirar el reloj es nulo. */
        esp_timer_start_periodic(t, 60 * 1000000ULL);
        ESP_LOGI(TAG, "telemetria del viaje armada (cada %d min, contadores cada %d)",
                 TELEMETRIA_MIN, CONTADORES_MIN);
    }
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
     * satelite lo de por entregado y no se atasque reintentando para siempre.
     *
     * EL INICIO SE EXCLUYE, y no es un descuido. El contador vive en la NVS del
     * satelite: si esa NVS se borra (un reflasheo con borrado, un cambio de
     * placa) su numeracion vuelve a 1 mientras esta P4 sigue recordando que iba
     * por el 50. Con el inicio dentro de esta comprobacion, TODO lo que llegara
     * despues se descartaria como duplicado respondiendo 200, el satelite
     * vaciaria su cola creyendolo entregado y se perderia el viaje entero sin un
     * solo mensaje de error.
     *
     * Empezar un viaje es, por definicion, empezar una numeracion nueva: el
     * inicio pasa siempre y pone el contador a cero. Un inicio repetido de
     * verdad no cuela igualmente -- op_inicio devuelve 409 si ya hay uno
     * abierto. Detectado auditando el 22-ago-2026. */
    bool es_inicio = !strcmp(jop->valuestring, "inicio");
    if (!es_inicio && id != 0 && id <= last_id_get()) {
        ESP_LOGI(TAG, "id %lu ya aplicado, lo descarto", (unsigned long)id);
        cJSON_Delete(j);
        httpd_resp_sendstr(req, "duplicado");
        return ESP_OK;
    }

    esp_err_t ret;
    if      (!strcmp(jop->valuestring, "inicio")) ret = op_inicio(req, j, id);
    else if (!strcmp(jop->valuestring, "fin"))    ret = op_fin(req, j, id);
    else if (!strcmp(jop->valuestring, "registro")) ret = op_registro(req, j, id);
    else if (!strcmp(jop->valuestring, "descartar")) ret = op_descartar(req, j, id);
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "op? (inicio|fin|registro|descartar)");
        ret = ESP_OK;
    }

    cJSON_Delete(j);
    return ret;
}


/* ── Descarga de viajes ────────────────────────────────────────────────────
 *
 * Hasta ahora /data/viaje.tar NO era un viaje: empaquetaba el historico entero
 * (bateria + solar + frigo). El nombre mentia desde que existe. Ahora ese
 * paquete es /data/historico.tar y "viaje" significa un viaje de verdad.
 *
 * Un viaje solo se puede bajar cuando la P4 SABE que no le falta nada. El aviso
 * ya estaba escrito en el codigo del tar: "el analizador del PC se traga un
 * viaje incompleto creyendo que esta entero". */

typedef enum { V_EN_CURSO, V_INCOMPLETO, V_LISTO } estado_viaje_t;

static estado_viaje_t estado_de(const char *nombre)
{
    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        const char *base = strrchr(abierto, '/');
        if (base && !strcmp(base + 1, nombre)) return V_EN_CURSO;
    }
    char ruta[RUTA_MAX];
    struct stat st;
    snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/" MARCA_INCOMPLETO, nombre);
    if (stat(ruta, &st) == 0) return V_INCOMPLETO;
    snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/resumen.txt", nombre);
    if (stat(ruta, &st) == 0) return V_LISTO;
    return V_EN_CURSO;      /* sin resumen: nunca se cerro */
}

esp_err_t handle_data_viajes(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
        "margin:0;padding:16px}h1{font-size:20px}a{color:#4FC3F7}"
        "li{margin:14px 0;list-style:none;border-left:3px solid #333;padding-left:10px}"
        ".ok{border-color:#66BB6A}.inc{border-color:#FFA726}.cur{border-color:#888}"
        ".e{font-size:13px;color:#aaa}</style>"
        "<h1>Viajes guardados</h1><ul>");

    DIR *d = opendir(VIAJES_DIR);
    bool alguno = false;
    if (d) {
        struct dirent *ent;
        char linea[512];
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            alguno = true;
            estado_viaje_t e = estado_de(ent->d_name);
            if (e == V_LISTO) {
                snprintf(linea, sizeof(linea),
                    "<li class=ok><b>%s</b><div class=e>Listo</div>"
                    "<a href='/data/viaje.tar?v=%s'>Descargar</a></li>",
                    ent->d_name, ent->d_name);
            } else if (e == V_INCOMPLETO) {
                /* Salida de emergencia: si algo se perdio para siempre, el viaje
                 * quedaria bloqueado eternamente. Se deja bajar, pero con el
                 * nombre gritando lo que es -- imposible confundirlo con uno
                 * entero al verlo en la carpeta de descargas del PC. */
                snprintf(linea, sizeof(linea),
                    "<li class=inc><b>%s</b><div class=e>INCOMPLETO: le faltan apuntes. "
                    "Lee " MARCA_INCOMPLETO " dentro.</div>"
                    "<a href='/data/viaje.tar?v=%s&incompleto=si'>Descargar de todos modos</a></li>",
                    ent->d_name, ent->d_name);
            } else {
                snprintf(linea, sizeof(linea),
                    "<li class=cur><b>%s</b><div class=e>En curso: finalizalo en la "
                    "pantalla de la cabina antes de bajarlo</div></li>", ent->d_name);
            }
            httpd_resp_sendstr_chunk(req, linea);
        }
        closedir(d);
    }
    if (!alguno) httpd_resp_sendstr_chunk(req, "<li>Todavia no hay ningun viaje.</li>");
    httpd_resp_sendstr_chunk(req,
        "</ul><p class=e>El paquete de siempre (bateria, solar y frigo de TODOS "
        "los dias) esta en <a href='/data/historico.tar'>historico.tar</a>.</p>"
        "<p><a href='/data'>Volver</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
