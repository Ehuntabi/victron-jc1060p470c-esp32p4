#include "log_cleanup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "camera.h"          /* camera_sd_bus_lock: serializar el barrido con la camara */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>        /* rmdir: borrar la carpeta de una sesion de vigilancia */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "log_cleanup";

/* Carpetas de LOGS DIARIOS (ficheros AAAA-MM-DD.csv). Solo entran aqui las que
 * son datos regenerables y con ese nombre: parse_csv_date() exige el formato
 * exacto, asi que meter una carpeta con otro patron no borraria nada.
 *
 * NO estan ni /sdcard/viajes ni /sdcard/vehiculo ni /sdcard/config_backup, y no
 * es un olvido: eso es lo que el usuario quiere conservar.
 *
 * ne185v se anadio el 24-ago-2026 auditando: escribe una linea por minuto en
 * /sdcard/ne185v/AAAA-MM-DD.csv desde que se activo el registro de voltajes, y
 * no lo limpiaba nadie. */
static const char *DIRS[] = { "/sdcard/frigo", "/sdcard/bateria", "/sdcard/ne185v" };
#define NUM_DIRS (sizeof(DIRS)/sizeof(DIRS[0]))

/* Parsea YYYY-MM-DD.csv y devuelve epoch a las 00:00 de ese dia, o 0 si no parsea */
static time_t parse_csv_date(const char *fname)
{
    int y = 0, mo = 0, d = 0, n = 0;
    /* %n captura cuantos chars consumio; solo se alcanza si el literal ".csv"
     * casa. Exigimos ademas que ".csv" sea el final EXACTO del nombre para no
     * borrar ".txt", ".csv.bak" ni nombres sin extension. */
    if (sscanf(fname, "%4d-%2d-%2d.csv%n", &y, &mo, &d, &n) != 3) return 0;
    if (n == 0 || fname[n] != '\0') return 0;
    if (y < 2024 || y > 2100) return 0;
    if (mo < 1 || mo > 12) return 0;
    if (d < 1 || d > 31) return 0;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    return mktime(&tm);
}

/* Procesa un directorio. Si dry_run, solo cuenta los que serian borrados/avisados.
   threshold_delete: borrar si fecha < (now - max_days * 86400)
   threshold_warn: avisar si fecha < (now - (max_days - 1) * 86400) y >= threshold_delete */
static int process_dir(const char *dir, int max_days, bool dry_run, bool count_warn)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        ESP_LOGW(TAG, "RTC sin fecha valida, abortando limpieza");
        return 0;
    }
    /* Proteccion: hoy y ayer NUNCA se borran aunque max_days sea 1,
     * para evitar carreras con datalogger_flush / bh_flush en curso. */
    int effective_max = (max_days < 2) ? 2 : max_days;
    time_t cutoff_delete = now - (time_t)effective_max * 86400;
    time_t cutoff_warn   = now - (time_t)(effective_max - 1) * 86400;

    /* Timeout corto: los callbacks que llaman aqui (daily_cleanup_cb,
     * initial_cleanup_cb) corren en la tarea esp_timer compartida; un lock
     * largo retrasaria TODOS los demas timers del firmware. Si no se
     * consigue, saltar este ciclo (se reintenta en el siguiente disparo). */
    if (!camera_sd_bus_lock(200)) {
        return 0;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        camera_sd_bus_unlock();
        ESP_LOGD(TAG, "%s no abre (probablemente no montado)", dir);
        return 0;
    }
    int hits = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        time_t ftime = parse_csv_date(de->d_name);
        if (ftime == 0) continue;
        char full_path[300];
        snprintf(full_path, sizeof full_path, "%s/%s", dir, de->d_name);
        if (count_warn) {
            /* Avisar: fecha mas antigua que cutoff_warn pero NO la suficiente para cutoff_delete (por si hay solapamiento) */
            if (ftime < cutoff_warn && ftime >= cutoff_delete) {
                hits++;
            }
        } else {
            /* Modo borrar */
            if (ftime < cutoff_delete) {
                if (!dry_run) {
                    if (remove(full_path) == 0) {
                        ESP_LOGI(TAG, "Borrado %s (antiguedad > %d dias)", full_path, max_days);
                        hits++;
                    } else {
                        ESP_LOGW(TAG, "No se pudo borrar %s", full_path);
                    }
                } else {
                    hits++;
                }
            }
        }
    }
    closedir(dp);
    camera_sd_bus_unlock();
    return hits;
}

