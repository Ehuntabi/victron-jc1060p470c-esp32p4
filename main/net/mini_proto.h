/* mini_proto.h - Protocolo UDP-broadcast del 7" (P4+C6) hacia el satelite.
 *
 * MANTENER SINCRONIZADO entre DOS proyectos, byte a byte:
 *   - 7"       : ~/joint/victron/main/net/mini_proto.h
 *   - 35cabina : ~/joint/35cabina/main/net/mini_proto.h
 *
 * El satelite viejo, el mini C6 1.47" (~/joint/victron_mini), esta DESCARTADO
 * desde el 20-ago-2026: la 35cabina lo sustituye. Ya NO condiciona el diseno de
 * este protocolo, que puede crecer por encima de los 32 bytes heredados. Aviso
 * practico por si alguno sigue enchufado: subir MINI_PROTO_VERSION lo deja mudo
 * (rechaza las versiones que no conoce).
 *
 * Cambios en el struct requieren bump de MINI_PROTO_VERSION y recompilar los dos.
 *
 * Transporte: UDP broadcast a 192.168.4.255:MINI_PROTO_UDP_PORT.
 *   (intentamos primero ESP-NOW pero esp_hosted no exporta esa API.)
 * Topologia: el satelite se asocia al SoftAP del 7" como cliente STA (DHCP).
 * Cadencia: 1 Hz desde el 7".
 *
 * Para valores "sin dato" usar el sentinel definido por campo.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINI_PROTO_VERSION   3
#define MINI_PROTO_UDP_PORT  4242
#define MINI_NO_DATA_I16     INT16_MIN   /* -32768 = sin sensor / sin dato */
#define MINI_NO_DATA_I32     INT32_MIN
#define MINI_NO_DATA_U8      0xFF

/* Umbral de cordura para el reloj: cualquier fecha anterior a esta (1-ene-2021)
 * es el 1970 que devuelve el sistema mientras el RTC no ha puesto la hora.
 * Sirve para no mandar un día de calendario inventado. */
#define MINI_EPOCH_VALIDO    1609459200L

/* Payload broadcast 7" -> mini. Tamaño fijo, sin punteros, packed.
 * Total: 36 bytes (sizeof(struct mini_msg), verificado con el compilador).
 * Eran 32 hasta la versión 3, que añadió epoch_local. */
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

    /* Canal auxiliar del SmartShunt (mismo campo "aux" que victron_records.h:
     * crudo, la unidad depende de aux_input). MINI_NO_DATA_U8 en aux_input si
     * no hay shunt (bat_has=false). Ver ui_format_aux_value() en el 7" para
     * el mismo criterio de formato. */
    uint16_t aux_value_raw;       /* V*100 (aux_input 0/1) o Kelvin*100 (2) */
    uint8_t  aux_input;           /* 0=voltage2(arranque), 1=mid-point, 2=temp */

    /* Exterior. Sin sensor todavía en el 7" -> se envía MINI_NO_DATA_I16. */
    int16_t  exterior_temp_centi;
    uint8_t  screensaver;         /* 1 = el 7"(P4) está en salvapantallas → el mini atenúa su pantalla */

    /* Reloj para el satélite, que no tiene ninguno: se apaga con el contacto y
     * al encender no sabe ni qué día es. Segundos desde 1970 YA DESPLAZADOS a
     * la hora local del 7" (epoch + huso), o 0 si su RTC todavía no tiene hora
     * buena.
     *
     * Desplazado a local y no en UTC a propósito: así el satélite saca el día
     * de calendario con una división entera (epoch_local / 86400) sin saber
     * nada de husos ni de horario de verano, y le hacen falta las dos cosas:
     *   - el DÍA para contar noches de parada (llegas el viernes por la tarde
     *     y te vas el sábado por la mañana: eso es UNA noche);
     *   - la HORA para las áreas que cobran por periodos de 24 h desde que
     *     entras, donde el calendario no sirve.
     *
     * uint32 sin signo llega hasta 2106. */
    uint32_t epoch_local;

    uint32_t crc32;               /* CRC32 sobre los bytes [0 .. crc32) */
};

typedef struct mini_msg mini_msg_t;

#ifdef __cplusplus
}
#endif
