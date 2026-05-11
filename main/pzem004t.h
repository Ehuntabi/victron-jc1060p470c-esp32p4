#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     has_data;      /* true si la ultima lectura fue valida */
    uint32_t age_ms;        /* edad de la ultima lectura valida */
    float    voltage_v;     /* tension AC (resolucion 0.1 V) */
    float    current_a;     /* corriente AC (0.001 A) */
    float    power_w;       /* potencia activa (0.1 W) */
    uint32_t energy_wh;     /* energia acumulada total (1 Wh, no-volatile en el modulo) */
    float    freq_hz;       /* frecuencia (0.1 Hz) */
    float    power_factor;  /* 0.00..1.00 */
    bool     alarm;         /* sobrepotencia segun threshold del modulo */
    uint32_t total_reads;
    uint32_t failed_reads;
} pzem_data_t;

typedef struct {
    uart_port_t uart_num;       /* p.ej. UART_NUM_2 */
    gpio_num_t  tx_gpio;        /* p.ej. GPIO 32 */
    gpio_num_t  rx_gpio;        /* p.ej. GPIO 33 */
    uint8_t     slave_address;  /* default 0x01 */
    uint32_t    poll_period_ms; /* p.ej. 2000 */
} pzem_config_t;

/* Inicializa la UART y arranca la task de polling en background. No bloquea
 * si no hay PZEM conectado: simplemente acumula failed_reads. Llamar UNA vez. */
esp_err_t pzem_init(const pzem_config_t *cfg);

/* Snapshot de la ultima lectura. Devuelve has_data=false si nunca se ha
 * recibido respuesta valida. Thread-safe (mutex interno). */
void pzem_get(pzem_data_t *out);

#ifdef __cplusplus
}
#endif
