#pragma once
#include <time.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief Inicializar el RTC RX8025T (I2C addr 0x32) usando el bus I2C del BSP.
 *        Comparte el bus (GPIO7=SDA, GPIO8=SCL) con el touch GT911 y el codec
 *        ES8311. Pila de respaldo CR1220.
 *
 * @param bus  Handle del bus I2C obtenido con bsp_i2c_get_handle()
 * @return ESP_OK si el chip responde, ESP_ERR_NOT_FOUND si no hay respuesta
 */
esp_err_t rtc_init(i2c_master_bus_handle_t bus);

/**
 * @brief Devuelve true si el RTC se inicializó correctamente
 */
bool rtc_is_ready(void);

/**
 * @brief Leer hora actual del RTC
 */
esp_err_t rtc_get_time(struct tm *tm_out);

/**
 * @brief Escribir hora en el RTC
 */
esp_err_t rtc_set_time(const struct tm *tm_in);

/* EL RTC GUARDA UTC desde el 14-oct-2026 (antes guardaba hora local, y al
 * arrancar se convertia con mktime(isdst=-1), o sea adivinando el horario de
 * verano: una placa apagada justo en el cambio de hora salia una hora desviada
 * hasta el primer fix del GPS).
 *
 * rtc_marca_utc() deja constancia en NVS de que lo que hay guardado ya es UTC;
 * rtc_es_utc() dice si esa marca existe. main.c la usa para migrar UNA vez: si
 * no hay marca, el valor que se lea es hora local vieja y se convierte como
 * antes (y se reescribe en UTC). */
void    rtc_marca_utc(void);
bool    rtc_es_utc(void);

/* Epoch UTC a partir de un struct tm que YA viene en UTC.
 *
 * Existe porque la newlib de ESP-IDF no trae timegm(), y mktime() aplicaria el
 * huso local: es el mismo algoritmo de dias civiles que usan gps.c y udp_tx.c
 * (days_from_civil de Hinnant). */
time_t  rtc_epoch_utc(const struct tm *tm_utc);

/**
 * @brief Vuelve a leer y comprobar el VLF (Voltage Low Flag, pila CR1220
 * baja) AHORA, no solo en rtc_init(). Antes SOLO se comprobaba una vez al
 * arrancar: si la pila se moria EN MARCHA (semanas/meses de uptime, caida
 * lenta tipica de una CR1220), nadie volvia a mirar el registro hasta el
 * siguiente power-on -- sin aviso previo para cambiarla, el reloj
 * simplemente empezaba a desviarse/perderse en el siguiente corte de luz.
 * Limpia el flag si lo encuentra activo (igual que rtc_init). Pensada para
 * llamarse periodicamente (p.ej. una vez por hora, la caida de una pila
 * no es urgente de detectar al segundo). Detectado por el usuario el
 * 09-sep-2026.
 * @return true si VLF estaba activo en ESTA comprobacion (pila sospechosa).
 */
bool rtc_check_vlf_now(void);
