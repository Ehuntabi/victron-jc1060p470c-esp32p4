#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BH_POINTS         480     /* 24h @ 3 min sample */
#define BH_SAMPLE_MS      (3 * 60 * 1000)
#define BH_PERSIST_MS     (15 * 60 * 1000)

typedef enum {
    BH_SRC_BATTERY_MONITOR = 0,
    BH_SRC_SOLAR_CHARGER,
    BH_SRC_ORION_XS,
    BH_SRC_AC_CHARGER,
    BH_SRC_COUNT
} bh_source_t;

typedef struct {
    int32_t ts;             /* unix-ish seconds (relative to esp_timer if no rtc) */
    int32_t milli_amps;     /* signed; positive = charging into battery */
    bool    valid;
} bh_point_t;

esp_err_t battery_history_init(void);

/* Latest reading from a source (called from ui_on_panel_data hook) */
void battery_history_update_latest(bh_source_t src, int32_t milli_amps);

/* Get full series (caller-allocated array of BH_POINTS).
 * Returns the number of valid points and out_oldest_ts/out_newest_ts. */
size_t battery_history_get_series(bh_source_t src,
                                  bh_point_t *out_points,
                                  int32_t *out_oldest_ts,
                                  int32_t *out_newest_ts);

/* Totals over the buffer (Ah accumulated charge & discharge for src) */
void battery_history_get_totals(bh_source_t src,
                                float *out_charge_ah,
                                float *out_discharge_ah);

const char *battery_history_source_name(bh_source_t src);

#ifdef __cplusplus
}
#endif
