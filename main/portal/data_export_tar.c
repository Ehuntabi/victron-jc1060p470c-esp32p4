/* data_export_tar.c — streaming .tar de las carpetas de /sdcard (frigo,
 * bateria, solar, capturas, vigilancia, config, logs, viaje) para el portal
 * web. Extraido de config_server.c (2026-08-08). */

static const char *TAG = "cfg_srv";

#include "config_server_internal.h"
#include "data_export_tar.h"
#include "camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* === TAR streaming === */
/* Header TAR USTAR de 512 bytes */
#define TAR_BLOCK 512

static unsigned tar_checksum(const uint8_t *header)
{
    unsigned sum = 0;
    /* Cuando se calcula el checksum, los 8 bytes del campo checksum cuentan como espacios */
    for (int i = 0; i < TAR_BLOCK; ++i) {
        if (i >= 148 && i < 156) sum += ' ';
        else sum += header[i];
    }
    return sum;
}

static void tar_write_octal(char *dst, size_t len, uint64_t val)
{
    /* Octal ASCII alineado a la derecha, terminado en 0 */
    memset(dst, '0', len - 1);
    dst[len - 1] = 0;
    int i = (int)len - 2;
    while (val > 0 && i >= 0) {
        dst[i--] = '0' + (val & 7);
        val >>= 3;
    }
}

/* Construye un header TAR USTAR para un fichero. mtime epoch. */
static void tar_build_header(uint8_t *hdr, const char *name, size_t size, time_t mtime)
{
    memset(hdr, 0, TAR_BLOCK);
    /* name: campo de 100 bytes. El llamante ya descarta los nombres que no caben
     * (antes se copiaban cortados a 99 y el .tar salia con un nombre falso, o con
     * dos entradas iguales si dos ficheros compartian los primeros 99). memcpy
     * sobre el hdr ya puesto a cero: no hace falta terminador. 2026-07-26. */
    memcpy(&hdr[0], name, strlen(name));
    /* mode 0644 */
    tar_write_octal((char*)&hdr[100], 8, 0644);
    /* uid/gid 0 */
    tar_write_octal((char*)&hdr[108], 8, 0);
    tar_write_octal((char*)&hdr[116], 8, 0);
    /* size */
    tar_write_octal((char*)&hdr[124], 12, (uint64_t)size);
    /* mtime */
    tar_write_octal((char*)&hdr[136], 12, (uint64_t)mtime);
    /* checksum placeholder spaces */
    memset(&hdr[148], ' ', 8);
    /* typeflag '0' regular file */
    hdr[156] = '0';
    /* magic ustar */
    memcpy(&hdr[257], "ustar", 5);
    memcpy(&hdr[263], "00", 2);
    /* checksum */
    unsigned sum = tar_checksum(hdr);
    char chk[8];
    snprintf(chk, sizeof chk, "%06o", sum);
    memcpy(&hdr[148], chk, 7);
    hdr[155] = ' ';
}

/* Vuelca el contenido de src_dir dentro de un .tar ya empezado. Si prefix no es
 * NULL, las entradas salen como "<prefix>/<fichero>": eso es lo que hace que el
 * paquete de viaje traiga ya las carpetas dentro.
 *
 * Devuelve false si hubo que CORTAR el paquete a medias (envio fallido, lectura
 * corta o tarjeta ocupada). El llamante no debe mandar entonces el bloque final:
 * un .tar al que le falta contenido SIGUE pareciendo valido si lleva su cierre, y
 * el analizador del PC se traga un viaje incompleto creyendo que esta entero.
 * Una carpeta ausente NO es un corte: aporta cero entradas y se sigue. */
