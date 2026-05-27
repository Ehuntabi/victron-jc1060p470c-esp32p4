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
    uint8_t  s1;          /* nivel agua limpia: 0=Reserva, 1=1/4, 2=2/4, 3=3/4, 4=4/4,
                           * 0xFF = sin datos / combo de probes invalido */
    uint8_t  r1;          /* nivel grises: 0=vacio (OK), >0=lleno (cualquier probe activo),
                           * 0xFF = sin datos. Encoding exacto NE185 pendiente de validar */
    bool     light_in;    /* salida luz interior ON/OFF */
    bool     light_out;   /* salida luz exterior ON/OFF */
    bool     pump;        /* bomba ON/OFF */
    bool     shore;       /* conectado a red 230 V */
    float    battery1_v;  /* bateria servicio (V), 0 si no disponible */
    float    battery2_v;  /* bateria motor (V), 0 si no disponible */
    bool     fresh;       /* hubo trama valida en los ultimos 30 s */
    uint32_t last_update_ms;
} ne185_data_t;

void ne185_init(void);
void ne185_get(ne185_data_t *out);
void ne185_send_cmd(char cmd);   /* 'i', 'o', 'p' */

/* Inyectar estado simulado (para demos / debug visual). Marca fresh=true
 * automaticamente. Reemplaza por completo el contenido actual. */
void ne185_sim_inject(uint8_t s1, uint8_t r1,
                      bool light_in, bool light_out,
                      bool pump, bool shore);

/* Contador de tramas RX validas recibidas del NE185. Se usa como
 * feedback en vivo en la UI ("NE185 RX: N tramas") para saber si hay
 * comunicacion sin abrir el log de la SD. */
uint32_t ne185_get_sniff_count(void);

/* Loguea un marcador en el log con prefijo "MARK:" para autoetiquetar
 * la captura (ej. "MARK: Luz INT" justo antes de pulsar). */
void ne185_log_marker(const char *what);

/* Inyecta una trama RAW de 20 bytes (tipica respuesta de la centralita)
 * y la procesa como si hubiera llegado del bus. Para test/simulacion
 * sin HW. Devuelve true si la trama paso la validacion y se decodifico. */
bool ne185_sim_inject_raw(const uint8_t *frame20);

/* Devuelve la ultima trama RX valida (20 bytes) en `out`. Util para vista
 * de diagnostico en pantalla (ver bytes raw sin sacar SD). Si `n_frames_ok`
 * y `n_frames_fail` no son NULL, devuelve los contadores actuales. */
void ne185_get_last_raw(uint8_t out[20], uint32_t *n_frames_ok,
                        uint32_t *n_frames_fail);

/* Verbose log: si ON, loguea el hex de cada frame RX recibido del bus
 * (util para diagnosticar cambios en bytes desconocidos como b9/b14).
 * Si OFF, solo loguea cambios de estado (default, evita spam a 16Hz).
 *
 * Compat: estas funciones se llamaban antes set_sniffer_tx/get_sniffer_tx
 * pero ese concepto desaparecio - el master TX siempre esta activo. */
void ne185_set_verbose(bool enable);
bool ne185_get_verbose(void);

/* Sniff mode: si true, NO emite cmds. Solo lee el bus. Util para detectar
 * si los frames del NE185 son emision propia (vienen aunque no enviamos)
 * o respuesta a nuestros cmds. */
void ne185_set_polling_paused(bool paused);
bool ne185_get_polling_paused(void);

/* Inyecta un cmd custom para que la siguiente iteracion del rs485_task lo
 * envie en lugar del cmd normal. Formato: FF <b1> 00 00 (FF+b1)&0xFF.
 * Util para probar otros comandos (FF 50, FF 60, etc.) y ver respuesta.
 * Solo envia UNA vez (no en loop). */
void ne185_inject_custom_cmd(uint8_t b1);

#ifdef __cplusplus
}
#endif
