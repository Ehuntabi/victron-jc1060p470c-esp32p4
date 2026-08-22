/* sim_overview.c — Modo simulacion para previsualizar la vista Overview
 * con valores que cambian en el tiempo. Activar via SIM_OVERVIEW_ENABLE
 * en sim_overview.h o desactivar para deshabilitar. */
#include "sim_overview.h"

#if SIM_OVERVIEW_ENABLE

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "camera.h"   /* camera_sd_bus_lock: serializar la SD con el GDMA */
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
#define SIM_TICK_MS     1000

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

/* El nivel del NE185 va de 0 a 4, no de 0 a 3: "0=Reserva, 1=1/4, 2=2/4,
 * 3=3/4, 4=4/4" (ver s1 en ne185/ne185.h). Esta funcion topaba en 3, asi que el
 * simulador NUNCA podia enseñar un deposito lleno -- ni en el 7" ni en el
 * satelite. Corregido el 22-ago-2026 al pedir el usuario ver las aguas limpias
 * a 4/4 y salir un 3. */
static uint8_t tank_level_from_pct(float pct) {
    if (pct < 12.0f) return 0;   /* reserva */
    if (pct < 33.0f) return 1;
    if (pct < 58.0f) return 2;
    if (pct < 83.0f) return 3;
    return 4;                    /* lleno */
}

