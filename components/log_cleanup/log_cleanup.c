#include "log_cleanup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "camera.h"          /* camera_sd_bus_lock: serializar el barrido con la camara */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>        /* rmdir: borrar la carpeta de una sesion de vigilancia */
#include "ff.h"            /* f_getfree: espacio que queda en la tarjeta (no hay statvfs en IDF) */
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

/* Retencion propia de las sesiones de vigilancia (0 = la misma que los datos).
   Vive aqui arriba porque log_cleanup_run_now() la consulta. */
static int s_max_days_vig = 0;

/* ── Espacio libre ──────────────────────────────────────────────────────────
 * La retencion por dias no basta: una temporada de salidas largas puede llenar la
 * tarjeta antes de que venza el plazo, y con la tarjeta llena dejan de guardarse
 * TANTO la vigilancia COMO los datos (bateria, frigo, viaje), con el unico aviso
 * en el log serie. Si al hacer el barrido diario queda menos de VIG_LIBRE_MIN_MB,
 * se borran las sesiones de vigilancia mas antiguas (solo esas: ni viajes ni
 * datos) hasta recuperar VIG_LIBRE_OBJETIVO_MB. */
#define VIG_LIBRE_MIN_MB       300
#define VIG_LIBRE_OBJETIVO_MB  600

/* Nombre de la sesion que se esta grabando ahora mismo, para no borrarla (cadena
 * vacia si no hay vigilancia puesta). */
static void sesion_en_curso(char *out, size_t n)
{
    out[0] = '\0';
    char s[24];
    if (camera_vig_sesion_actual(s, sizeof(s))) {
        snprintf(out, n, "%s", s);
    }
}

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

    /* Timeout corto para no acaparar el bus: daily_cleanup_cb/initial_cleanup_cb
     * ya no llaman aqui directamente (solo notifican a cleanup_task, su propia
     * tarea dedicada), pero la camara y el resto de escritores de SD siguen
     * esperando el mismo cerrojo. Si no se consigue, saltar este ciclo (se
     * reintenta en el siguiente disparo). */
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
 * 24-ago-2026; retencion elegida por el usuario: 60 dias (los datos pasaron a
 * 120 el 18-sep-2026 -- ver log_cleanup_set_vigilancia_days). Antes era la misma que el
 * resto).
 *
 * Son pruebas de un allanamiento, asi que 60 dias y no menos: si a los dos
 * meses no las has mirado, ya no las vas a mirar. */
#define VIG_DIR "/sdcard/vigilancia"
/* Las miniaturas de esas mismas capturas (mismo esquema sesion/fichero.jpg).
 * Hasta el 18-sep-2026 NO las limpiaba nadie: crecian para siempre. */
#define VIG_THUMBS_DIR "/sdcard/vigilancia_thumbs"

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

/* Borra una sesion (o carpeta de dia) y todo lo que tiene dentro. Una carpeta no
 * se borra con contenido: primero los ficheros.
 *
 * El cerrojo del bus SD se toma y se suelta en cada paso (abrir, leer el nombre,
 * borrar): una sesion puede tener 300 ficheros y tener el bus tomado todo el
 * rato asfixiaria la ventana del GDMA de la camara -> INT WDT. False si al final
 * la carpeta sigue ahi. El nombre tiene que venir ya validado por
 * parse_sesion_date(). */
static bool borra_sesion(const char *dir, const char *nombre)
{
    char sesion[128];
    snprintf(sesion, sizeof(sesion), "%s/%s", dir, nombre);

    if (!camera_sd_bus_lock(2000)) return false;
    DIR *sd = opendir(sesion);
    camera_sd_bus_unlock();
    if (sd) {
        for (;;) {
            char nom[128] = "";
            if (!camera_sd_bus_lock(1000)) break;
            struct dirent *f = readdir(sd);
            if (f && f->d_name[0] != '.') snprintf(nom, sizeof(nom), "%s", f->d_name);
            camera_sd_bus_unlock();
            if (!f) break;
            if (nom[0] == '\0') continue;

            char ruta[256];
            snprintf(ruta, sizeof(ruta), "%s/%s", sesion, nom);
            if (camera_sd_bus_lock(2000)) { remove(ruta); camera_sd_bus_unlock(); }
            vTaskDelay(pdMS_TO_TICKS(2));   /* ceder a la camara entre ficheros */
        }
        if (camera_sd_bus_lock_wait(5000)) { closedir(sd); camera_sd_bus_unlock(); }
        else { closedir(sd); ESP_LOGW(TAG, "closedir sin cerrojo SD tras 5s (%s)", sesion); }
    }

    if (!camera_sd_bus_lock(2000)) return false;
    const bool ok = (rmdir(sesion) == 0);
    camera_sd_bus_unlock();
    return ok;
}

