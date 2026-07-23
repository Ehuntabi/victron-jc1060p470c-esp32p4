/* mini_proto.h - Protocolo UDP-broadcast entre 7" (P4+C6) y mini (C6 1.47")
 *
 * MANTENER SINCRONIZADO con:
 *   - 7"   : main/net/mini_proto.h  (este fichero)
 *   - mini : main/net/mini_proto.h  (copia idéntica)
 *
 * Cambios en el struct requieren bump de MINI_PROTO_VERSION y recompilar AMBOS.
 *
 * Transporte: UDP broadcast a 192.168.4.255:MINI_PROTO_UDP_PORT.
 *   (intentamos primero ESP-NOW pero esp_hosted no exporta esa API.)
 * Topología: el mini se asocia al SoftAP del 7" como cliente STA (DHCP).
 * Cadencia: 1 Hz desde el 7".
 *
 * Para valores "sin dato" usar el sentinel definido por campo.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINI_PROTO_VERSION   1
#define MINI_PROTO_UDP_PORT  4242
#define MINI_NO_DATA_I16     INT16_MIN   /* -32768 = sin sensor / sin dato */
#define MINI_NO_DATA_I32     INT32_MIN
#define MINI_NO_DATA_U8      0xFF

/* Payload broadcast 7" -> mini. Tamaño fijo, sin punteros, packed.
 * Total: 28 bytes. */
struct __attribute__((packed)) mini_msg {
    uint8_t  version;             /* MINI_PROTO_VERSION */
    uint8_t  _pad0;               /* alignment */

    /* Batería principal (SmartShunt / BMV) */
    int16_t  shunt_soc_deci;      /*  % * 10   ej: 782 = 78.2 %  */
    int16_t  shunt_voltage_centi; /*  V * 100  ej: 1342 = 13.42 V */
    int32_t  shunt_current_milli; /*  A * 1000 signo (+ carga, - descarga) */

    /* DC/DC (Orion XS / DCDC converter). Sólo voltajes, no hay corriente
     * cacheada en el 7" todavía. */
    int16_t  dcdc_v_in_centi;
    int16_t  dcdc_v_out_centi;
    uint8_t  dcdc_state;          /* 0=off, 3=bulk, 4=absorption, 5=float */
    uint8_t  _pad1;

    /* Frigorífico (sensor 1-Wire si está conectado). */
    int16_t  frigo_temp_centi;    /*  °C * 100. MINI_NO_DATA_I16 si sin sensor */
    uint8_t  frigo_fan_pct;       /* 0..100. >0 implica compresor/vent ON */
    uint8_t  _pad2;

    /* Aguas (NE185 RS-485, niveles 0..3). MINI_NO_DATA_U8 si !fresh. */
    uint8_t  water_clean;         /* s1 limpia */
    uint8_t  water_gray;          /* r1 grises */
    uint8_t  _pad3;
    uint8_t  _pad4;

    /* Exterior. Sin sensor todavía en el 7" -> se envía MINI_NO_DATA_I16. */
    int16_t  exterior_temp_centi;
    uint8_t  _pad5;
    uint8_t  screensaver;         /* 1 = el 7"(P4) está en salvapantallas → el mini atenúa su pantalla */

    uint32_t crc32;               /* CRC32 sobre los bytes [0 .. crc32) */
};

typedef struct mini_msg mini_msg_t;

#ifdef __cplusplus
}
#endif
