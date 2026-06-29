/* Modo ausente / vigilancia.
 *
 * Apaga la pantalla y la MANTIENE apagada (precedencia maxima sobre auto-brillo,
 * franja nocturna y screensaver). El toque normal no la despierta. Se sale con
 * 4 toques en la esquina superior izquierda.
 *
 * Activacion: switch en Settings -> "Sonido y avisos". Al activar hay una cuenta
 * atras de 10 s (cancelable apagando el switch) antes de entrar.
 *
 * NOTA(vigilancia): la deteccion de movimiento + captura de foto/video se
 * enganchan en otro ciclo (cuando ausente_is_active() == true).
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* on=true: inicia la cuenta atras de 10 s y luego activa el modo.
 * on=false: cancela la cuenta atras (si pendiente) o sale del modo (si activo). */
void ausente_request(bool on);

/* true cuando el modo esta plenamente activo (pantalla apagada). */
bool ausente_is_active(void);

#ifdef __cplusplus
}
#endif
