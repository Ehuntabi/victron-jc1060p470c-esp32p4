/* udp_tx.h - Publisher UDP-broadcast del 7" hacia el satélite mini.
 *
 * Emite un mini_msg_t (ver net/mini_proto.h) por broadcast a
 * 192.168.4.255:MINI_PROTO_UDP_PORT cada segundo.
 *
 * Requisitos:
 *   - Wi-Fi inicializado y SoftAP arrancado (lo hace wifi_ap_init()).
 *   - El AP debe tener el netif activo (DHCP server up).
 *
 * Llamar UNA vez tras wifi_ap_init() / config_server_start().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void udp_tx_start(void);

#ifdef __cplusplus
}
#endif
