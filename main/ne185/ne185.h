/* ne185.h — Maestro RS-485 directo al derivador Nordelettronica NE185
 *
 * La placa Guition JC1060P470C_I lleva un MAX485 onboard (U8) cableado a:
 *   - GPIO 26  -> TX1 -> pin D del MAX485
 *   - GPIO 27  -> RX1 -> pin R del MAX485
 *   - DE/RE controlados automaticamente por una NAND gate (U7) a partir
 *     del propio TX1, sin GPIO extra.
 *   - Conector J5 (MX 1.25 4P) expone A, B, GND, +5V para enchufar el
 *     cable del NE187 directamente.
 *
 * Asi que NO hace falta caja externa con ESP32+MAX485+buck. El P4 habla
 * RS-485 directamente con el NE185 y reemplaza al panel NE187 retirado.
 *
 * Protocolo (38400 8N1):
 *   - Frame de 20 bytes; cabecera 0xFF en posicion 0 y 14.
 *   - Checksum sobre bytes 0..17, comparada con b[18..19] mod 128 - 2.
 *   - Comando idle FF 40 00 C0 BF cada 5 s para mantener vivo el bus.
 *   - Comandos toggle: luz interior (i), exterior (o), bomba (p).
 *
 * API publica identica al antiguo componente camper para minimizar
 * cambios en las vistas LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NE185_UART_NUM
#define NE185_UART_NUM   UART_NUM_1
#endif
#ifndef NE185_UART_RX
#define NE185_UART_RX    GPIO_NUM_27   /* RX1 -> pin R del MAX485 onboard */
#endif
#ifndef NE185_UART_TX
#define NE185_UART_TX    GPIO_NUM_26   /* TX1 -> pin D del MAX485 onboard */
#endif
#ifndef NE185_UART_BAUD
#define NE185_UART_BAUD  38400
#endif

typedef struct {
    uint8_t  s1;          /* 0..3, nivel agua limpia */
    uint8_t  r1;          /* 0..3, nivel grises */
    bool     light_in;    /* salida luz interior ON/OFF */
    bool     light_out;   /* salida luz exterior ON/OFF */
    bool     pump;        /* bomba ON/OFF */
    bool     shore;       /* conectado a red 230 V */
    bool     fresh;       /* hubo trama valida en los ultimos 30 s */
    uint32_t last_update_ms;
} ne185_data_t;

void ne185_init(void);
void ne185_get(ne185_data_t *out);
void ne185_send_cmd(char cmd);   /* 'i', 'o', 'p' */

#ifdef __cplusplus
}
#endif
