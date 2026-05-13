/* sim_overview.c — Modo simulacion para previsualizar la vista Overview
 * con valores que cambian en el tiempo. Activar via SIM_OVERVIEW_ENABLE
 * en sim_overview.h o desactivar para deshabilitar. */
#include "sim_overview.h"

#if SIM_OVERVIEW_ENABLE

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ne185/ne185.h"
#include "frigo.h"
#include "ui.h"
#include "victron_records.h"

static const char *TAG = "sim_overview";

/* Periodo del ciclo completo de la simulacion: 60 segundos. Los distintos
 * indicadores tienen sub-ciclos (algunos mas rapidos, otros lentos) para
 * que la pantalla NO se vea estatica. */
#define SIM_TICK_MS     200

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Triangle wave: ciclo entre lo y hi en period_ms */
static float tri(float lo, float hi, uint32_t now, uint32_t period_ms) {
    uint32_t p = now % period_ms;
    float k;
    if (p < period_ms / 2) {
        k = (float)p / (float)(period_ms / 2);
    } else {
        k = 1.0f - (float)(p - period_ms / 2) / (float)(period_ms / 2);
    }
    return lo + (hi - lo) * k;
}

static uint8_t tank_level_from_pct(float pct) {
    if (pct < 16.0f) return 0;
    if (pct < 50.0f) return 1;
    if (pct < 83.0f) return 2;
    return 3;
}

static void sim_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Simulacion overview ACTIVA — datos ficticios cambiantes");
    uint32_t t0 = now_ms();
    while (1) {
        uint32_t t = now_ms() - t0;

        /* === Bateria: SOC entre 30 % y 95 % con ciclo de 40 s.
         *   Corriente: +5 A cuando sube SOC, -3 A cuando baja.
         *   Voltaje: 12.5 a 14.0 V acorde al SOC. */
        float soc_pct = tri(30.0f, 95.0f, t, 40000);
        uint16_t soc_deci = (uint16_t)(soc_pct * 10);
        uint32_t bat_phase = t % 40000;
        int32_t cur_milli = (bat_phase < 20000) ? 5000 : -3000;  /* +5A o -3A */
        uint16_t v_centi = 1250 + (uint16_t)(soc_pct * 1.5f);    /* 12.5..14V */
        uint32_t ttg = (cur_milli < 0) ? (uint32_t)(soc_pct * 4) : 0xFFFFFFFF;

        victron_data_t d = {0};
        d.type = VICTRON_BLE_RECORD_BATTERY_MONITOR;
        d.record.battery.soc_deci_percent = soc_deci;
        d.record.battery.battery_voltage_centi = v_centi;
        d.record.battery.battery_current_milli = cur_milli;
        d.record.battery.time_to_go_minutes = ttg;
        ui_on_panel_data(&d);

        /* === Solar: oscila entre 0 W (noche) y 280 W (mediodia), ciclo
         *   de 80 s (representa un dia completo acelerado). */
        float pv_w = tri(0.0f, 280.0f, t, 80000);
        memset(&d, 0, sizeof(d));
        d.type = VICTRON_BLE_RECORD_SOLAR_CHARGER;
        d.record.solar.pv_power_w = (uint16_t)pv_w;
        d.record.solar.battery_voltage_centi = v_centi;
        d.record.solar.battery_current_deci = (int16_t)(pv_w / v_centi * 1000);
        d.record.solar.load_current_deci = 0;
        d.record.solar.yield_today_centikwh = (uint16_t)(50 + (t / 1000) % 200);
        ui_on_panel_data(&d);

        /* === DC/DC: alternar activo / inactivo cada 25 s.
         *   V_in (motor) 12.4 V con motor parado, ~13.8 V con motor en marcha.
         *   V_out 13.6 V cuando carga, 12.7 V cuando idle. */
        bool dcdc_active = ((t / 25000) % 2) == 1;
        memset(&d, 0, sizeof(d));
        d.type = VICTRON_BLE_RECORD_DCDC_CONVERTER;
        d.record.dcdc.device_state = dcdc_active ? 4 : 0;   /* 4=charging,0=off */
        d.record.dcdc.input_voltage_centi = dcdc_active ? 1380 : 1240;
        d.record.dcdc.output_voltage_centi = dcdc_active ? 1360 : 1270;
        ui_on_panel_data(&d);

        /* === Tanques: limpia se vacia en 50 s y se rellena de golpe.
         *   Grises sube de 0 a lleno en 60 s y se vacia de golpe.
         *   Luces y bomba alternan estados a distinto ritmo. */
        float s1_pct = tri(95.0f, 5.0f, t, 50000);   /* baja-sube */
        float r1_pct = tri(5.0f, 95.0f, t, 60000);   /* sube-baja */
        bool lin   = ((t / 7000)  % 2) == 0;
        bool lout  = ((t / 11000) % 2) == 1;
        bool pump  = ((t / 13000) % 2) == 0;
        bool shore = ((t / 30000) % 2) == 1;
        ne185_sim_inject(tank_level_from_pct(s1_pct),
                         tank_level_from_pct(r1_pct),
                         lin, lout, pump, shore);

        /* === Frigo: T_Congelador oscila entre -20°C y 0°C (40s); ventilador
         *   sube cuando T sube. */
        float t_cong = tri(-20.0f, 0.0f, t, 40000);
        float t_aletas = tri(2.0f, 18.0f, t, 35000);
        float t_ext = 22.0f + tri(0.0f, 6.0f, t, 90000);
        uint8_t fan = (uint8_t)tri(0.0f, 100.0f, t, 18000);
        frigo_sim_inject(t_aletas, t_cong, t_ext, fan);

        vTaskDelay(pdMS_TO_TICKS(SIM_TICK_MS));
    }
}

void sim_overview_start(void) {
    xTaskCreate(sim_task, "sim_overview", 4096, NULL, 4, NULL);
}

#else  /* !SIM_OVERVIEW_ENABLE */
void sim_overview_start(void) { /* no-op */ }
#endif
