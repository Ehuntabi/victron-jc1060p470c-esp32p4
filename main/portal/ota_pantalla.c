/* ota_pantalla.c — servidor de actualizacion OTA para las DOS pantallas
 * (cabina5 = 5", 35cabina = 3,5") desde la P4.
 *
 * POR QUE ESTA AQUI Y NO EN CADA CABINA: la P4 es la que tiene la tarjeta SD y
 * el portal Wi-Fi del que ya se sirven los .tar y la OTA de la propia P4. Las
 * cabinas solo tienen que pedir el .bin y escribirlo en su particion de OTA;
 * guardar los dos firmwares en la P4 permite actualizar las dos pantallas desde
 * el movil sin sacar ninguna del sitio.
 *
 * REPARTO DE SERVIDORES (ver config_server.c):
 *   - /api/pantalla/info va en el httpd LIGERO (80), como /api/state: es lo que
 *     se pide al entrar en la pantalla de actualizacion.
 *   - /api/pantalla/bin y /api/pantalla/subir van en el PESADO (8081), como
 *     /ota y los .tar: mover 2-4 MB por Wi-Fi no puede dejar mudo el portal.
 *
 * NADA de ESP_ERROR_CHECK en los caminos de datos: un abort en la tarea del
 * httpd se lleva por delante el portal entero (y con la cabina a medias).
 */
#include "ota_pantalla.h"
#include "config_server_internal.h"   /* REQUIRE_AUTH / REQUIRE_AUTH_STRICT_AVISO */
#include "camera.h"                   /* camera_sd_bus_lock y sd_reader_* */
#include "datalogger.h"               /* datalogger_sd_montada */
#include "sd_safe.h"                  /* sd_mkdir con el cerrojo de la SD dentro */
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"              /* esp_vfs_fat_info: hueco libre real */
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>                   /* fsync */
#include <sys/stat.h>

static const char *TAG = "ota_pantalla";

/* Los dos modelos del contrato. El nombre es el `project_name` del binario y
 * tambien el nombre del fichero: asi no hay una tabla de traduccion que se
 * pueda quedar desfasada (si el .bin se llama cabina5.bin, su project_name
 * TIENE que ser "cabina5", y la validacion de la subida lo exige). */
typedef struct {
    const char *nombre;
    const char *ruta;
} pantalla_modelo_t;

#define PANTALLA_DIR "/sdcard/firmware"

static const pantalla_modelo_t s_modelos[] = {
    { "cabina5",  PANTALLA_DIR "/cabina5.bin"  },
    { "35cabina", PANTALLA_DIR "/35cabina.bin" },
};
#define PANTALLA_NUM_MODELOS (sizeof(s_modelos) / sizeof(s_modelos[0]))

/* Tope del contrato: 0x3F0000 = 4.128.768 bytes. Es el tamano que cabe de
 * sobra en una particion de app de las cabinas y el que el cliente espera.
 * Un .bin mayor se rechaza ANTES de escribir nada en la tarjeta. */
#define PANTALLA_MAX_BYTES 0x3F0000u

/* Primeros 112 bytes del .bin: cabecera de imagen (24 B) + cabecera del primer
 * segmento (8 B) + esp_app_desc_t hasta el final de project_name. En 0x20 esta
 * el magic 0xABCD5432, en 0x30 el campo version y en 0x50 project_name. */
#define PANTALLA_CABECERA 0x70u

/* Version devuelta al cliente: el contrato pide 16 bytes del campo (el campo
 * real de esp_app_desc_t son 32, pero 16 sobran para "vX.Y"). */
#define PANTALLA_VER_N 17

#define PANTALLA_CHUNK  4096
#define PANTALLA_ESPERAS 4
/* Subir 4 MB por el AP puede tardar; 10 min cubren una subida lenta legitima
 * sin dejar la tarea del httpd colgada para siempre con un cliente que gotea. */
#define PANTALLA_PLAZO_US (10LL * 60LL * 1000000LL)

/* ── Regla PURA de validacion del .bin ──────────────────────────────────────
 *
 * Esta funcion no toca HTTP, ni la SD, ni la NVS: solo mira bytes. El test de
 * host (test/test_pantalla_fw.sh) EXTRAE este texto del fuente y lo compila con
 * gcc, igual que test_viaje_dueno.sh hace con las reglas del viaje. Asi lo que
 * se prueba es el codigo que corre en la placa, no una copia que se queda
 * vieja.
 *
 * Devuelve true y deja la version en ver[] solo si:
 *   - hay al menos PANTALLA_CABECERA bytes de cabecera,
 *   - el tamano total es razonable (entre la cabecera y bytes_max),
 *   - el magic de esp_app_desc_t (0xABCD5432) esta en el offset 0x20,
 *   - project_name es EXACTAMENTE el modelo pedido (NUL dentro del campo),
 *   - version empieza por 'v' y es ASCII imprimible.
 */
