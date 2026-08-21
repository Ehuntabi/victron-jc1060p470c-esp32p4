/* slave_ota.h - Actualizar el firmware del C6 (la radio) desde el P4.
 * Ver slave_ota.c para el porque, el como y los riesgos. */
#pragma once

#include <stdbool.h>

/* Lanza la actualizacion en una tarea aparte y vuelve en el acto. Tarda unos
 * minutos; el progreso va al log. Al terminar bien, los dos chips se
 * reinician solos. NO se llama sola en el arranque: solo desde Ajustes. */
void slave_ota_start(void);

/* Para no lanzar dos a la vez ni dejar tocar el boton mientras va. */
bool slave_ota_en_curso(void);