static int borrar_sesiones_dir(const char *dir, int max_days)
{
    time_t now = time(NULL);
    if (now < 1700000000) return 0;          /* sin fecha fiable no se borra nada */
    int effective_max = (max_days < 2) ? 2 : max_days;
    time_t cutoff = now - (time_t)effective_max * 86400;

    if (!camera_sd_bus_lock(2000)) {
        ESP_LOGW(TAG, "vigilancia: tarjeta ocupada, lo dejo para la proxima");
        return 0;
    }
    DIR *dp = opendir(dir);
    if (!dp) { camera_sd_bus_unlock(); return 0; }

    /* Se recogen primero los nombres (bajo el cerrojo) y se borran despues, uno a
     * uno: borrar mientras se recorre el directorio con el cerrojo tomado dejaba
     * el bus SD retenido durante todo el barrido. */
    char victimas[32][24];
    int nv = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL && nv < 32) {
        time_t fecha = parse_sesion_date(ent->d_name);
        if (fecha == 0 || fecha >= cutoff) continue;
        snprintf(victimas[nv], sizeof(victimas[nv]), "%s", ent->d_name);
        nv++;
    }
    closedir(dp);
    camera_sd_bus_unlock();

    int borradas = 0;
    for (int i = 0; i < nv; i++) {
        if (borra_sesion(dir, victimas[i])) {
            borradas++;
            ESP_LOGI(TAG, "vigilancia: borrada la sesion %s (mas de %d dias)",
                     victimas[i], effective_max);
        } else {
            ESP_LOGW(TAG, "vigilancia: no he podido borrar %s/%s", dir, victimas[i]);
        }
    }
    return borradas;
}

/* Espacio libre en la tarjeta, en MB, o -1 si no se puede saber. Con FatFs y el
 * cerrojo del bus, igual que hace Ajustes para enseñar el hueco: f_getfree
 * recorre la FAT (bloqueante) y el bus es el mismo que necesita la camara.
 * OJO: 0 es un valor legitimo (tarjeta llena) y no significa "no se sabe"; por
 * eso el desconocido es -1 (lo cazo la simulacion del 27-sep-2026). */
static int libre_mb(void)
{
    if (!camera_sd_bus_lock(2000)) return -1;   /* ocupada: mejor no borrar a ciegas */
    FATFS *fs = NULL;
    DWORD libre_cl = 0;
    int mb = -1;
    if (f_getfree("0:", &libre_cl, &fs) == FR_OK && fs) {
        mb = (int)(((uint64_t)libre_cl * fs->csize * 512ULL) / (1024ULL * 1024ULL));
    }
    camera_sd_bus_unlock();
    return mb;
}

/* Si queda poco espacio, borra sesiones de vigilancia por orden de antiguedad
 * (solo esas: ni viajes ni datos) hasta recuperar VIG_LIBRE_OBJETIVO_MB, sin
 * tocar nunca la sesion que se esta grabando. Ver el comentario de arriba. */
static int vig_libera_espacio(void)
{
    int libre = libre_mb();
    /* -1 = no se pudo saber (tarjeta ocupada o sin montar): no se borra nada a
     * ciegas. 0 si es un dato (tarjeta llena) y entonces hay que limpiar. */
    if (libre < 0 || libre >= VIG_LIBRE_MIN_MB) return 0;

    ESP_LOGW(TAG, "quedan %d MB libres en la tarjeta: borro sesiones de vigilancia "
                  "antiguas (objetivo %d MB)", libre, VIG_LIBRE_OBJETIVO_MB);
    char en_curso[24];
    sesion_en_curso(en_curso, sizeof(en_curso));

    int borradas = 0;
    while (libre < VIG_LIBRE_OBJETIVO_MB) {
        char vieja[24] = "";
        if (!camera_sd_bus_lock(2000)) break;
        DIR *dp = opendir(VIG_DIR);
        if (dp) {
            struct dirent *e;
            while ((e = readdir(dp)) != NULL) {
                if (parse_sesion_date(e->d_name) == 0) continue;
                if (en_curso[0] && strcmp(e->d_name, en_curso) == 0) continue;
                if (vieja[0] == '\0' || strcmp(e->d_name, vieja) < 0)
                    snprintf(vieja, sizeof(vieja), "%s", e->d_name);
            }
            closedir(dp);
        }
        camera_sd_bus_unlock();
        if (vieja[0] == '\0') break;          /* no queda nada que borrar */

        borra_sesion(VIG_DIR, vieja);
        borra_sesion(VIG_THUMBS_DIR, vieja);  /* la miniatura de esa sesion, si la hay */
        borradas++;
        const int antes = libre;
        libre = libre_mb();
        if (libre < 0) break;                 /* ya no se puede medir: parar */
        if (libre <= antes) break;            /* no sube: no insistir en bucle */
    }
    if (libre >= 0)
        ESP_LOGW(TAG, "espacio: borradas %d sesion(es) de vigilancia; quedan %d MB libres",
                 borradas, libre);
    else
        ESP_LOGW(TAG, "espacio: borradas %d sesion(es) de vigilancia (el hueco ya no se "
                      "pudo medir)", borradas);
    return borradas;
}

