#include "log_browser.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include "esp_log.h"
#include "camera.h"   /* camera_sd_bus_lock: serializar SD con el GDMA de la camara */

static const char *TAG = "LOG_BROWSER";

int log_browser_list_dates(const char *dir,
                           char dates_out[][LOG_BROWSER_DATE_LEN],
                           int max)
{
    if (!dir || !dates_out || max <= 0) return 0;
    bool sdl = camera_sd_bus_lock(3000);   /* serializar el barrido de dir con el GDMA de la camara */
    if (!sdl) return 0;   /* bus ocupado por la camara: no tocar la SD */
    DIR *d = opendir(dir);
    if (!d) {
        if (sdl) camera_sd_bus_unlock();
        ESP_LOGW(TAG, "opendir %s: %s", dir, strerror(errno));
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        /* Solo "YYYY-MM-DD.csv": 14 chars */
        if (strlen(name) != 14) continue;
        if (name[4] != '-' || name[7] != '-' || strcmp(name + 10, ".csv") != 0) continue;
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (!isdigit((unsigned char)name[i])) goto skip;
        }
        /* Insercion ordenada quedandose con las `max` fechas MAS RECIENTES.
         * Antes se cogian las `max` primeras que devolviera readdir y se
         * ordenaban despues: ese orden no es cronologico (en FAT los huecos de
         * los ficheros borrados se reutilizan), asi que al pasar de `max` dias
         * en el directorio podia caerse de la lista un dia reciente en vez del
         * mas viejo. Con retencion de 60 dias y max=60 el caso se da. */
        char day[LOG_BROWSER_DATE_LEN];
        memcpy(day, name, 10);
        day[10] = 0;
        if (n == max) {
            if (strcmp(day, dates_out[0]) <= 0) continue;   /* mas vieja que todas */
            memmove(dates_out[0], dates_out[1],
                    (size_t)(max - 1) * LOG_BROWSER_DATE_LEN);
            n--;
        }
        int pos = n;
        while (pos > 0 && strcmp(dates_out[pos - 1], day) > 0) {
            memcpy(dates_out[pos], dates_out[pos - 1], LOG_BROWSER_DATE_LEN);
            pos--;
        }
        memcpy(dates_out[pos], day, LOG_BROWSER_DATE_LEN);
        n++;
        skip: ;
    }
    closedir(d);
    if (sdl) camera_sd_bus_unlock();
    return n;   /* ya ordenado ascendente por la insercion */
}

/* Extrae "HH:MM" de un timestamp con offset 11..15 ("YYYY-MM-DD HH:MM:..") */
static bool parse_hhmm(const char *ts, int *hh, int *mm)
{
    if (!ts || strlen(ts) < 16) return false;
    if (sscanf(ts + 11, "%d:%d", hh, mm) != 2) return false;
    if (*hh < 0 || *hh > 23 || *mm < 0 || *mm > 59) return false;
    return true;
}

/* Tokeniza una linea CSV en hasta `max_fields` campos. Acepta campos vacios
 * ("a,,c"). Modifica `line` in-place poniendo '\0' en las comas. */
static int csv_split(char *line, char *fields[], int max_fields)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < max_fields) {
        if (*p == ',') {
            *p = 0;
            fields[n++] = p + 1;
        }
        p++;
    }
    /* Strip newline del ultimo campo */
    if (n > 0) {
        char *last = fields[n - 1];
        size_t len = strlen(last);
        while (len > 0 && (last[len - 1] == '\n' || last[len - 1] == '\r')) {
            last[--len] = 0;
        }
    }
    return n;
}

int log_browser_load_frigo(const char *path,
                           frigo_log_entry_t *out, int max)
{
    if (!path || !out || max <= 0) return 0;
    bool sdl = camera_sd_bus_lock(3000);   /* serializar SD con el GDMA de la camara */
    if (!sdl) return 0;   /* bus ocupado por la camara: no tocar la SD */
    FILE *f = fopen(path, "r");
    if (!f) {
        camera_sd_bus_unlock();
        ESP_LOGW(TAG, "fopen %s: %s", path, strerror(errno));
        return 0;
    }
    char line[160];
    /* Saltar cabecera */
    if (!fgets(line, sizeof(line), f)) { fclose(f); if (sdl) camera_sd_bus_unlock(); return 0; }
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        char *fields[8] = {0};
        int nf = csv_split(line, fields, 8);
        if (nf < 5) continue;
        frigo_log_entry_t *e = &out[n];
        if (!parse_hhmm(fields[0], &e->hh, &e->mm)) continue;
        e->t_aletas = fields[1][0] ? strtof(fields[1], NULL) : NAN;
        e->t_congel = fields[2][0] ? strtof(fields[2], NULL) : NAN;
        e->t_exter  = fields[3][0] ? strtof(fields[3], NULL) : NAN;
        e->fan_pct  = fields[4][0] ? atoi(fields[4]) : 0;
        e->excedente_solar = (nf >= 6 && fields[5][0]) ? atoi(fields[5]) : 0;
        n++;
    }
    fclose(f);
    if (sdl) camera_sd_bus_unlock();
    return n;
}

int log_browser_load_battery(const char *path,
                             battery_log_entry_t *out, int max)
{
    if (!path || !out || max <= 0) return 0;
    bool sdl = camera_sd_bus_lock(3000);   /* serializar SD con el GDMA de la camara */
    if (!sdl) return 0;   /* bus ocupado por la camara: no tocar la SD */
    FILE *f = fopen(path, "r");
    if (!f) {
        camera_sd_bus_unlock();
        ESP_LOGW(TAG, "fopen %s: %s", path, strerror(errno));
        return 0;
    }
    char line[160];
    if (!fgets(line, sizeof(line), f)) { fclose(f); camera_sd_bus_unlock(); return 0; }
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        char *fields[8] = {0};
        int nf = csv_split(line, fields, 8);
        if (nf < 3) continue;
        /* Solo BM (Battery Monitor) — los otros sources se ignoran en el grafico.
         * El CSV escribe el nombre completo "BatteryMonitor" (no "BM"). */
        if (strcmp(fields[1], "BatteryMonitor") != 0) continue;
        battery_log_entry_t *e = &out[n];
        if (!parse_hhmm(fields[0], &e->hh, &e->mm)) continue;
        e->milli_amps = fields[2][0] ? (int32_t)strtol(fields[2], NULL, 10) : 0;
        /* Columna de tension (centivoltios) opcional: ausente en CSV antiguos */
        e->centi_volts = (nf >= 6 && fields[5][0])
            ? (int32_t)strtol(fields[5], NULL, 10) : 0;
        n++;
    }
    fclose(f);
    if (sdl) camera_sd_bus_unlock();
    return n;
}
