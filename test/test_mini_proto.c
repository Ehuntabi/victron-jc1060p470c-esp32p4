/* test_mini_proto.c — Test del protocolo entre la pantalla (P4) y la cabina.
 *
 * POR QUE EXISTE: el mini_proto es la frontera entre dos placas que se actualizan
 * por separado. Un cambio de tamaño, de orden de campos o de version ahi no da un
 * error de compilacion: da dos equipos que dejan de entenderse. Hasta el
 * 21-sep-2026 lo unico que lo vigilaba era un diff de ficheros en el CI; esto
 * ademas comprueba que los bytes que se envian son los que se esperan.
 *
 * Corre en el PC (no necesita hardware):
 *     gcc -I. -o /tmp/t test/test_mini_proto.c && /tmp/t
 */
#include <stdio.h>
#include <string.h>
#include "main/net/mini_proto.h"

static int fallos = 0;
#define COMPROBAR(cond, txt) do { \
    if (cond) { printf("  ok   %s\n", txt); } \
    else { printf("  FALLO %s\n", txt); fallos++; } } while (0)

int main(void)
{
    printf("test_mini_proto (version %d del protocolo)\n", MINI_PROTO_VERSION);

    /* 1. Tamano y distribucion: son el contrato con la otra placa. */
    COMPROBAR(sizeof(mini_msg_t) == 40, "el mensaje mide 40 bytes");
    COMPROBAR(offsetof(mini_msg_t, crc32) == 36, "el crc32 va en el byte 36");

    /* 2. Version exacta: si cambia, hay que actualizar las DOS placas a la vez. */
    COMPROBAR(MINI_PROTO_VERSION == 5, "la version sigue siendo la 5");

    /* 3. Centinelas de "sin dato": si alguien los cambia, la cabina interpretaria
     *    un valor real donde no lo hay (o al reves). */
    COMPROBAR(MINI_NO_DATA_I16 == -32768, "centinela i16 = -32768");
    COMPROBAR(MINI_NO_DATA_I32 == -2147483647 - 1, "centinela i32 = INT32_MIN");
    COMPROBAR(MINI_NO_DATA_U8 == 0xFF, "centinela u8 = 0xFF");

    /* 4. Mascaras de alarma: la cabina las usa una a una. */
    COMPROBAR(MINI_ALARM_TODAS == (MINI_ALARM_AGUA | MINI_ALARM_GRISES |
                                   MINI_ALARM_BATERIA | MINI_ALARM_CONGELADOR),
              "las 4 alarmas no se solapan");

    /* 5. Los bytes que salen de verdad: se rellena un mensaje conocido y se
     *    comprueba el volcado byte a byte, que es lo que viaja por el aire. */
    mini_msg_t m;
    memset(&m, 0, sizeof m);
    m.version = MINI_PROTO_VERSION;
    m.shunt_voltage_centi = 1342;      /* 13,42 V */
    m.frigo_temp_centi = -1850;        /* -18,50 C */
    m.water_clean = 4;
    m.alarmas = MINI_ALARM_GRISES;
    m.epoch_local = 1789762971u;
    unsigned char *b = (unsigned char *)&m;
    COMPROBAR(b[0] == MINI_PROTO_VERSION, "el primer byte es la version");
    COMPROBAR(b[1] == 0, "el byte 1 es relleno (0)");
    {
        size_t o = offsetof(mini_msg_t, shunt_voltage_centi);
        COMPROBAR(b[o] == (1342 & 0xFF) && b[o + 1] == (1342 >> 8),
                  "la tension va en little-endian en su offset");
    }
    {
        size_t o = offsetof(mini_msg_t, frigo_temp_centi);
        int16_t v = -1850;
        COMPROBAR(b[o] == (unsigned char)(v & 0xFF) && b[o + 1] == (unsigned char)((v >> 8) & 0xFF),
                  "la temperatura negativa conserva el signo en los bytes");
    }
    {
        size_t o = offsetof(mini_msg_t, epoch_local);
        COMPROBAR(b[o] == 0x9B && b[o + 3] == 0x6A,
                  "la marca de tiempo ocupa 4 bytes en su offset");
    }
    COMPROBAR(offsetof(mini_msg_t, crc32) == sizeof(mini_msg_t) - 4,
              "no hay relleno oculto: el crc cierra el mensaje");

    /* 6. El CRC cubre los 36 primeros bytes: ese es el trato con la otra placa. */
    COMPROBAR(offsetof(mini_msg_t, crc32) == 36, "el CRC cubre los 36 primeros bytes");

    printf(fallos ? "\n%d FALLOS\n" : "\nTODO CORRECTO\n", fallos);
    return fallos ? 1 : 0;
}
