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

/**
 * @brief true si la ULTIMA comprobacion de VLF (en rtc_init o en
 * rtc_check_vlf_now) lo encontro activo -- señal de pila CR1220 baja.
 */
bool rtc_battery_low(void);
