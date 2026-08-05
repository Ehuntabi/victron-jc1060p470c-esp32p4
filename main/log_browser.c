#include "log_browser.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* vTaskDelay: ceder CPU al trocear la lectura */
#include "camera.h"   /* camera_sd_bus_lock: serializar SD con el GDMA de la camara */
#include "battery_history.h"  /* BH_SRC_COUNT + battery_history_source_name */

static const char *TAG = "LOG_BROWSER";

/* Lineas que se leen entre dos cesiones de CPU en log_browser_load_battery.
 *
 * El CSV de un dia de bateria son ~8640 muestras por fuente (una cada 10 s, ver
 * BH_POINTS): decenas de miles de lineas, cada una con csv_split + strtol. La
 * lectura corre en bh_loader_task (main/ui.c), fuera del hilo de LVGL, asi que
 * el troceado ya no esta para salvar a la UI: esta para no acaparar la SD.
 * Entre trozos se suelta camera_sd_bus_lock y se cede, y asi la camara recupera
 * su ventana de GDMA durante los segundos que dura el parseo.
 *
 * A ~100 us por linea, 512 lineas son ~50 ms; el coste total de los yields en
 * un dia entero es ~330 ms (33 trozos x 1 tick de 10 ms).
 *
 * El log del frigo no lleva troceado a proposito: son ~288 lineas por dia (una
 * cada 5 min), 60 veces menos, y nunca se acerco al limite. Si algun dia sube
 * su cadencia, hay que traerse este mismo patron. */
#define BATT_CHUNK_LINES  512

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
    /* Diagnostico: la placa listaba UN solo dia donde el mismo codigo, corrido
     * contra la misma tarjeta en un PC, encuentra todos. Trazamos lo que ve
     * readdir de verdad (cuantas entradas y como se llaman las descartadas)
     * porque la sospecha es que devuelva nombres cortos 8.3 (2026-0~1.CSV, 12
     * chars) en vez de los largos, y entonces no casa ninguno. */
    int total = 0, descartadas = 0;
    char muestra[3][32];
    int n_muestra = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        total++;
        /* Solo "YYYY-MM-DD.csv": 14 chars */
        bool valido = (strlen(name) == 14) && name[4] == '-' && name[7] == '-' &&
                      strcmp(name + 10, ".csv") == 0;
        for (int i = 0; valido && i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (!isdigit((unsigned char)name[i])) valido = false;
        }
        if (!valido) {
            descartadas++;
            if (n_muestra < 3) {
                snprintf(muestra[n_muestra], sizeof muestra[0], "%s", name);
                n_muestra++;
            }
            continue;
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
    }
    closedir(d);
    if (sdl) camera_sd_bus_unlock();
    ESP_LOGI(TAG, "%s: readdir dio %d entradas -> %d fechas, %d descartadas%s%s%s%s%s%s",
             dir, total, n, descartadas,
             n_muestra > 0 ? " (ej: " : "", n_muestra > 0 ? muestra[0] : "",
             n_muestra > 1 ? ", " : "",   n_muestra > 1 ? muestra[1] : "",
             n_muestra > 2 ? ", " : "",   n_muestra > 2 ? muestra[2] : "");
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
                             battery_log_entry_t *const *out, int max,
                             int *n_out)
{
    if (!path || !out || !n_out || max <= 0) return 0;
    for (int s = 0; s < BH_SRC_COUNT; ++s) n_out[s] = 0;
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
    int since_yield = 0;
    bool have_lock = true;
    while (fgets(line, sizeof(line), f)) {
        if (++since_yield >= BATT_CHUNK_LINES) {
            since_yield = 0;
            /* Soltar el bus y ceder: la camara recupera su ventana de GDMA y
             * las tareas de menor prioridad (IDLE incluida, que es a quien
             * vigila el Task WDT) vuelven a correr. El FILE sigue abierto entre
             * trozos: nadie mas escribe este fichero mientras se lee. Mismo
             * patron que el drenador de vigilancia, que suelta el bus entre
             * trozos de 8 KB. */
            camera_sd_bus_unlock();
            have_lock = false;
            vTaskDelay(1);
            if (!camera_sd_bus_lock(3000)) {
                ESP_LOGW(TAG, "%s: bus SD ocupado a mitad; devuelvo %d entradas",
                         path, n);
                break;   /* un dia parcial se ve mejor que una grafica vacia */
            }
            have_lock = true;
        }
        char *fields[8] = {0};
        int nf = csv_split(line, fields, 8);
        if (nf < 3) continue;
        /* La columna `source` lleva el nombre completo que escribio
         * battery_history ("BatteryMonitor", "SolarCharger", "OrionTR",
         * "ACCharger"): se compara contra la misma tabla para que no puedan
         * divergir. Una fuente desconocida (CSV de una version futura) se
         * ignora en vez de descartar la linea entera. */
        int src = -1;
        for (int s = 0; s < BH_SRC_COUNT; ++s) {
            if (strcmp(fields[1], battery_history_source_name((bh_source_t)s)) == 0) {
                src = s;
                break;
            }
        }
        if (src < 0 || !out[src] || n_out[src] >= max) continue;
        battery_log_entry_t *e = &out[src][n_out[src]];
        if (!parse_hhmm(fields[0], &e->hh, &e->mm)) continue;
        e->milli_amps = fields[2][0] ? (int32_t)strtol(fields[2], NULL, 10) : 0;
        /* Columna de tension (centivoltios) opcional: ausente en CSV antiguos */
        e->centi_volts = (nf >= 6 && fields[5][0])
            ? (int32_t)strtol(fields[5], NULL, 10) : 0;
        n_out[src]++;
        n++;
    }
    /* El fclose toca la SD: recuperar el cerrojo si se salio del bucle sin el.
     * Si tampoco se consigue, se cierra igual (dejar el FILE abierto seria
     * peor: fuga de descriptor y del buffer de stdio). */
    if (!have_lock) have_lock = camera_sd_bus_lock(3000);
    fclose(f);
    if (have_lock) camera_sd_bus_unlock();
    return n;
}
