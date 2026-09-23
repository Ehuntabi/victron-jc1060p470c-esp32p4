/* slave_ota.h - Grabar el firmware de la radio (C6) desde un fichero de la SD.
 *
 * ESTO NO VA EN EL FIRMWARE DE PUBLICACION. Es una herramienta de mantenimiento:
 * el guion aplicar.sh la inyecta en un arbol del firmware ANTIGUO (el que lleva
 * esp_hosted 0.0.27) para poder actualizar el C6 de una placa que todavia tenga
 * el firmware viejo. El motivo y el procedimiento completo, en LEEME.md.
 *
 * Por que desde la SD y no empotrada en el firmware: la imagen del C6 ocupa
 * 1,2 MB y la particion de aplicacion es de 4 MB; con la app actual (3 MB) no
 * cabe, y la compilacion falla con "All app partitions are too small".
 */
#pragma once

#include <stdbool.h>

/* Lanza la grabacion ya (boton de Ajustes -> Wi-Fi). */
void slave_ota_start(void);

/* Lanza la grabacion sola, tras N segundos, y SOLO si el fichero esta en la
 * SD. Pensado para no depender de que alguien encuentre el boton. */
void slave_ota_start_diferido(int segundos);

/* Hay una grabacion en marcha (para no lanzar dos). */
bool slave_ota_en_curso(void);

/* El fichero esta en la SD y tiene un tamano plausible. */
bool slave_ota_hay_fichero(void);
