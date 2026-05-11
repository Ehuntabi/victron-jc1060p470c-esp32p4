#include "log_browser.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include "esp_log.h"

static const char *TAG = "LOG_BROWSER";

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int log_browser_list_dates(const char *dir,
                           char dates_out[][LOG_BROWSER_DATE_LEN],
                           int max)
{
    if (!dir || !dates_out || max <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s: %s", dir, strerror(errno));
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        const char *name = ent->d_name;
        /* Solo "YYYY-MM-DD.csv": 14 chars */
        if (strlen(name) != 14) continue;
        if (name[4] != '-' || name[7] != '-' || strcmp(name + 10, ".csv") != 0) continue;
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (!isdigit((unsigned char)name[i])) goto skip;
        }
        memcpy(dates_out[n], name, 10);
        dates_out[n][10] = 0;
        n++;
        skip: ;
    }
    closedir(d);
    qsort(dates_out, n, LOG_BROWSER_DATE_LEN, cmp_str);
    return n;
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
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s: %s", path, strerror(errno));
        return 0;
    }
    char line[160];
    /* Saltar cabecera */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
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
        n++;
    }
    fclose(f);
    return n;
}

int log_browser_load_battery(const char *path,
                             battery_log_entry_t *out, int max)
{
    if (!path || !out || max <= 0) return 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "fopen %s: %s", path, strerror(errno));
        return 0;
    }
    char line[160];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        char *fields[8] = {0};
        int nf = csv_split(line, fields, 8);
        if (nf < 3) continue;
        /* Solo BM (Battery Monitor) — los otros sources se ignoran en el grafico */
        if (strcmp(fields[1], "BM") != 0) continue;
        battery_log_entry_t *e = &out[n];
        if (!parse_hhmm(fields[0], &e->hh, &e->mm)) continue;
        e->milli_amps = fields[2][0] ? (int32_t)strtol(fields[2], NULL, 10) : 0;
        n++;
    }
    fclose(f);
    return n;
}
