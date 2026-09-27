/* Modo ausente / vigilancia.
 *
 * Apaga la pantalla y la MANTIENE apagada (precedencia maxima sobre auto-brillo,
 * franja nocturna y screensaver). El toque normal no la despierta. Se sale con
 * 4 toques en CUALQUIERA de las 4 esquinas (ver ausente_mode.c).
 *
 * Activacion: switch en Ajustes -> "Autocaravana", o desde la app / el navegador
 * (GET /ausente?on). Al activar hay una cuenta atras de 10 s (cancelable apagando
 * el switch) antes de entrar.
 *
 * NOTA(vigilancia): la deteccion de movimiento + captura de foto/video se
 * enganchan en otro ciclo (cuando ausente_is_active() == true).
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* on=true: inicia la cuenta atras de 10 s y luego activa el modo. Devuelve
 * false SIN empezar la cuenta atras si la SD no esta montada -- vigilancia
 * necesita la SD para guardar las fotos (via vig_sd_drain_task en camera.c);
 * armar igual dejaria "modo ausente ACTIVO" en el log mientras cada foto
 * fallaba en silencio al no encontrar donde escribirla (solo un WARN en
 * el log de la camara, invisible para quien confia en que esto vigila) --
 * o si la camara no llego a arrancar (camera_init fallo): sin camara no hay
 * vigilancia. on=false: cancela la cuenta atras (si pendiente) o sale del
 * modo (si activo); siempre devuelve true. */
bool ausente_request(bool on);

/* Igual, diciendo por donde viene la orden: el cartel de la cuenta atras no
 * puede decir "apaga el interruptor" a quien la activo desde la app, donde ese
 * interruptor no esta a la vista (hallazgo 1.I6 de la auditoria del 15-sep). */
bool ausente_request_ex(bool on, bool via_http);

/* Motivo del ultimo rechazo de ausente_request(true), o NULL si el ultimo
 * intento fue aceptado. Distingue "sin SD" de "camara no responde" para que
 * cada llamador (dialog del switch, handler /ausente) lo explique bien. */
const char *ausente_rechazo_razon(void);

/* true cuando el modo esta plenamente activo (pantalla apagada). */
bool ausente_is_active(void);

/* Aviso de una sola vez: "el P4 se reinicio con la vigilancia puesta", o NULL si
 * no paso. Se rellena en ausente_boot_check() (arranque) y lo publica el portal
 * en /ausente para que la app lo pueda decir: tras un reinicio el modo queda
 * apagado y la furgo sin vigilancia, y hasta ahora nadie se enteraba. */
const char *ausente_aviso_reinicio(void);

/* Lee de NVS si la vigilancia quedo puesta antes de este arranque (corte de
 * corriente o reinicio) y prepara el aviso. Se llama UNA vez, al arrancar. */
void ausente_boot_check(void);

#ifdef __cplusplus
}
#endif
