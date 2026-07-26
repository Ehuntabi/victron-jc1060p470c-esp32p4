#pragma once
/* Logica pura de las sondas del frigo: quien es quien tras un escaneo del bus.
 *
 * Sin dependencias de ESP-IDF a proposito, para poder probarla en el PC con gcc
 * (ver test/test_frigo_sondas.c). frigo.c habla con el hardware; esto decide.
 *
 * La idea: cada DS18B20 trae una direccion ROM unica de fabrica. Los roles
 * (aletas / congelador / exterior) se guardan por ESA direccion, no por la
 * posicion en la lista, para que al aparecer o desaparecer una sonda los demas
 * roles no se desplacen. */
#include <stdint.h>
#include <stdbool.h>

#define FRIGO_SONDAS_MAX     3
#define FRIGO_SONDA_AUSENTE  0xFF   /* el rol no tiene sonda presente ahora mismo */

/* Que hay que decidir. Se combinan con OR. */
enum {
    FRIGO_SCAN_HAY_NUEVA = 1 << 0,  /* hay una sonda presente sin rol asignado */
    FRIGO_SCAN_FALTA_UNA = 1 << 1,  /* hay un rol cuya sonda no responde */
};

typedef struct {
    uint8_t flags;                            /* combinacion de FRIGO_SCAN_* */
    uint8_t asignacion[FRIGO_SONDAS_MAX];     /* rol -> indice en 'encontradas', o FRIGO_SONDA_AUSENTE */
    uint8_t n_sin_rol;                        /* cuantas sondas presentes no tienen rol */
} frigo_scan_result_t;

/* Resuelve que indice ocupa cada rol tras un escaneo.
 *   encontradas[0..n-1] : direcciones ROM leidas del bus ahora mismo
 *   role_addr[rol]      : direccion guardada para ese rol (0 = rol sin asignar)
 * No modifica role_addr. n se recorta a FRIGO_SONDAS_MAX. */
frigo_scan_result_t frigo_sondas_resolver(const uint64_t *encontradas, int n,
                                          const uint64_t role_addr[FRIGO_SONDAS_MAX]);

/* Primer arranque con esta version: role_addr esta a cero y lo unico guardado es
 * la asignacion por posicion de siempre. Rellena role_addr con las direcciones
 * que le correspondan para no perder la configuracion del usuario.
 * Devuelve true si escribio algo. No hace nada si role_addr ya tiene contenido. */
bool frigo_sondas_migrar(const uint64_t *encontradas, int n,
                         const uint8_t asignacion[FRIGO_SONDAS_MAX],
                         uint64_t role_addr[FRIGO_SONDAS_MAX]);

/* Banderas que acaban de ACTIVARSE. Sirve para avisar al usuario solo cuando
 * aparece algo que decidir, y no repetir el aviso en cada escaneo mientras la
 * situacion siga igual. */
uint8_t frigo_sondas_evento(uint8_t flags_antes, uint8_t flags_ahora);
