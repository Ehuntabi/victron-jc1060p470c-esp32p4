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

#ifdef __cplusplus
}
#endif
