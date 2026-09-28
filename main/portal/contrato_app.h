/* contrato_app.h — Lo que la app del movil espera de esta P4, en un sitio que se
 * puede probar SIN placa: el CI compila contrato_app.c con gcc y ejecuta
 * test/test_contrato_app.c (job "contrato_app").
 *
 * Nace el 28-sep-2026, despues de que el borrado de viajes estuviera roto desde la
 * v3.29 sin que nadie lo viera: la guarda de /api/viaje exigia "id" numerico a
 * TODAS las operaciones y "borrar" lleva "carpeta", asi que la app se llevaba un
 * 400 "faltan op o id". Leccion: compilar los dos lados no prueba el contrato
 * entre ellos, y eso habia que comprobarlo en algun sitio automatico.
 *
 * Si cambias los campos que pide una operacion, o los que publica /ausente,
 * actualiza tambien test/test_contrato_app.c y el fichero de la app que lo use
 * (victron-app/lib/data/api_client.dart). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Que le falta al cuerpo de POST /api/viaje para poder atenderlo.
 * Devuelve NULL si esta completo, o el texto del 400 ("falta op",
 * "falta 'carpeta'", "faltan op o id"), que es lo que contesta el handler. */
const char *viaje_campos_que_faltan(const char *op, bool hay_carpeta, bool hay_id);

/* Construye el JSON de GET /ausente (estado del modo vigilancia) y devuelve lo
 * que ocupa, como snprintf. Los textos se escapan (comillas, barras y saltos).
 * Lo publica el portal y lo lee la pantalla "Camara" de la app. */
int ausente_json(char *out, size_t n, bool activo, const char *motivo,
                 const char *salud, const char *aviso, int fotos, bool rotando);