static void sim_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Simulacion overview ACTIVA — datos ficticios FIJOS");
    (void)now_ms;   /* datos estaticos: ya no usamos el reloj */
    while (1) {
        /* Frame FIJO (el usuario quiere rellenar una vez, sin animar): da
         * SoC ~62% descargando, TTG ~250min, 13.4V, DC-DC activo, solar ~210W,
         * frigo -10C, tanques parciales. Los valores NO cambian entre capturas. */
        const uint32_t t = 30000;

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

        /* Canal auxiliar = bateria de ARRANQUE (aux_input 0 = voltage2). Sin
         * esto llegaba 0.00 V al satelite y la tarjeta del motor salia a cero.
         * Se mueve poco a proposito: una bateria de arranque en reposo va entre
         * 12,4 y 12,8 y solo sube al alternador. */
        d.record.battery.aux_input = 0;
        d.record.battery.aux_value = (uint16_t)(1240 + (int)(tri(0.0f, 40.0f, t, 45000)));
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

        /* === Inversor: encendido (inverting), 230 V, ~230 VA, 1 A. === */
        memset(&d, 0, sizeof(d));
        d.type = VICTRON_BLE_RECORD_INVERTER;
        d.record.inverter.device_state          = 9;      /* 9 = inverting */
        d.record.inverter.alarm_reason          = 0;
        d.record.inverter.battery_voltage_centi = 1340;   /* 13.4 V */
        d.record.inverter.ac_apparent_power_va  = 230;    /* 230 VA */
        d.record.inverter.ac_voltage_centi      = 23000;  /* 230.00 V */
        d.record.inverter.ac_current_deci       = 10;     /* 1.0 A */
        ui_on_panel_data(&d);

        /* === Tanques: limpia se vacia en 50 s y se rellena de golpe.
         *   Grises sube de 0 a lleno en 60 s y se vacia de golpe.
         *   Luces y bomba alternan estados a distinto ritmo. */
        /* TEMPORAL (22-ago-2026): limpia clavada a 4/4 para ver la 3.5" con el
         * deposito lleno. Original: tri(95,5,t,50000) -- baja y sube en 50 s.
         * Descomentar al terminar de ajustar la pantalla. */
        float s1_pct = 100.0f;
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

/* ── Historicos inventados para las capturas ──────────────────────────────────
 *
 * Los tres graficos (bateria, frigo y solar) NO se dibujan de lo que hay en
 * pantalla: los leen de CSV de la tarjeta. Sin datos salen en blanco, y una
 * captura en blanco no sirve para el README.
 *
 * Esto escribe un dia creible. Las reglas:
 *  - Frigo y bateria: el registrador YA ha escrito el csv de HOY, y con la
 *    pantalla en la mesa ese fichero es todo -127. Se APARTA como ".real" y se
 *    escribe el inventado. Reversible renombrando.
 *  - Solar (produccion.csv) NO se toca: es historico acumulado de viajes de
 *    verdad, y 10 dias reales lucen mejor que cualquier invento.
 *  - Los csv de OTROS dias no se tocan nunca.
 *
 * Solo existe con SIM_OVERVIEW_ENABLE: no viaja en el firmware que se publica.
 * 2026-07-26. */

#define SIM_PASO_MIN   5     /* una muestra cada 5 min -> 288 al dia */

static bool sim_existe(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* Aparta el fichero del dia en curso antes de escribir el inventado.
 *
 * Hace falta porque el registrador YA ha escrito el csv de hoy: con la pantalla
 * en la mesa, sin sondas ni BLE, ese fichero es todo -127 y la grafica sale
 * inservible. Se guarda como ".real" en vez de borrarlo: si esto se ejecutara
 * por error con datos buenos del dia, se recuperan renombrando.
 * Devuelve false si habia copia previa (ya se aparto en un arranque anterior:
 * no volver a hacerlo, machacaria la copia buena con la inventada). */
static bool sim_apartar(const char *path)
{
    if (!sim_existe(path)) return true;          /* no hay nada que apartar */
    char bak[80];
    snprintf(bak, sizeof(bak), "%s.real", path);
    if (sim_existe(bak)) {
        ESP_LOGW(TAG, "ya habia copia de %s: no toco nada", path);
        return false;
    }
    if (rename(path, bak) != 0) {
        ESP_LOGW(TAG, "no puedo apartar %s", path);
        return false;
    }
    ESP_LOGW(TAG, "apartado el registro real -> %s", bak);
    return true;
}

/* Curva de sol: 0 fuera de 7:00-21:00, campana suave en medio. 0.0..1.0 */
static float sim_sol(int minuto)
{
    const int amanecer = 7 * 60, ocaso = 21 * 60;
    if (minuto < amanecer || minuto > ocaso) return 0.0f;
    float k = (float)(minuto - amanecer) / (float)(ocaso - amanecer);   /* 0..1 */
    float s = sinf(k * 3.14159265f);
    return s * s;   /* mas estrecha: mediodia marcado */
}

static void sim_escribir_frigo(const char *fecha)
{
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/frigo/%s.csv", fecha);
    if (!sim_apartar(path)) return;
    mkdir("/sdcard/frigo", 0777);
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", path); return; }
    fprintf(f, "timestamp,T_Aletas,T_Congelador,T_Exterior,fan_pct,excedente_solar\n");
    for (int m = 0; m < 24 * 60; m += SIM_PASO_MIN) {
        float sol = sim_sol(m);
        float t_ext    = 14.0f + 18.0f * sol;                 /* 14..32 */
        float t_aletas = 30.0f + 18.0f * sol;                 /* 30..48 */
        float t_cong   = -17.5f + 2.0f * sol;                 /* -17,5..-15,5 */
        int   fan      = (t_aletas <= 35.0f) ? 0
                       : (int)((t_aletas - 35.0f) / 13.0f * 100.0f);
        if (fan > 100) fan = 100;
        fprintf(f, "%s %02d:%02d:00,%.1f,%.1f,%.1f,%d,%d\n",
                fecha, m / 60, m % 60, t_aletas, t_cong, t_ext,
                fan, (sol > 0.55f) ? 1 : 0);
    }
    fclose(f);
    ESP_LOGI(TAG, "historico frigo inventado -> %s", path);
}

static void sim_escribir_bateria(const char *fecha)
{
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/bateria/%s.csv", fecha);
    if (!sim_apartar(path)) return;
    mkdir("/sdcard/bateria", 0777);
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", path); return; }
    fprintf(f, "timestamp,source,milli_amps,milli_amps_max,milli_amps_min,"
               "centi_volts,pv_watts\n");
    for (int m = 0; m < 24 * 60; m += SIM_PASO_MIN) {
        float sol = sim_sol(m);
        int pv_w  = (int)(340.0f * sol);
        /* De noche consume (negativo), de dia la placa carga (positivo). */
        int ma    = (int)(pv_w * 1000.0f / 13.2f) - 4200;
        int cv    = (int)(1250.0f + 130.0f * sol);            /* 12,50..13,80 V */
        /* La grafica SOLO pinta las filas de BatteryMonitor (ver log_browser). */
        fprintf(f, "%s %02d:%02d:00,BatteryMonitor,%d,%d,%d,%d,-1\n",
                fecha, m / 60, m % 60, ma, ma + 150, ma - 150, cv);
        if (pv_w > 0) {
            fprintf(f, "%s %02d:%02d:00,SolarCharger,%d,%d,%d,%d,%d\n",
                    fecha, m / 60, m % 60,
                    (int)(pv_w * 1000.0f / 13.2f), 0, 0, cv, pv_w);
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "historico bateria inventado -> %s", path);
}

/* Produccion dia a dia: una linea por dia, los 10 anteriores a hoy. */
static void sim_escribir_solar(void)
{
    const char *path = "/sdcard/solar/produccion.csv";
    if (sim_existe(path)) { ESP_LOGW(TAG, "YA EXISTE, no lo toco: %s", path); return; }
    mkdir("/sdcard/solar", 0777);
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", path); return; }
    fprintf(f, "fecha,kwh,horas,pico_w,kwh_consumo\n");
    time_t now = time(NULL);
    for (int d = 10; d >= 1; --d) {
        time_t t = now - (time_t)d * 86400;
        struct tm tm_l; localtime_r(&t, &tm_l);
        /* Dias variados: alguno nublado, para que la grafica no sea plana. */
        static const float KWH[10]  = {1.42f, 1.83f, 0.61f, 1.95f, 1.71f,
                                       0.94f, 1.88f, 2.05f, 1.33f, 1.79f};
        static const float HRS[10]  = {7.5f, 9.1f, 3.2f, 9.6f, 8.8f,
                                       5.0f, 9.3f, 9.9f, 6.8f, 9.0f};
        static const int   PICO[10] = {268, 331, 142, 352, 318,
                                       201, 340, 366, 249, 327};
        int i = 10 - d;
        fprintf(f, "%04d-%02d-%02d,%.3f,%.2f,%d,%.3f\n",
                tm_l.tm_year + 1900, tm_l.tm_mon + 1, tm_l.tm_mday,
                KWH[i], HRS[i], PICO[i], KWH[i] * 0.78f);
    }
    fclose(f);
    ESP_LOGI(TAG, "historico solar inventado -> %s", path);
}

static void sim_generar_historicos(void)
{
    time_t now = time(NULL);
    struct tm tm_l;
    localtime_r(&now, &tm_l);
    if (tm_l.tm_year + 1900 < 2020) {
        ESP_LOGW(TAG, "sin hora valida: no genero historicos (saldrian con fecha absurda)");
        return;
    }
    char fecha[11];
    strftime(fecha, sizeof(fecha), "%Y-%m-%d", &tm_l);

    /* Serializar con la camara, como el resto de accesos a la tarjeta. */
    if (!camera_sd_bus_lock(5000)) {
        ESP_LOGW(TAG, "tarjeta ocupada: no genero historicos");
        return;
    }
    sim_escribir_frigo(fecha);
    sim_escribir_bateria(fecha);
    sim_escribir_solar();
    camera_sd_bus_unlock();
}

void sim_overview_start(void) {
    sim_generar_historicos();
    xTaskCreate(sim_task, "sim_overview", 4096, NULL, 4, NULL);
}

#else  /* !SIM_OVERVIEW_ENABLE */
void sim_overview_start(void) { /* no-op */ }
#endif