static bool tar_stream_dir(httpd_req_t *req, const char *src_dir,
                           const char *prefix, char *buf,
                           size_t *bytes_since_yield)
{
    /* Sin cerrojo NO se toca la tarjeta. Antes se hacia la operacion igual
     * cuando caducaba, y eso pisa el GDMA de la camara: es la receta del
     * "DMA timeout 0x107" y la SD pillada. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) return false;
    DIR *dp = opendir(src_dir);
    camera_sd_bus_unlock();
    if (!dp) return true;   /* carpeta ausente = aporta cero entradas, no es un error */

    uint8_t hdr[TAR_BLOCK];
    bool ok = true;
    struct dirent *de;
    while (1) {
        /* readdir lee sectores de dir: serializar con el GDMA de la camara */
        if (!camera_sd_bus_lock(3000)) { ok = false; break; }
        de = readdir(dp);
        camera_sd_bus_unlock();
        if (de == NULL) break;
        if (de->d_type == DT_DIR) continue;

        /* El campo "name" del ustar son 100 bytes, y con prefijo la entrada es
         * "<prefix>/<fichero>". Lo que no quepa se OMITE en vez de servirlo con
         * el nombre cortado (dos ficheros que compartan el principio saldrian
         * con el mismo nombre). Los ficheros de la pantalla son
         * "AAAA-MM-DD.csv": esto no salta nunca en la practica. 2026-07-26. */
        char entry[100];
        int elen = prefix ? snprintf(entry, sizeof entry, "%s/%s", prefix, de->d_name)
                          : snprintf(entry, sizeof entry, "%s", de->d_name);
        if (elen < 0 || elen >= (int)sizeof entry) {
            ESP_LOGW(TAG, "nombre demasiado largo para el tar, se omite: %s", de->d_name);
            continue;
        }

        char full_path[400];
        snprintf(full_path, sizeof full_path, "%s/%s", src_dir, de->d_name);

        /* Abrir ANTES de anunciar la cabecera: si se anunciaban N bytes y luego
         * fallaba el fopen, esos N bytes no llegaban nunca y todo lo que venia
         * detras quedaba desalineado -> paquete corrupto con aspecto de bueno. */
        struct stat st;
        if (!camera_sd_bus_lock(3000)) { ok = false; break; }
        int sr = stat(full_path, &st);
        FILE *f = (sr == 0 && st.st_size > 0) ? fopen(full_path, "rb") : NULL;
        camera_sd_bus_unlock();
        if (!f) continue;

        tar_build_header(hdr, entry, st.st_size, st.st_mtime);
        if (httpd_resp_send_chunk(req, (const char*)hdr, TAR_BLOCK) != ESP_OK) ok = false;

        /* Contenido en chunks de 2KB. Cada op de SD bajo el cerrojo; se SUELTA
         * en el envio de red para que la camara interleave. */
        size_t remaining = ok ? (size_t)st.st_size : 0;
        while (remaining > 0) {
            size_t to_read = remaining > 2048 ? 2048 : remaining;
            if (!camera_sd_bus_lock(3000)) { ok = false; break; }
            size_t n = fread(buf, 1, to_read, f);
            camera_sd_bus_unlock();
            /* Lectura corta: la cabecera ya anuncio st.st_size y no hay forma de
             * cuadrarlo. Cortar es lo unico honesto. */
            if (n == 0) { ok = false; break; }
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { ok = false; break; }
            remaining -= n;
            *bytes_since_yield += n;
            if (*bytes_since_yield >= 32768) {
                *bytes_since_yield = 0;
                vTaskDelay(1);   /* ceder CPU a LVGL/WDT/WiFi (evita el reset por asfixia) */
            }
        }
        /* El fclose SI tiene que ocurrir (dejarlo abierto seria una fuga), asi
         * que aqui se espera al cerrojo en vez de saltarselo. */
        while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
        fclose(f);
        camera_sd_bus_unlock();
        if (!ok) break;

        /* Padding hasta multiplo de 512 */
        size_t pad = (TAR_BLOCK - (st.st_size % TAR_BLOCK)) % TAR_BLOCK;
        if (pad > 0) {
            uint8_t zero[TAR_BLOCK] = {0};
            if (httpd_resp_send_chunk(req, (const char*)zero, pad) != ESP_OK) {
                ok = false;
                break;
            }
        }
    }
    closedir(dp);
    return ok;
}

/* Empaqueta N carpetas en un solo .tar. prefixes puede ser NULL: entonces las
 * entradas van sin carpeta, que es como se han servido siempre los paquetes por
 * tema. */
static esp_err_t tar_send_dirs(httpd_req_t *req, const char *attach_name,
                               const char *const *dirs, const char *const *prefixes,
                               int n)
{
    /* Se tantea el cerrojo ANTES de anunciar nada para poder contestar un 503
     * legible. Una vez empezado el .tar ya no hay forma de decirlo: lo unico que
     * queda es cortar la conexion. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "La tarjeta esta ocupada. Prueba otra vez en unos segundos.");
        return ESP_OK;
    }
    camera_sd_bus_unlock();

    httpd_resp_set_type(req, "application/x-tar");
    char disp[160];
    /* Construir manualmente para evitar warning de format-truncation */
    strcpy(disp, "attachment; filename=");
    strncat(disp, attach_name, sizeof(disp) - strlen(disp) - 1);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    /* Ceder CPU cada ~32 KB enviados: httpd (prio 5) es mas prioritario que
     * LVGL (prio 4); sin esto, un .tar grande ahoga la UI y el watchdog SW la da
     * por colgada (3/3) -> esp_restart. Ademas deja respirar a la pila WiFi
     * (evita el drop de conexion en descargas medianas). El contador es comun a
     * todas las carpetas: lo que asfixia es el total enviado, no cada carpeta. */
    size_t bytes_since_yield = 0;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        ok = tar_stream_dir(req, dirs[i], prefixes ? prefixes[i] : NULL,
                            buf, &bytes_since_yield);
    }
    free(buf);

    /* Si se corto NO se manda el bloque final: sin el, el cliente ve la descarga
     * cortada y no la da por buena. Devolver ESP_FAIL cierra la conexion. */
    if (!ok) return ESP_FAIL;

    /* Final TAR: dos bloques de zeros */
    uint8_t zeros[TAR_BLOCK * 2] = {0};
    httpd_resp_send_chunk(req, (const char*)zeros, sizeof zeros);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Para carpetas RAIZ cuyo contenido son subcarpetas (p.ej. /sdcard/vigilancia
 * con una subcarpeta por sesion, o /sdcard/logs con una por dia): tar_stream_dir
 * SALTA subcarpetas (DT_DIR), asi que sin esto el .tar saldria vacio en cuanto
 * el contenido este organizado en subcarpetas. Escanea root_dir, arma las
 * listas dirs[]/prefixes[] dinamicamente (una entrada por subcarpeta, con
 * prefijo "<root_name>/<subcarpeta>") y reutiliza tar_send_dirs tal cual. */
