/* camper.h — Integracion con derivador Nordelettronica NE185
 *
 * Sustituye al panel NE187 (eliminado fisicamente). Un ESP32 externo
 * conectado por UART al P4 actua como maestro del bus RS485 hacia el
 * NE185 y nos envia un JSON cada segundo con el estado de los tanques,
 * de la red 230 V y de las salidas (luz interior, exterior, bomba).
 *
 * Protocolo entre ESP32 y P4:
 *   - El ESP envia lineas JSON terminadas en \n:
 *       {"s1":3,"r1":2,"lin":1,"lout":0,"pump":0,"shore":1}
 *   - El P4 envia 1 byte por accion:
 *       'i' = toggle luz interior
 *       'o' = toggle luz exterior
 *       'p' = toggle bomba
 *
 * Niveles s1, r1: 0..3 (sumatorio de sondas). 100 % equivale a valor 3.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pines del UART del P4 (ajustar a los disponibles en tu Guition).
 *
 *   - UART0 lo usa la consola/debug.
 *   - UART2 (GPIO32/33) lo usa el PZEM-004T.
 *   - UART1 esta libre: lo usamos para hablar con el ESP del RS485.
 *
 * Los GPIO de abajo son los pines fisicos donde conectaras el cable de
 * 3 hilos. Cambialos si no estan accesibles en tu placa Guition. */
#ifndef CAMPER_UART_NUM
#define CAMPER_UART_NUM   UART_NUM_1
#endif
/* GPIOs usados en el proyecto y NO disponibles:
 *   4            frigo 1-Wire (DS18B20)
 *   5            frigo PWM ventilador
 *   7, 8         I2C (touch, RTC, codec)
 *   9, 10, 12, 13 audio I2S
 *   11           PA_CTRL audio amp
 *   14-19, 54    SDIO ESP32-C6 (Wi-Fi/BT)  <-- NO TOCAR
 *   23           LCD backlight
 *   27           LCD RST
 *   32, 33       PZEM-004T (UART2)
 *   39-44        microSD
 *   47, 48       touch INT / RST
 *
 * Pines elegidos en JP1: GPIO 1 (pin 7) y GPIO 2 (pin 9). Ambos
 * verdes/libres en el header, adyacentes para cableado simple. Si los
 * necesitas para otra cosa, otras opciones libres del JP1: 3, 20, 45, 46. */
#ifndef CAMPER_UART_RX
#define CAMPER_UART_RX    GPIO_NUM_1      /* JP1 pin 7  — P4 RX <- ESP32 TX */
#endif
#ifndef CAMPER_UART_TX
#define CAMPER_UART_TX    GPIO_NUM_2      /* JP1 pin 9  — P4 TX -> ESP32 RX */
#endif
#ifndef CAMPER_UART_BAUD
#define CAMPER_UART_BAUD  115200
#endif

/* Snapshot de los datos recibidos. Lectura siempre via camper_get(). */
typedef struct {
    uint8_t  s1;          /* 0..3, nivel agua limpia */
    uint8_t  r1;          /* 0..3, nivel grises */
    bool     light_in;    /* salida luz interior ON/OFF */
    bool     light_out;   /* salida luz exterior ON/OFF */
    bool     pump;        /* bomba ON/OFF */
    bool     shore;       /* conectado a red 230 V */
    bool     fresh;       /* hubo trama valida en los ultimos 5 s */
    uint32_t last_update_ms;
} camper_data_t;

/* Inicializa UART, tarea lectora y mutex. Llamar una vez en app_main(). */
void camper_init(void);

/* Lee el snapshot actual de forma segura. */
void camper_get(camper_data_t *out);

/* Envia un comando al ESP RS485:
 *   'i' toggle luz interior
 *   'o' toggle luz exterior
 *   'p' toggle bomba                                                  */
void camper_send_cmd(char cmd);

/* Callback opcional para que la UI se entere de cambios.
 * Si lo registras, se llama desde la tarea UART tras parsear cada JSON. */
typedef void (*camper_on_update_cb_t)(void);
void camper_set_on_update_cb(camper_on_update_cb_t cb);

#ifdef __cplusplus
}
#endif
