/* udp_latido.h - Escucha el latido que manda la 35cabina (ver mini_proto.h).
 *
 * Llamar una vez tras wifi_ap_init(). El vigilante del AP (config_server_ap.c)
 * es quien lee el estado con udp_latido_estado() para decidir si el enlace esta
 * sano en los dos sentidos y, si no, repararlo.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  seg_desde_latido;    /* segundos desde el ultimo latido; -1 = nunca */
    int  seg_sin_datos_mini;  /* lo que dice la cabina que lleva sin telemetria; -1 = nunca */
    bool ip_fija_mini;        /* la cabina tuvo que ponerse la IP a mano */
    bool hay_datos;           /* ya ha llegado algun latido */
    uint32_t ip_mini;         /* IP desde la que llego el ultimo latido (in_addr_t) */
} udp_latido_info_t;

void udp_latido_start(void);
void udp_latido_estado(udp_latido_info_t *out);

/* Cuantas cabinas distintas han latido en los ultimos 'vivo_s' segundos. */
int udp_latido_fuentes_vivas(int vivo_s);

/* true si ALGUNA cabina que YA habia latido lleva mas de 'callada_s' segundos sin
 * hacerlo (su entrada sigue en la tabla, que dura 60 s).
 *
 * Existe por el fallo que cazo el red team del 14-oct-2026: si lo que se rompe es
 * la SUBIDA de una cabina (no le llega el latido a la P4), su entrada se queda
 * callada y nadie la contaba -- `udp_latido_estado()` devuelve la fuente MAS
 * RECIENTE, asi que la otra pantalla (sana) tapaba a la muda y la P4 se creia
 * sana y no reparaba nada. La 3,5" se quedaba sin telemetria hasta apagarla a
 * mano, y el log decia que todo iba bien. */
bool udp_latido_alguna_callada(int callada_s);

/* true si ALGUNA cabina que sigue latiendo dice llevar >= 15 s sin recibir
 * telemetria (y cuantos segundos, si 'seg_out' no es NULL). Es la pregunta que
 * tiene que hacer el vigilante del AP: con dos pantallas, mirar solo el ultimo
 * latido dejaba que la sana tapara a la muda (auditoria del 13-oct-2026). */
bool udp_latido_alguno_mudo(int *seg_out);

#ifdef __cplusplus
}
#endif
