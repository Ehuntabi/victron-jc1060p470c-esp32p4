/* satelite.h — hace de cabina/satelite contra la P4: recibe su telemetria UDP y
 * le manda las mismas peticiones HTTP. Herramienta de banco: ver LEEME.md. */
#pragma once
#include <stdbool.h>

bool sat_iniciar(const char *ssid, const char *clave,
                 const char *usuario, const char *clave_portal);
void sat_parar(void);
void sat_informe(void);
bool sat_activo(void);