/* ── Fotos de vigilancia ──────────────────────────────────────────────────
 *
 * No son ficheros diarios sino CARPETAS DE SESION: cada vez que se activa el
 * modo ausente se crea "/sdcard/vigilancia/AAAAMMDD_HHMMSS/" con hasta 300
 * .jpg dentro (unos 60-120 KB cada uno, o sea hasta ~35 MB por sesion). El tope
 * es POR SESION y las sesiones no tienen limite: nada las borraba nunca, y la
 * tarjeta acabaria llena -- y con ella el cuaderno de viaje (auditoria del
 * 24-ago-2026; retencion elegida por el usuario: 60 dias, la misma que el
 * resto).
 *
 * Son pruebas de un allanamiento, asi que 60 dias y no menos: si a los dos
 * meses no las has mirado, ya no las vas a mirar. */
#define VIG_DIR "/sdcard/vigilancia"

static time_t parse_sesion_date(const char *nombre)
{
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0, n = 0;
    if (sscanf(nombre, "%4d%2d%2d_%2d%2d%2d%n", &y, &mo, &d, &hh, &mi, &ss, &n) != 6)
        return 0;
    if (n == 0 || nombre[n] != '\0') return 0;   /* el nombre es EXACTAMENTE eso */
    if (y < 2024 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = hh; tm.tm_min = mi; tm.tm_sec = ss;
    return mktime(&tm);
}

static int borrar_sesiones_vigilancia(int max_days)
{
    time_t now = time(NULL);
    if (now < 1700000000) return 0;          /* sin fecha fiable no se borra nada */
    int effective_max = (max_days < 2) ? 2 : max_days;
    time_t cutoff = now - (time_t)effective_max * 86400;

    if (!camera_sd_bus_lock(2000)) {
        ESP_LOGW(TAG, "vigilancia: tarjeta ocupada, lo dejo para la proxima");
        return 0;
    }
    DIR *dp = opendir(VIG_DIR);
    if (!dp) { camera_sd_bus_unlock(); return 0; }

    int borradas = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        time_t fecha = parse_sesion_date(ent->d_name);
        if (fecha == 0 || fecha >= cutoff) continue;

        char sesion[128];
        snprintf(sesion, sizeof(sesion), "%s/%s", VIG_DIR, ent->d_name);

        /* Una carpeta no se borra con contenido: primero los .jpg. */
        DIR *sd = opendir(sesion);
        if (sd) {
            struct dirent *f;
            while ((f = readdir(sd)) != NULL) {
                if (f->d_name[0] == '.') continue;
                char ruta[256];
                snprintf(ruta, sizeof(ruta), "%s/%s", sesion, f->d_name);
                remove(ruta);
            }
            closedir(sd);
        }
        if (rmdir(sesion) == 0) {
            borradas++;
            ESP_LOGI(TAG, "vigilancia: borrada la sesion %s (mas de %d dias)",
                     ent->d_name, effective_max);
        } else {
            ESP_LOGW(TAG, "vigilancia: no he podido borrar %s", sesion);
        }
    }
    closedir(dp);
    camera_sd_bus_unlock();
    return borradas;
}

int log_cleanup_run_now(int max_days_keep)
{
    int total = 0;
    for (size_t i = 0; i < NUM_DIRS; ++i) {
        total += process_dir(DIRS[i], max_days_keep, false, false);
    }
    total += borrar_sesiones_vigilancia(max_days_keep);
    if (total > 0) ESP_LOGI(TAG, "Borrados %d ficheros antiguos", total);
    return total;
}

int log_cleanup_files_pending_warning(int max_days_keep)
{
    int total = 0;
    for (size_t i = 0; i < NUM_DIRS; ++i) {
        total += process_dir(DIRS[i], max_days_keep, true, true);
    }
    return total;
}

/* Timer diario (24h) que ejecuta limpieza */
static int s_max_days_cached = 60;
static esp_timer_handle_t s_daily_timer = NULL;

static void daily_cleanup_cb(void *arg)
{
    log_cleanup_run_now(s_max_days_cached);
}

/* Primer barrido tras 5s del boot */
static void initial_cleanup_cb(void *arg)
{
    log_cleanup_run_now(s_max_days_cached);
}

void log_cleanup_init(int max_days_keep)
{
    s_max_days_cached = max_days_keep;
    /* Barrido inicial a los 5s */
    esp_timer_handle_t init_t;
    esp_timer_create_args_t a1 = {
        .callback = initial_cleanup_cb,
        .name = "logclean_init"
    };
    if (esp_timer_create(&a1, &init_t) == ESP_OK) {
        esp_timer_start_once(init_t, 5 * 1000000ULL);
    }
    /* Tarea diaria */
    esp_timer_create_args_t a2 = {
        .callback = daily_cleanup_cb,
        .name = "logclean_daily"
    };
    if (esp_timer_create(&a2, &s_daily_timer) == ESP_OK) {
        /* Cada 24h = 86400 * 1000000 us */
        esp_timer_start_periodic(s_daily_timer, 86400ULL * 1000000ULL);
    }
    ESP_LOGI(TAG, "log_cleanup inicializado (max_days=%d)", max_days_keep);
}
