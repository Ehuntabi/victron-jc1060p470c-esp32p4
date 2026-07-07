#pragma once
#include <stdint.h>

/* Instrumentacion para CONFIRMAR el root-cause del INT WDT (erase de la GC de
 * NVS que bloquea las interrupciones >300ms). Breadcrumb en RTC-noinit RAM que
 * sobrevive al reset: marca que operacion NVS estaba EN VUELO cuando peto, y
 * cronometra las ops para ver si alguna se acerca a los 300ms. Cero escrituras
 * de flash propias (solo RAM no-inicializada). Ver project_victron_sd_audit_intwdt. */

typedef enum {
    NVS_SITE_NONE = 0,
    NVS_SITE_FLASH_INIT,   /* nvs_flash_init (GC de recovery en el boot) */
    NVS_SITE_RTC_BACKUP,   /* backup horario de epoch (unico commit en marcha en banco) */
    NVS_SITE_WD_COUNTER,   /* contador de resets del watchdog (una vez por boot) */
    NVS_SITE_ENERGY,       /* energy_today (cada 300s, solo con Victron) */
    NVS_SITE_TRIP,         /* trip_computer (cada 300s, solo con Victron) */
} nvs_site_t;

/* Emparejar begin/end alrededor de la escritura NVS (set+commit). end debe
 * ejecutarse siempre (no return en medio). */
void nvs_trace_begin(nvs_site_t site);
void nvs_trace_end(void);

/* Al inicio de app_main, ANTES de nvs_flash_init: si el reset fue INT WDT,
 * informa que operacion estaba en vuelo y la op NVS mas larga vista. */
void nvs_trace_boot_report(void);

/* Tras nvs_flash_init: vuelca nvs_get_stats (¿NVS lleno de claves legacy?). */
void nvs_trace_stats(void);