static bool pantalla_fw_valido(const uint8_t *cab, size_t cab_n, size_t bytes,
                               size_t bytes_max, const char *modelo,
                               char *ver, size_t ver_n)
{
    /* Offsets ABSOLUTOS dentro del .bin, no dentro del struct: un firmware de
     * ESP-IDF empieza por esp_image_header_t (24 B) y el primer
     * esp_image_segment_header_t (8 B), asi que el esp_app_desc_t cae en 0x20
     * y su campo version en 0x30. Comprobado con los .bin publicados
     * (xxd -s 0x20: alli estan cd ab 54 32 y "v1.30"). */
    const size_t OFF_MAGIC = 0x20;
    const size_t OFF_VER   = 0x30;
    const size_t OFF_PROY  = 0x50;
    const size_t PROY_MAX  = 32;   /* project_name: 32 bytes en esp_app_desc_t */
    const size_t VER_MAX   = 16;   /* lo que pide el contrato */

    if (!cab || !modelo || !ver || ver_n < 2) return false;
    if (cab_n < PANTALLA_CABECERA || bytes < PANTALLA_CABECERA ||
        bytes > bytes_max) return false;

    /* Little-endian a mano: el .bin viaja byte a byte y no se puede hacer un
     * cast del buffer (alineacion y endianness del compilador). */
    const uint32_t magic = (uint32_t)cab[OFF_MAGIC] |
                           ((uint32_t)cab[OFF_MAGIC + 1] << 8) |
                           ((uint32_t)cab[OFF_MAGIC + 2] << 16) |
                           ((uint32_t)cab[OFF_MAGIC + 3] << 24);
    if (magic != 0xABCD5432u) return false;

    /* project_name: el campo son 32 bytes y TIENE que acabar en NUL dentro de
     * ellos. Sin esta comprobacion, un fichero con 32 letras se compararia
     * contra el modelo leyendo mas alla del campo. */
    const char *proy = (const char *)(cab + OFF_PROY);
    size_t proy_len = 0;
    while (proy_len < PROY_MAX && proy[proy_len] != '\0') proy_len++;
    if (proy_len == PROY_MAX) return false;
    const size_t modelo_len = strlen(modelo);
    if (modelo_len != proy_len || memcmp(proy, modelo, modelo_len) != 0)
        return false;

    /* version: se copia lo que quepa y se valida que sea texto imprimible.
     * El contrato solo exige que empiece por 'v'; lo de imprimible evita que
     * un fichero corrupto meta bytes de control en el JSON de respuesta. */
    size_t ver_len = 0;
    while (ver_len < VER_MAX && cab[OFF_VER + ver_len] != '\0') {
        const unsigned char c = cab[OFF_VER + ver_len];
        if (c < 0x20 || c > 0x7E) return false;
        ver_len++;
    }
    if (ver_len == 0 || cab[OFF_VER] != 'v') return false;
    if (ver_len >= ver_n) ver_len = ver_n - 1;
    memcpy(ver, cab + OFF_VER, ver_len);
    ver[ver_len] = '\0';
    return true;
}

/* ── Utilidades de SD ───────────────────────────────────────────────────────
 * TODO lo que toca la tarjeta va con camera_sd_bus_lock(): el bus es el mismo
 * que usa el GDMA de la camara y pisarlo acaba en "DMA timeout" y tarjeta
 * pillada (ver camera.h). El lock se suelta entre trozos para que la camara
 * tenga su ventana, pero nunca se toca el FILE* fuera de el. */

/* Borra un fichero de la tarjeta. Si el lock no llega en 5 s se borra igual y
 * se avisa: dejarlo seria una fuga de espacio peor que la contencion. */
static void pantalla_borrar(const char *ruta)
{
    const bool got_lock = camera_sd_bus_lock_wait(5000);
    if (!got_lock) ESP_LOGW(TAG, "borro %s sin cerrojo SD tras 5 s", ruta);
    if (remove(ruta) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "no he podido borrar %s (errno=%d)", ruta, errno);
    if (got_lock) camera_sd_bus_unlock();
}

