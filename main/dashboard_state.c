#include "dashboard_state.h"
#include "energy_today.h"
#include "trip_computer.h"
#include "pzem004t.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool     bat_has;
    uint16_t soc_deci;
    uint16_t bat_v_centi;
    int32_t  bat_i_milli;
    uint16_t ttg_min;
    uint16_t bat_alarm;

    bool     solar_has;
    uint16_t pv_w;
    uint16_t solar_yield_centikwh;
    int16_t  solar_i_deci;
    uint8_t  solar_state;
    uint8_t  solar_err;

    bool     dcdc_has;
    uint16_t dc_in_v_centi;
    uint16_t dc_out_v_centi;
    uint8_t  dc_state;
    uint8_t  dc_err;

    SemaphoreHandle_t mtx;
} s;

static void lock(void)
{
    if (!s.mtx) s.mtx = xSemaphoreCreateMutex();
    if (s.mtx) xSemaphoreTake(s.mtx, portMAX_DELAY);
}
static void unlock(void) { if (s.mtx) xSemaphoreGive(s.mtx); }

void dashboard_state_on_record(const victron_data_t *data)
{
    if (!data) return;
    lock();
    switch (data->type) {
        case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
            const victron_record_battery_monitor_t *b = &data->record.battery;
            s.bat_has = true;
            s.soc_deci = b->soc_deci_percent;
            s.bat_v_centi = b->battery_voltage_centi;
            s.bat_i_milli = b->battery_current_milli;
            s.ttg_min = b->time_to_go_minutes;
            s.bat_alarm = b->alarm_reason;
            break;
        }
        case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
            const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
            s.bat_has = true;
            s.soc_deci = b->soc_deci_percent;
            s.bat_v_centi = b->battery_voltage_centi;
            s.bat_i_milli = (int32_t)b->battery_current_deci * 100;
            s.ttg_min = b->time_to_go_min;
            break;
        }
        case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
            const victron_record_solar_charger_t *sc = &data->record.solar;
            s.solar_has = true;
            s.pv_w = sc->pv_power_w;
            s.solar_yield_centikwh = sc->yield_today_centikwh;
            s.solar_i_deci = sc->battery_current_deci;
            s.solar_state = sc->device_state;
            s.solar_err = sc->charger_error;
            break;
        }
        case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
            const victron_record_dcdc_converter_t *d = &data->record.dcdc;
            s.dcdc_has = true;
            s.dc_in_v_centi = d->input_voltage_centi;
            s.dc_out_v_centi = d->output_voltage_centi;
            s.dc_state = d->device_state;
            s.dc_err = d->charger_error;
            break;
        }
        case VICTRON_BLE_RECORD_ORION_XS: {
            const victron_record_orion_xs_t *o = &data->record.orion;
            s.dcdc_has = true;
            s.dc_in_v_centi = o->input_voltage_centi;
            s.dc_out_v_centi = o->output_voltage_centi;
            s.dc_state = o->device_state;
            s.dc_err = o->charger_error;
            break;
        }
        default: break;
    }
    unlock();
}

size_t dashboard_state_to_json(char *buf, size_t maxlen)
{
    if (!buf || maxlen < 256) return 0;
    lock();
    float pv_kwh = energy_today_pv_kwh();
    float ld_kwh = energy_today_loads_kwh();
    int p_w = (int)((int64_t)s.bat_v_centi * s.bat_i_milli / 100000LL);
    trip_computer_t trip; trip_computer_get(&trip);
    int trip_hours = (int)(trip.seconds_running / 3600);
    int trip_min   = (int)((trip.seconds_running % 3600) / 60);
    pzem_data_t pz; pzem_get(&pz);
    int n = snprintf(buf, maxlen,
        "{"
          "\"battery\":{"
            "\"has\":%s,"
            "\"soc_pct\":%.1f,"
            "\"voltage_v\":%.2f,"
            "\"current_a\":%.3f,"
            "\"power_w\":%d,"
            "\"ttg_min\":%u,"
            "\"alarm\":%u"
          "},"
          "\"solar\":{"
            "\"has\":%s,"
            "\"pv_w\":%u,"
            "\"yield_today_kwh\":%.2f,"
            "\"state\":%u,"
            "\"error\":%u"
          "},"
          "\"dcdc\":{"
            "\"has\":%s,"
            "\"in_v\":%.2f,"
            "\"out_v\":%.2f,"
            "\"state\":%u,"
            "\"error\":%u"
          "},"
          "\"energy_today\":{"
            "\"pv_kwh\":%.2f,"
            "\"loads_kwh\":%.2f"
          "},"
          "\"trip\":{"
            "\"reset_epoch\":%lld,"
            "\"hours\":%d,"
            "\"minutes\":%d,"
            "\"charged_kwh\":%.2f,"
            "\"discharged_kwh\":%.2f,"
            "\"charged_ah\":%.1f,"
            "\"discharged_ah\":%.1f"
          "},"
          "\"ac\":{"
            "\"has\":%s,"
            "\"voltage_v\":%.1f,"
            "\"current_a\":%.3f,"
            "\"power_w\":%.1f,"
            "\"energy_wh\":%u,"
            "\"freq_hz\":%.1f,"
            "\"pf\":%.2f,"
            "\"alarm\":%s"
          "}"
        "}",
        s.bat_has   ? "true" : "false",
        (float)s.soc_deci / 10.0f,
        (float)s.bat_v_centi / 100.0f,
        (float)s.bat_i_milli / 1000.0f,
        p_w,
        (unsigned)s.ttg_min,
        (unsigned)s.bat_alarm,

        s.solar_has ? "true" : "false",
        (unsigned)s.pv_w,
        (float)s.solar_yield_centikwh / 100.0f,
        (unsigned)s.solar_state, (unsigned)s.solar_err,

        s.dcdc_has  ? "true" : "false",
        (float)s.dc_in_v_centi  / 100.0f,
        (float)s.dc_out_v_centi / 100.0f,
        (unsigned)s.dc_state, (unsigned)s.dc_err,

        pv_kwh, ld_kwh,

        (long long)trip.reset_epoch,
        trip_hours, trip_min,
        trip.wh_charged / 1000.0,
        trip.wh_discharged / 1000.0,
        trip.ah_charged,
        trip.ah_discharged,

        pz.has_data ? "true" : "false",
        pz.voltage_v, pz.current_a, pz.power_w,
        (unsigned)pz.energy_wh, pz.freq_hz, pz.power_factor,
        pz.alarm ? "true" : "false");
    unlock();
    return (n > 0 && (size_t)n < maxlen) ? (size_t)n : 0;
}