/* 256 de margen: no hay rotacion de sesiones de vigilancia (se acumulan sin
 * limite), asi que un cap bajo se alcanzaria con el uso normal del dispositivo
 * y truncaria el .tar SIN avisar. dirs[]/prefixes[] van al heap (no a la pila
 * del worker httpd) para poder permitirse este margen sin arriesgar su stack. */
#define TAR_MAX_SUBDIRS 256
typedef struct { char dir[192]; char prefix[128]; } tar_subdir_entry_t;

static esp_err_t handle_tar_dir_of_subdirs(httpd_req_t *req, const char *root_dir,
                                           const char *root_name, const char *attach_name)
{
    tar_subdir_entry_t *entries = malloc(sizeof(tar_subdir_entry_t) * TAR_MAX_SUBDIRS);
    const char **dirs = malloc(sizeof(char *) * TAR_MAX_SUBDIRS);
    const char **prefixes = malloc(sizeof(char *) * TAR_MAX_SUBDIRS);
    if (!entries || !dirs || !prefixes) {
        free(entries); free(dirs); free(prefixes);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int n = 0;
    bool truncated = false;
    if (camera_sd_bus_lock(3000)) {
        DIR *dp = opendir(root_dir);
        camera_sd_bus_unlock();
        if (dp) {
            struct dirent *de;
            for (;;) {
                if (!camera_sd_bus_lock(1000)) break;
                de = readdir(dp);
                camera_sd_bus_unlock();
                if (!de) break;
                if (de->d_name[0] == '.') continue;
                if (n == TAR_MAX_SUBDIRS) { truncated = true; continue; }   /* seguir leyendo por si acaba pronto, solo para el log */
                snprintf(entries[n].dir, sizeof(entries[n].dir), "%s/%s", root_dir, de->d_name);
                snprintf(entries[n].prefix, sizeof(entries[n].prefix), "%s/%s", root_name, de->d_name);
                dirs[n] = entries[n].dir;
                prefixes[n] = entries[n].prefix;
                n++;
            }
            while (!camera_sd_bus_lock(1000)) vTaskDelay(1);
            closedir(dp);
            camera_sd_bus_unlock();
        }
    }
    if (truncated) {
        ESP_LOGW(TAG, "%s: %s tiene mas de %d subcarpetas, el .tar se corta (quedan fuera algunas)",
                 attach_name, root_dir, TAR_MAX_SUBDIRS);
    }

    esp_err_t err = tar_send_dirs(req, attach_name, dirs, prefixes, n);
    free(entries); free(dirs); free(prefixes);
    return err;
}

static esp_err_t handle_tar_dir(httpd_req_t *req, const char *src_dir, const char *attach_name)
{
    const char *const dirs[1] = { src_dir };
    return tar_send_dirs(req, attach_name, dirs, NULL, 1);
}

esp_err_t handle_data_frigo_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/frigo", "frigo.tar");
}

esp_err_t handle_data_bateria_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/bateria", "bateria.tar");
}

esp_err_t handle_data_solar_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/solar", "solar.tar");
}

/* Paquete para el analizador del PC: bateria + solar + frigo con las carpetas
 * ya puestas dentro, para poder montar un viaje sin sacar la tarjeta.
 * Las fotos de vigilancia NO van aqui a proposito: pesan mucho y tienen su
 * propio paquete. */
esp_err_t handle_data_viaje_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    static const char *const dirs[]     = { "/sdcard/bateria", "/sdcard/solar", "/sdcard/frigo" };
    static const char *const prefixes[] = { "bateria",         "solar",         "frigo"         };
    return tar_send_dirs(req, "viaje.tar", dirs, prefixes, 3);
}

esp_err_t handle_data_capturas_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/screenshots", "capturas.tar");
}

esp_err_t handle_data_vigilancia_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir_of_subdirs(req, "/sdcard/vigilancia", "vigilancia", "vigilancia.tar");
}

esp_err_t handle_data_config_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir(req, "/sdcard/config_backup", "config.tar");
}

/* Logs de sistema: /sdcard/logs/<YYYYMMDD>/log_*.txt y crash_*.txt, una
 * subcarpeta por dia. */
esp_err_t handle_data_logs_tar(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return handle_tar_dir_of_subdirs(req, "/sdcard/logs", "logs", "logs.tar");
}
