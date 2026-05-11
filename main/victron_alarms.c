#include "victron_alarms.h"
#include "victron_records.h"
#include <stddef.h>

const char *victron_alarm_reason_string(uint16_t mask)
{
    if (mask == 0) return NULL;
    if (mask & VIC_ALARM_LOW_VOLTAGE)   return "V baja";
    if (mask & VIC_ALARM_HIGH_VOLTAGE)  return "V alta";
    if (mask & VIC_ALARM_LOW_SOC)       return "SoC bajo";
    if (mask & VIC_ALARM_LOW_TEMP)      return "T baja";
    if (mask & VIC_ALARM_HIGH_TEMP)     return "T alta";
    if (mask & VIC_ALARM_OVERLOAD)      return "Sobrecarga";
    if (mask & VIC_ALARM_DC_RIPPLE)     return "DC ripple";
    if (mask & VIC_ALARM_SHORT_CIRCUIT) return "Corto";
    if (mask & VIC_ALARM_BMS_LOCKOUT)   return "BMS lock";
    return "Alarma";
}

const char *victron_charger_error_string(uint8_t code)
{
    switch (code) {
        case VIC_ERR_NONE:              return NULL;
        case VIC_ERR_BAT_TEMP_HIGH:     return "Bat T alta";
        case VIC_ERR_BAT_VOLT_HIGH:     return "Bat V alta";
        case VIC_ERR_REMOTE_TEMP_SENSOR:return "Sensor T";
        case VIC_ERR_REMOTE_BAT_SENSE:  return "Sensor V";
        case VIC_ERR_HIGH_RIPPLE:       return "Ripple";
        case VIC_ERR_TEMP_LOW:          return "T baja";
        case VIC_ERR_TEMP_CHARGER:      return "Carg T alta";
        case VIC_ERR_OVER_CURRENT:      return "Sobrecorriente";
        case VIC_ERR_POLARITY:          return "Polaridad";
        case VIC_ERR_OVERHEATED:        return "Sobrecalent.";
        case VIC_ERR_SHORT_CIRCUIT:     return "Corto";
        case VIC_ERR_INPUT_VOLT_HIGH:   return "V entrada";
        case VIC_ERR_INPUT_CURR_HIGH:   return "I entrada";
        case VIC_ERR_INPUT_SHUTDOWN:    return "Entrada off";
        case VIC_ERR_CPU_TEMP:          return "CPU T alta";
        case VIC_ERR_CALIBRATION_LOST:  return "Calibracion";
        case VIC_ERR_UNKNOWN:           return NULL;  /* 0xFF = no aplica */
        default:                        return "Error";
    }
}

const char *victron_vebus_error_string(uint8_t code)
{
    if (code == 0) return NULL;
    switch (code) {
        case 1:  return "Sincr fase";
        case 2:  return "Otro inv";
        case 3:  return "Sin otro inv";
        case 4:  return "Sin AC";
        case 5:  return "VAC alta";
        case 6:  return "Fase corto";
        case 7:  return "BMS L1<>L2";
        case 10: return "Sincr fase";
        case 14: return "Hardware";
        case 16: return "Relay";
        case 17: return "Sin master";
        case 22: return "OS error";
        case 24: return "Switch over";
        case 25: return "Firmware";
        case 26: return "Internal";
        default: return "VE.Bus err";
    }
}
