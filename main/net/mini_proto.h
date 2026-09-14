/* mini_proto.h - Protocolo entre el 7" (P4+C6) y el satelite 3,5" de cabina.
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
 * Sentido de la telemetria: 7" -> satelite, y SOLO en ese sentido. Lo que va de
 * vuelta (apuntes del cuaderno de viaje y la orden de silenciar una alarma) NO
 * viaja por aqui: va por HTTP contra el portal del 7", que confirma la entrega
 * (ver net/p4_api.c de la 35cabina). Un silencio perdido por UDP en silencio
 * dejaria la alarma pitando sin que nadie sepa por que.
 *
 * Para valores "sin dato" usar el sentinel definido por campo.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINI_PROTO_VERSION   5
#define MINI_PROTO_UDP_PORT  4242
#define MINI_NO_DATA_I16     INT16_MIN   /* -32768 = sin sensor / sin dato */
#define MINI_NO_DATA_I32     INT32_MIN
#define MINI_NO_DATA_U8      0xFF

/* Bitmask de alarmas activas. Se usa en DOS sitios y con el MISMO significado
 * en los dos; de eso va este byte (14-sep-2026):
 *   - en la telemetria (mini_msg.alarmas), de la P4 hacia la 35cabina, para
 *     que la cabina sepa cual de sus tarjetas esta en alarma y le ponga el
 *     icono del altavoz;
 *   - en la orden de silencio de la cabina hacia la P4 (POST /api/alarma,
 *     ver udp_tx.c aqui y net/p4_api.c en la 35cabina), diciendo CUAL se
 *     silencia.
 *
 * QUE SIGNIFICA "ACTIVA": que la condicion de alarma se cumple, este sonando
 * o este silenciada. El silencio corta el pitido, no la senal visual (decision
 * del 13-sep-2026), asi que el parpadeo, el aviso y este byte siguen igual
 * mientras dure la condicion. Lo que este byte NO dice es si esta sonando:
 * eso lo sabe quien tiene el estado de silencio (la P4, alarm_estado.c).
 *
 * El bit se pone y se quita solo: al recuperarse la alarma el silencio se
 * rearma, de modo que si vuelve a pasar, vuelve a sonar. */
#define MINI_ALARM_AGUA       0x01   /* agua limpia en reserva */
#define MINI_ALARM_GRISES     0x02   /* aguas grises llenas */
#define MINI_ALARM_BATERIA    0x04   /* bateria por debajo del umbral critico */
#define MINI_ALARM_CONGELADOR 0x08   /* congelador por encima de su umbral */
#define MINI_ALARM_TODAS      0x0F

/* Umbral de cordura para el reloj: cualquier fecha anterior a esta (1-ene-2021)
 * es el 1970 que devuelve el sistema mientras el RTC no ha puesto la hora.
 * Sirve para no mandar un día de calendario inventado. */
#define MINI_EPOCH_VALIDO    1609459200L

/* Payload broadcast 7" -> mini. Tamaño fijo, sin punteros, packed.
 * Total: 40 bytes (sizeof(struct mini_msg), verificado con el compilador).
 * Eran 32 hasta la v3 (que añadió epoch_local), 36 hasta la v4 (gps_estado) y
 * 38 hasta la v5 (alarmas). */
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

    /* Aguas (NE185 RS-485, niveles 0..4). MINI_NO_DATA_U8 si !fresh. */
    uint8_t  water_clean;         /* s1 limpia */
    uint8_t  water_gray;          /* r1 grises */

    /* Canal auxiliar del SmartShunt (mismo campo "aux" que victron_records.h:
     * crudo, la unidad depende de aux_input). MINI_NO_DATA_U8 en aux_input si
     * el dato no esta fresco (bat_fresh=false, no bat_has -- ver el comentario
     * de build_msg() en udp_tx.c). Ver ui_format_aux_value() en el 7" para
     * el mismo criterio de formato. */
    uint16_t aux_value_raw;       /* V*100 (aux_input 0/1) o Kelvin*100 (2) */
    uint8_t  aux_input;           /* 0=voltage2(arranque), 1=mid-point, 2=temp */

    /* Exterior. Sin sensor todavía en el 7" -> se envía MINI_NO_DATA_I16. */
    int16_t  exterior_temp_centi;
    /* 1 = el 7"(P4) está en salvapantallas. La P4 lo manda (udp_tx.c) pero el
     * satelite (35cabina) todavia no lo lee -- no tiene salvapantallas propio
     * (Fase 0, ver lv_port.c). Confirmado muerto en recepcion el 07-sep-2026. */
    uint8_t  screensaver;

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

    /* Estado del GPS de la P4, para que el satélite pueda pintar su indicador.
     * TRES estados y no un sí/no, porque cuesta el mismo byte y la diferencia
     * importa: "no llega nada" manda a mirar el cable, y "buscando" solo pide
     * esperar — un GPS recién encendido tarda un par de minutos. Con un
     * booleano los dos casos se verían igual.
     *
     * NO se manda la posición (decisión del usuario, 23-ago-2026): el satélite
     * solo enseña si hay GPS. Si algún día se quiere que sus apuntes lleven el
     * sitio donde ocurrieron, habrá que subir el protocolo OTRA VEZ y regrabar
     * las dos pantallas. */
    uint8_t  gps_estado;          /* 0=sin datos, 1=buscando, 2=posición fijada */
    uint8_t  _pad3;               /* relleno heredado: mantiene los offsets */

    /* Alarmas ACTIVAS (MINI_ALARM_*, ver arriba). Va AQUI, en el hueco de
     * relleno que ya existia antes del CRC (el byte 34, que hasta la v4 nadie
     * leia), para no mover ningun campo de los que ya viajaban.
     *
     * Existe para que la cabina pueda poner el icono del altavoz en la tarjeta
     * que esta en alarma y mandar la orden de silencio sabiendo CUAL es. Hasta
     * ahora la cabina deducia la alarma por su cuenta a partir de los niveles
     * (agua a 0, grises lleno...) y el pitido del congelador no lo veia nadie
     * desde alli: la P4 es la unica que sabe si una alarma esta activa de
     * verdad, porque es la unica que tiene los umbrales y las temporizaciones. */
    uint8_t  alarmas;

    uint8_t  _pad4[1];            /* el relleno que alinea el uint32 del CRC */

    uint32_t crc32;               /* CRC32 sobre los bytes [0 .. crc32) */
};

typedef struct mini_msg mini_msg_t;

/* El comentario de arriba ("Total: 40 bytes... verificado con el compilador")
 * no era una asercion real, solo lo decia: nada impedia que un campo nuevo
 * cambiara el tamano sin que nadie se enterase hasta que la P4 y el satelite
 * empezaran a rechazarse los paquetes en produccion. Con esto, si algun dia
 * dejan de cuadrar, el build de LOS DOS proyectos falla en el sitio exacto,
 * no en el monitor serie semanas despues. Detectado auditando el 07-sep-2026. */
_Static_assert(sizeof(mini_msg_t) == 40,
               "mini_msg_t cambio de tamano: sube MINI_PROTO_VERSION y "
               "actualiza este numero (y el comentario de mas arriba)");
_Static_assert(offsetof(mini_msg_t, crc32) == 36,
               "el CRC32 ya no esta al final de la struct: build_msg() en "
               "udp_tx.c y la comprobacion en udp_rx.c asumen los bytes "
               "[0..crc32) como el area protegida");

#ifdef __cplusplus
}
#endif