/* Cierra y borra el .tmp en los caminos de error (incluido el de quedarse sin
 * memoria, cuando el fichero ya esta abierto). Siempre deja el FILE* cerrado. */
static void pantalla_cerrar_y_borrar(FILE *f, const char *ruta)
{
    if (f) {
        const bool got_lock = camera_sd_bus_lock_wait(5000);
        if (!got_lock) ESP_LOGW(TAG, "fclose sin cerrojo SD tras 5 s");
        if (fclose(f) != 0) ESP_LOGW(TAG, "fclose fallo en %s", ruta);
        if (got_lock) camera_sd_bus_unlock();
    }
    pantalla_borrar(ruta);
}

/* Hueco libre de la tarjeta, medido como en /api/viaje (esp_vfs_fat_info lee
 * la FAT y por eso va con el cerrojo). Devuelve false si no se puede medir; en
 * ese caso la subida sigue y sera el fwrite el que diga si hay sitio. */
static bool pantalla_hueco_libre(uint64_t *libre_out)
{
    if (!camera_sd_bus_lock(1500)) return false;
    uint64_t total = 0, libre = 0;
    const esp_err_t e = esp_vfs_fat_info("/sdcard", &total, &libre);
    camera_sd_bus_unlock();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no he podido medir el hueco de la tarjeta (%s)",
                 esp_err_to_name(e));
        return false;
    }
    if (libre_out) *libre_out = libre;
    return true;
}

/* Modelo del query string. Se compara contra la tabla en lugar de construir
 * una ruta con lo que llega: asi el cliente no puede salirse de
 * /sdcard/firmware ni pedir un fichero que no sea uno de los dos. */
static const pantalla_modelo_t *pantalla_modelo(const char *nombre)
{
    if (!nombre) return NULL;
    for (size_t i = 0; i < PANTALLA_NUM_MODELOS; i++)
        if (strcmp(s_modelos[i].nombre, nombre) == 0) return &s_modelos[i];
    return NULL;
}

static const pantalla_modelo_t *pantalla_modelo_de_query(httpd_req_t *req)
{
    char query[64] = {0};
    char modelo[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return NULL;
    if (httpd_query_key_value(query, "modelo", modelo, sizeof(modelo)) != ESP_OK)
        return NULL;
    return pantalla_modelo(modelo);
}

/* Respuesta de error corta. 'cerrar' se pone cuando queda cuerpo de la peticion
 * sin leer: sin cerrar la sesion, el httpd intentaria interpretar esos megabytes
 * como la siguiente peticion y el cliente veria un 400 sin sentido (o el
 * servidor se quedaria parseando basura). */
static esp_err_t pantalla_error(httpd_req_t *req, const char *estado,
                                const char *texto, bool cerrar)
{
    httpd_resp_set_status(req, estado);
    if (cerrar) httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const esp_err_t e = httpd_resp_sendstr(req, texto);
    if (cerrar) {
        const int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) httpd_sess_trigger_close(req->handle, fd);
    }
    return e;
}

static bool pantalla_enviar(httpd_req_t *req, const char *buf, size_t n)
{
    int esperas = 0;
    while (n > 0) {
        const int enviado = httpd_send(req, buf, n);
        /* httpd_send puede enviar MENOS de lo pedido (send() parcial). No vale
         * con llamarlo una vez y dar el trozo por bueno. */
        if (enviado == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= PANTALLA_ESPERAS)
            continue;
        if (enviado <= 0) return false;
        esperas = 0;
        buf += enviado;
        n -= (size_t)enviado;
    }
    return true;
}

static void pantalla_sha256_hex(const uint8_t hash[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[hash[i] >> 4];
        out[i * 2 + 1] = hex[hash[i] & 0x0F];
    }
    out[64] = '\0';
}

typedef struct {
    bool     hay;
    char     ver[PANTALLA_VER_N];
    uint64_t bytes;
    char     sha256[65];
} pantalla_fw_info_t;

/* Lee version, tamano y sha256 de un .bin. Devuelve false si no hay fichero, si
 * no es valido para ese modelo o si la tarjeta no deja leerlo.
 *
 * El sha256 es REAL (se recalcula del fichero en cada llamada, como pide el
 * contrato): el cliente se lo pasa a su verificador antes de instalar. Por eso
 * este endpoint es el unico coste del servidor LIGERO: hashear 2 MB de la SD
 * tarda un par de segundos. Se cede la CPU cada 64 KB para no ahogar a LVGL y
 * se suelta el cerrojo entre trozos para que la camara respire. */