int log_cleanup_run_now(int max_days_keep)
{
    int total = 0;
    for (size_t i = 0; i < NUM_DIRS; ++i) {
        total += process_dir(DIRS[i], max_days_keep, false, false);
    }
    /* La vigilancia puede tener su propia retencion: son carpetas de hasta
     * ~35 MB por sesion, asi que no tiene sentido que sigan la de los CSV de
     * datos (que ocupan ~570 KB al dia). Si nadie la fija, se usa la general. */
    int dias_vig = s_max_days_vig > 0 ? s_max_days_vig : max_days_keep;
    total += borrar_sesiones_dir(VIG_DIR, dias_vig);
    total += borrar_sesiones_dir(VIG_THUMBS_DIR, dias_vig);
    /* Y, ademas de la retencion por dias, el espacio: si la tarjeta esta llena no
     * se guarda nada (ni vigilancia ni datos). Va despues, para que la antiguedad
     * mande y esto solo actue cuando de verdad aprieta. */
    vig_libera_espacio();
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

/* process_dir() recorre directorios y borra ficheros (opendir/unlink): I/O de
 * SD real, sin timeout propio. Los dos timers de aqui corren en la tarea
 * COMPARTIDA de esp_timer; si la SD se cuelga a nivel hardware durante un
 * barrido, se lleva por delante tambien el heap log, GPS, RTC, modo noche...
 * El timer solo notifica a una tarea dedicada. log_cleanup_run_now() (llamada
 * directa, p.ej. desde un boton de Ajustes) sigue siendo sincrona. Mismo
 * arreglo que datalogger.c/battery_history.c/ne185_vlog.c/
 * config_server_viaje.c. Detectado auditando el 07-sep-2026. */
static TaskHandle_t s_cleanup_task_handle;
static log_cleanup_heartbeat_cb_t s_hb_cb = NULL;   /* ver log_cleanup_set_heartbeat_cb */

void log_cleanup_set_heartbeat_cb(log_cleanup_heartbeat_cb_t cb) { s_hb_cb = cb; }

static void cleanup_task(void *arg)
{
    (void)arg;
    if (s_hb_cb) s_hb_cb();
    /* Aviso ANTES del primer barrido: cuanto se va a borrar por antiguedad.
     * (Aqui si: esta tarea tiene 6144 B y el recorrido de directorios es de lo
     * mas hambriento de pila que hay; llamarlo desde main_task reventaba.) */
    {
        int pend = log_cleanup_files_pending_warning(s_max_days_cached > 0 ? s_max_days_cached : 120);
        if (pend > 0) {
            ESP_LOGW(TAG, "se borraran %d ficheros con mas de %d dias en el primer barrido "
                          "(esto es solo el aviso, todavia no borra nada)",
                     pend, s_max_days_cached > 0 ? s_max_days_cached : 120);
        }
    }
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_hb_cb) s_hb_cb();   /* latido watchdog: ver log_cleanup_set_heartbeat_cb */
        log_cleanup_run_now(s_max_days_cached);
    }
}

static void daily_cleanup_cb(void *arg)
{
    (void)arg;
    if (s_cleanup_task_handle) xTaskNotifyGive(s_cleanup_task_handle);
}

/* Primer barrido tras 5s del boot */
static void initial_cleanup_cb(void *arg)
{
    (void)arg;
    if (s_cleanup_task_handle) xTaskNotifyGive(s_cleanup_task_handle);
}

TaskHandle_t log_cleanup_task_handle(void)
{
    return s_cleanup_task_handle;
}

void log_cleanup_set_vigilancia_days(int max_days)
{
    s_max_days_vig = max_days;
}

void log_cleanup_init(int max_days_keep)
{
    s_max_days_cached = max_days_keep;

    /* 3072 causo un bootloop real en bh_flush_task (mismo patron, misma SD)
     * el 08-sep-2026: la SD (fopen/fprintf/fclose, y aqui ademas recorrer
     * directorios) es de lo mas hambriento de pila de ESP-IDF. Sin formateo
     * de float -> se penso que 4096 bastaba. SUBIDA A 6144 el 21-sep-2026: el
     * stackwatch midio 1652 bytes libres (60% usado). */
    if (xTaskCreate(cleanup_task, "log_cleanup_task", 6144, NULL,
                     tskIDLE_PRIORITY + 2, &s_cleanup_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de limpieza: sin barrido periodico");
    }

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
