#include "nvs_trace.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "NVSTRACE";

#define NVS_TRACE_MAGIC 0x4E565431u   /* "NVT1"; distingue RAM valida de basura al power-on */

/* RTC-noinit: no se zeroea en el arranque; sobrevive a resets SW (incl. INT WDT),
 * se pierde solo al quitar la alimentacion (justo lo que queremos). */
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_inflight;    /* nvs_site_t en curso (0 = ninguno) */
static RTC_NOINIT_ATTR int64_t  s_t0;
static RTC_NOINIT_ATTR uint32_t s_max_site;
static RTC_NOINIT_ATTR uint32_t s_max_ms;

static const char *site_name(uint32_t s)
{
    switch (s) {
        case NVS_SITE_FLASH_INIT: return "nvs_flash_init (GC recovery boot)";
        case NVS_SITE_RTC_BACKUP: return "rtc_backup (epoch horario)";
        case NVS_SITE_WD_COUNTER: return "watchdog (contador)";
        case NVS_SITE_ENERGY:     return "energy_today";
        case NVS_SITE_TRIP:       return "trip_computer";
        default:                  return "ninguno";
    }
}

void nvs_trace_begin(nvs_site_t site)
{
    s_inflight = (uint32_t)site;
    s_t0 = esp_timer_get_time();
}

void nvs_trace_end(void)
{
    uint32_t ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
    uint32_t site = s_inflight;
    s_inflight = NVS_SITE_NONE;
    if (ms > s_max_ms) { s_max_ms = ms; s_max_site = site; }
    if (ms >= 100) ESP_LOGW(TAG, "op NVS LENTA %u ms en %s", (unsigned)ms, site_name(site));
}

void nvs_trace_boot_report(void)
{
    if (s_magic != NVS_TRACE_MAGIC) {
        /* Primer arranque (RAM sin inicializar tras quitar alimentacion). */
        s_magic = NVS_TRACE_MAGIC;
        s_inflight = NVS_SITE_NONE; s_t0 = 0; s_max_site = NVS_SITE_NONE; s_max_ms = 0;
        return;
    }
    if (esp_reset_reason() == ESP_RST_INT_WDT) {
        ESP_LOGE(TAG, "### INT WDT: op NVS EN VUELO al petar = %s ###", site_name(s_inflight));
        ESP_LOGE(TAG, "### op NVS mas larga vista = %u ms en %s ###",
                 (unsigned)s_max_ms, site_name(s_max_site));
    }
    s_inflight = NVS_SITE_NONE;   /* consumir para el siguiente ciclo */
}

void nvs_trace_stats(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGW(TAG, "NVS stats: usadas=%u libres=%u total=%u ns=%u",
                 (unsigned)st.used_entries, (unsigned)st.free_entries,
                 (unsigned)st.total_entries, (unsigned)st.namespace_count);
    }
}