static bool pantalla_leer_info(const pantalla_modelo_t *m, pantalla_fw_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!datalogger_sd_montada()) return false;

    struct stat st;
    FILE *f = NULL;
    if (!camera_sd_bus_lock(3000)) {
        ESP_LOGW(TAG, "info %s: tarjeta ocupada", m->nombre);
        return false;
    }
    const bool existe = (stat(m->ruta, &st) == 0 && S_ISREG(st.st_mode) &&
                         st.st_size > 0);
    if (existe && (uint64_t)st.st_size > (uint64_t)PANTALLA_MAX_BYTES) {
        camera_sd_bus_unlock();
        ESP_LOGW(TAG, "info %s: %lld bytes, mas que el tope; no lo anuncio",
                 m->nombre, (long long)st.st_size);
        return false;
    }
    if (existe) f = fopen(m->ruta, "rb");
    camera_sd_bus_unlock();
    if (!f) return false;   /* no hay fichero: no es un error, sale null */

    /* Avisa a "Soltar tarjeta" de que hay un FILE* abierto: desmontar por
     * debajo daria una lectura corta y un sha256 de un fichero truncado. */
    sd_reader_abrir();

    uint8_t cab[PANTALLA_CABECERA] = {0};
    size_t cab_n = 0;
    bool cab_ok = false;
    if (camera_sd_bus_lock(3000)) {
        cab_n = fread(cab, 1, sizeof(cab), f);
        cab_ok = (cab_n == sizeof(cab)) && (ferror(f) == 0);
        camera_sd_bus_unlock();
    }

    char ver[PANTALLA_VER_N] = {0};
    const bool valido = cab_ok &&
        pantalla_fw_valido(cab, cab_n, (size_t)st.st_size, PANTALLA_MAX_BYTES,
                           m->nombre, ver, sizeof(ver));

    uint8_t hash[32] = {0};
    uint64_t total = 0;
    uint8_t *buf = NULL;
    bool hash_ok = false;

    if (valido) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        if (mbedtls_sha256_starts(&ctx, 0) == 0) {
            bool seek_ok = false;
            if (camera_sd_bus_lock(3000)) {
                seek_ok = (fseek(f, 0, SEEK_SET) == 0);
                camera_sd_bus_unlock();
            }
            if (seek_ok) buf = malloc(PANTALLA_CHUNK);
            if (buf) {
                bool leer_ok = true;
                size_t desde_yield = 0;
                while (leer_ok) {
                    bool error = false;
                    size_t n = 0;
                    if (!camera_sd_bus_lock(3000)) { leer_ok = false; break; }
                    n = fread(buf, 1, PANTALLA_CHUNK, f);
                    error = (ferror(f) != 0);
                    camera_sd_bus_unlock();
                    if (error) { leer_ok = false; break; }
                    if (n == 0) break;   /* EOF */
                    if (mbedtls_sha256_update(&ctx, buf, n) != 0) {
                        leer_ok = false;
                        break;
                    }
                    total += n;
                    desde_yield += n;
                    if (desde_yield >= 64 * 1024) {
                        desde_yield = 0;
                        vTaskDelay(1);
                    }
                }
                if (leer_ok && total == (uint64_t)st.st_size &&
                    mbedtls_sha256_finish(&ctx, hash) == 0)
                    hash_ok = true;
            }
        }
        mbedtls_sha256_free(&ctx);
    }

    const bool got_lock = camera_sd_bus_lock_wait(5000);
    if (!got_lock) ESP_LOGW(TAG, "info %s: fclose sin cerrojo SD tras 5 s", m->nombre);
    if (fclose(f) != 0) hash_ok = false;
    if (got_lock) camera_sd_bus_unlock();
    sd_reader_cerrar();
    free(buf);

    if (!hash_ok) {
        ESP_LOGW(TAG, "info %s: no he podido leer/validar el .bin; sale null",
                 m->nombre);
        return false;
    }

    out->hay = true;
    out->bytes = total;
    memcpy(out->ver, ver, sizeof(out->ver));
    out->ver[sizeof(out->ver) - 1] = '\0';
    pantalla_sha256_hex(hash, out->sha256);
    return true;
}

/* ── GET /api/pantalla/info ────────────────────────────────────────────────
 * Servidor LIGERO (80), nivel ABIERTO (como /api/viaje): la cabina solo tiene
 * las credenciales del portal.
 *
 * {"cabina5":{"ver":"v1.30","bytes":2214608,"sha256":"<64 hex>"},"35cabina":...}
 * Un modelo sin fichero sale a null. SD sin montar: 200 con los dos a null y
 * aviso en el log (la cabina tiene que poder preguntar sin que el portal finja
 * que hay una actualizacion). */
esp_err_t ota_pantalla_info(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    cJSON *raiz = cJSON_CreateObject();
    if (!raiz)
        return pantalla_error(req, "500 Internal Server Error",
                              "sin memoria", false);

    if (!datalogger_sd_montada()) {
        ESP_LOGW(TAG, "/api/pantalla/info: SD sin montar; los dos modelos a null");
        for (size_t i = 0; i < PANTALLA_NUM_MODELOS; i++)
            cJSON_AddNullToObject(raiz, s_modelos[i].nombre);
    } else {
        for (size_t i = 0; i < PANTALLA_NUM_MODELOS; i++) {
            pantalla_fw_info_t info;
            if (!pantalla_leer_info(&s_modelos[i], &info)) {
                cJSON_AddNullToObject(raiz, s_modelos[i].nombre);
                continue;
            }
            cJSON *obj = cJSON_CreateObject();
            if (!obj ||
                !cJSON_AddStringToObject(obj, "ver", info.ver) ||
                !cJSON_AddNumberToObject(obj, "bytes", (double)info.bytes) ||
                !cJSON_AddStringToObject(obj, "sha256", info.sha256)) {
                if (obj) cJSON_Delete(obj);
                cJSON_Delete(raiz);
                return pantalla_error(req, "500 Internal Server Error",
                                      "sin memoria", false);
            }
            cJSON_AddItemToObject(raiz, s_modelos[i].nombre, obj);
        }
    }

    char *txt = cJSON_PrintUnformatted(raiz);
    cJSON_Delete(raiz);
    if (!txt)
        return pantalla_error(req, "500 Internal Server Error",
                              "sin memoria", false);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_sendstr(req, txt);
    free(txt);
    return e;
}

/* ── GET /api/pantalla/bin?modelo=<m> ──────────────────────────────────────
 * Servidor PESADO (8081), nivel ABIERTO. Devuelve el fichero ENTERO.
 *
 * POR QUE SE CONSTRUYE LA CABECERA HTTP A MANO: httpd_resp_send_chunk() impone
 * "Transfer-Encoding: chunked" pase lo que pase, y la cabina necesita un
 * Content-Length de verdad para pasarselo a esp_ota_begin. httpd_resp_send()
 * con el fichero entero exigiria meter hasta 4 MB en RAM. httpd_send() es la
 * API publica de respuesta cruda de esp_http_server: se manda la cabecera con
 * Content-Length exacto y despues el cuerpo por trozos. */
esp_err_t ota_pantalla_bin(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    const pantalla_modelo_t *m = pantalla_modelo_de_query(req);
    if (!m)
        return pantalla_error(req, "404 Not Found",
                              "modelo? (cabina5|35cabina)", false);

    if (!datalogger_sd_montada())
        return pantalla_error(req, "404 Not Found",
                              "no hay firmware: la tarjeta no esta montada", false);

    struct stat st;
    FILE *f = NULL;
    if (!camera_sd_bus_lock(3000))
        return pantalla_error(req, "503 Service Unavailable",
                              "tarjeta ocupada, reintenta", false);
    const bool existe = (stat(m->ruta, &st) == 0 && S_ISREG(st.st_mode) &&
                         st.st_size > 0);
    if (existe) f = fopen(m->ruta, "rb");
    camera_sd_bus_unlock();
    if (!f)
        return pantalla_error(req, "404 Not Found",
                              "no hay firmware para ese modelo", false);

    uint8_t *buf = malloc(PANTALLA_CHUNK);
    if (!buf) {
        const bool got_lock = camera_sd_bus_lock_wait(5000);
        if (!got_lock) ESP_LOGW(TAG, "bin %s: fclose sin cerrojo SD tras 5 s", m->nombre);
        fclose(f);
        if (got_lock) camera_sd_bus_unlock();
        return pantalla_error(req, "500 Internal Server Error",
                              "sin memoria para servir el firmware", false);
    }

    sd_reader_abrir();

    char cabecera[160];
    const int hlen = snprintf(cabecera, sizeof(cabecera),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        (unsigned long)st.st_size);
    bool ok = (hlen > 0 && hlen < (int)sizeof(cabecera)) &&
              pantalla_enviar(req, cabecera, (size_t)hlen);

    uint64_t enviado = 0;
    size_t desde_yield = 0;
    while (ok && enviado < (uint64_t)st.st_size) {
        bool error = false;
        size_t n = 0;
        if (!camera_sd_bus_lock(3000)) {
            ESP_LOGW(TAG, "bin %s: tarjeta ocupada, corto el envio", m->nombre);
            ok = false;
            break;
        }
        n = fread(buf, 1, PANTALLA_CHUNK, f);
        error = (ferror(f) != 0);
        camera_sd_bus_unlock();
        if (error) { ok = false; break; }
        if (n == 0) break;   /* se acabo antes de lo que decia st_size */
        if (!pantalla_enviar(req, (const char *)buf, n)) { ok = false; break; }
        enviado += n;
        desde_yield += n;
        if (desde_yield >= 32 * 1024) {
            desde_yield = 0;
            vTaskDelay(1);   /* ceder a LVGL/WiFi: el httpd pesado comparte CPU */
        }
    }
    if (ok && enviado != (uint64_t)st.st_size) {
        ESP_LOGW(TAG, "bin %s: he leido %llu de %lld bytes",
                 m->nombre, (unsigned long long)enviado, (long long)st.st_size);
        ok = false;
    }

    const bool got_lock = camera_sd_bus_lock_wait(5000);
    if (!got_lock) ESP_LOGW(TAG, "bin %s: fclose sin cerrojo SD tras 5 s", m->nombre);
    if (fclose(f) != 0) ESP_LOGW(TAG, "bin %s: fclose fallo", m->nombre);
    if (got_lock) camera_sd_bus_unlock();
    sd_reader_cerrar();
    free(buf);

    /* Si se corto el envio no se puede arreglar: devolver ESP_FAIL cierra la
     * conexion, que es lo unico honesto cuando el cliente ya tiene una parte. */
    return ok ? ESP_OK : ESP_FAIL;
}

/* ── POST /api/pantalla/subir?modelo=<m> ───────────────────────────────────
 * Servidor PESADO (8081), nivel ADMINISTRACION (escribe un firmware que otras
 * pantallas van a instalar: mismo nivel que /ota, no el del portal).
 *
 * El cuerpo se escribe tal cual a <modelo>.bin.tmp SIN cargarlo en RAM. Al
 * final se valida la cabecera capturada y SOLO entonces se publica: si algo
 * falla se borra el .tmp y la version anterior sigue intacta. */
esp_err_t ota_pantalla_subir(httpd_req_t *req)
{
    /* Mismo patron de aviso que 'borrar' de config_server_viaje.c: la respuesta
     * 401/403 ya la manda auth_comprueba, pero con un texto que dice QUE
     * credencial falta (si no, el usuario solo ve un 401 pelado). */
    REQUIRE_AUTH_STRICT_AVISO(req,
        "subir el firmware de una pantalla necesita la clave de administracion "
        "(pantalla de la P4: Ajustes -> Wi-Fi)");

    const pantalla_modelo_t *m = pantalla_modelo_de_query(req);
    if (!m)
        return pantalla_error(req, "400 Bad Request",
                              "modelo? (cabina5|35cabina)", true);

    /* Content-Length OBLIGATORIO: sin el no se puede ni comprobar el tope ni
     * anunciar el tamano, y esp_http_client de la cabina lo manda siempre. */
    char cl[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Length", cl, sizeof(cl)) != ESP_OK)
        return pantalla_error(req, "411 Length Required",
                              "falta Content-Length: sin el no se puede comprobar el tamano",
                              true);

    const size_t len = req->content_len;
    if (len == 0)
        return pantalla_error(req, "400 Bad Request", "cuerpo vacio", true);
    if (len > PANTALLA_MAX_BYTES) {
        char msg[96];
        snprintf(msg, sizeof(msg), "no cabe: el tope es %u bytes (0x3F0000)",
                 (unsigned)PANTALLA_MAX_BYTES);
        return pantalla_error(req, "400 Bad Request", msg, true);
    }
    if (len < PANTALLA_CABECERA)
        return pantalla_error(req, "400 Bad Request",
                              "demasiado pequeno para ser un firmware", true);

    if (!datalogger_sd_montada())
        return pantalla_error(req, "507 Insufficient Storage",
                              "no hay tarjeta SD montada", true);

    /* El directorio puede no existir en una tarjeta nueva; se crea aqui en vez
     * de exigir que el usuario lo prepare por el PC. */
    if (sd_mkdir(PANTALLA_DIR, 0777, 3000) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "no he podido crear %s (errno=%d)", PANTALLA_DIR, errno);
        return pantalla_error(req, "507 Insufficient Storage",
                              "no he podido crear /sdcard/firmware", true);
    }

    char ruta[96];
    char ruta_tmp[104];
    snprintf(ruta, sizeof(ruta), PANTALLA_DIR "/%s.bin", m->nombre);
    snprintf(ruta_tmp, sizeof(ruta_tmp), "%s.tmp", ruta);

    /* Un .tmp de una subida anterior (corte de luz a mitad) no puede contar
     * como espacio ocupado en la comprobacion de hueco ni quedarse ahi para
     * siempre: se borra antes de medir. Si no existe, remove da ENOENT y
     * pantalla_borrar() no dice nada. */
    pantalla_borrar(ruta_tmp);

    uint64_t libre = 0;
    if (pantalla_hueco_libre(&libre) &&
        libre < (uint64_t)len + (64ULL * 1024ULL)) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "no hay hueco: %u bytes y quedan %llu MB libres en la tarjeta",
                 (unsigned)len,
                 (unsigned long long)(libre / (1024ULL * 1024ULL)));
        return pantalla_error(req, "507 Insufficient Storage", msg, true);
    }

    if (!camera_sd_bus_lock(3000))
        return pantalla_error(req, "507 Insufficient Storage",
                              "tarjeta ocupada, reintenta", true);
    FILE *f = fopen(ruta_tmp, "wb");
    camera_sd_bus_unlock();
    if (!f) {
        ESP_LOGE(TAG, "no he podido crear %s", ruta_tmp);
        return pantalla_error(req, "507 Insufficient Storage",
                              "no he podido crear el fichero temporal en la tarjeta",
                              true);
    }
    sd_reader_abrir();

    uint8_t *buf = malloc(PANTALLA_CHUNK);
    if (!buf) {
        pantalla_cerrar_y_borrar(f, ruta_tmp);
        sd_reader_cerrar();
        return pantalla_error(req, "507 Insufficient Storage",
                              "sin memoria para recibir el firmware", true);
    }

    /* Estados de la recepcion: el codigo de respuesta depende de que fallo.
     * 1 = se corto el cliente; 2 = la tarjeta no pudo escribir. */
    enum { SUBIR_BIEN = 0, SUBIR_CORTADA = 1, SUBIR_TARJETA = 2 };
    int estado = SUBIR_BIEN;

    uint8_t cab[PANTALLA_CABECERA] = {0};
    size_t cab_n = 0;
    size_t restante = len;
    int esperas = 0;
    const int64_t t0 = esp_timer_get_time();

    while (restante > 0) {
        if (esp_timer_get_time() - t0 > PANTALLA_PLAZO_US) {
            ESP_LOGE(TAG, "subida %s: plazo agotado con %u bytes por recibir",
                     m->nombre, (unsigned)restante);
            estado = SUBIR_CORTADA;
            break;
        }
        const size_t pedir = restante < PANTALLA_CHUNK ? restante : PANTALLA_CHUNK;
        const int leido = httpd_req_recv(req, (char *)buf, pedir);
        if (leido == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= PANTALLA_ESPERAS)
            continue;
        if (leido <= 0) {
            ESP_LOGE(TAG, "subida %s: se corto con %u bytes por recibir",
                     m->nombre, (unsigned)restante);
            estado = SUBIR_CORTADA;
            break;
        }
        esperas = 0;

        /* Los primeros 112 bytes son la cabecera que se valida al final: se
         * guardan en RAM (112 B), no el cuerpo entero. */
        if (cab_n < sizeof(cab)) {
            size_t copiar = sizeof(cab) - cab_n;
            if (copiar > (size_t)leido) copiar = (size_t)leido;
            memcpy(cab + cab_n, buf, copiar);
            cab_n += copiar;
        }

        if (!camera_sd_bus_lock(3000)) {
            ESP_LOGE(TAG, "subida %s: tarjeta ocupada, corto", m->nombre);
            estado = SUBIR_TARJETA;
            break;
        }
        const size_t escrito = fwrite(buf, 1, (size_t)leido, f);
        camera_sd_bus_unlock();
        if (escrito != (size_t)leido) {
            ESP_LOGE(TAG, "subida %s: fwrite %u de %d bytes",
                     m->nombre, (unsigned)escrito, leido);
            estado = SUBIR_TARJETA;
            break;
        }

        restante -= (size_t)leido;
        /* Ceder la CPU tras cada trozo: el httpd pesado y LVGL van a la misma
         * prioridad y sin esto la UI se queda sin latido mientras sube un .bin. */
        vTaskDelay(1);
    }

    /* Cierre con volcado real. Si se corto la subida no se hace fsync: no hay
     * nada bueno que asegurar y el .tmp se borra igual. */
    const bool cerrar_lock = camera_sd_bus_lock_wait(5000);
    if (!cerrar_lock)
        ESP_LOGW(TAG, "subida %s: cierro el .tmp sin cerrojo SD tras 5 s", m->nombre);
    if (estado == SUBIR_BIEN) {
        if (fflush(f) != 0) {
            ESP_LOGE(TAG, "subida %s: fflush fallo (tarjeta llena?)", m->nombre);
            estado = SUBIR_TARJETA;
        }
        if (fsync(fileno(f)) != 0) {
            ESP_LOGE(TAG, "subida %s: fsync fallo (tarjeta llena?)", m->nombre);
            estado = SUBIR_TARJETA;
        }
    }
    if (fclose(f) != 0 && estado == SUBIR_BIEN) estado = SUBIR_TARJETA;
    if (cerrar_lock) camera_sd_bus_unlock();
    sd_reader_cerrar();
    free(buf);

    if (estado != SUBIR_BIEN) {
        pantalla_borrar(ruta_tmp);
        if (estado == SUBIR_CORTADA)
            return pantalla_error(req, "400 Bad Request",
                "la subida se ha cortado; no se ha cambiado nada", true);
        return pantalla_error(req, "507 Insufficient Storage",
            "no he podido escribir el firmware en la tarjeta (llena?)", true);
    }

    /* LA VALIDACION VA AL FINAL, con lo que de verdad se ha recibido. Si no
     * pasa, el .tmp se borra y <modelo>.bin sigue siendo la version anterior:
     * una cabina nunca ve un fichero a medias. */
    char ver[PANTALLA_VER_N] = {0};
    if (!pantalla_fw_valido(cab, cab_n, len, PANTALLA_MAX_BYTES, m->nombre,
                            ver, sizeof(ver))) {
        pantalla_borrar(ruta_tmp);
        ESP_LOGW(TAG, "subida %s: cabecera invalida (%u bytes)", m->nombre,
                 (unsigned)len);
        return pantalla_error(req, "400 Bad Request",
            "no es un firmware valido para ese modelo (magic, proyecto o version)",
            false);
    }

    /* Respuesta preparada ANTES de publicar: si falta memoria, mejor enterarse
     * aqui (y dejar la version anterior intacta) que despues de haber quitado
     * el fichero bueno. */
    cJSON *resp = cJSON_CreateObject();
    char *txt = NULL;
    if (resp && cJSON_AddBoolToObject(resp, "ok", true) &&
        cJSON_AddStringToObject(resp, "ver", ver) &&
        cJSON_AddNumberToObject(resp, "bytes", (double)len))
        txt = cJSON_PrintUnformatted(resp);
    if (resp) cJSON_Delete(resp);
    if (!txt) {
        pantalla_borrar(ruta_tmp);
        return pantalla_error(req, "507 Insufficient Storage",
                              "sin memoria para preparar la respuesta", false);
    }

    /* PUBLICAR. OJO: el rename() de FATFS NO sobrescribe (f_rename devuelve
     * FR_EXIST si el destino ya existe), asi que primero se quita la version
     * anterior. El hueco entre remove y rename son milisegundos y el .tmp ya
     * esta validado: si se va la luz justo ahi queda el .tmp completo y ninguna
     * version publicada, que es un estado SEGURO (la cabina ve "no hay
     * actualizacion", nunca un binario a medias). */
    bool publicado = false;
    if (camera_sd_bus_lock(3000)) {
        struct stat st_viejo;
        const bool hay_viejo = (stat(ruta, &st_viejo) == 0);
        const bool borrado_ok = (!hay_viejo) || (remove(ruta) == 0);
        if (borrado_ok && rename(ruta_tmp, ruta) == 0) publicado = true;
        camera_sd_bus_unlock();
    } else {
        ESP_LOGW(TAG, "subida %s: tarjeta ocupada al publicar", m->nombre);
    }

    if (!publicado) {
        free(txt);
        pantalla_borrar(ruta_tmp);
        ESP_LOGE(TAG, "subida %s: no he podido publicar %s", m->nombre, ruta);
        return pantalla_error(req, "507 Insufficient Storage",
            "no he podido publicar el firmware; vuelve a intentarlo", false);
    }

    ESP_LOGI(TAG, "firmware %s publicado: %s (%u bytes)",
             m->nombre, ver, (unsigned)len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_sendstr(req, txt);
    free(txt);
    return e;
}
