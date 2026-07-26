#include "frigo_sondas.h"

/* Indice de 'addr' dentro de encontradas[], o FRIGO_SONDA_AUSENTE. */
static uint8_t indice_de(uint64_t addr, const uint64_t *encontradas, int n)
{
    if (addr == 0) return FRIGO_SONDA_AUSENTE;
    for (int i = 0; i < n; i++)
        if (encontradas[i] == addr) return (uint8_t)i;
    return FRIGO_SONDA_AUSENTE;
}

frigo_scan_result_t frigo_sondas_resolver(const uint64_t *encontradas, int n,
                                          const uint64_t role_addr[FRIGO_SONDAS_MAX])
{
    frigo_scan_result_t r = { .flags = 0, .asignacion = {0}, .n_sin_rol = 0 };
    if (n < 0) n = 0;
    if (n > FRIGO_SONDAS_MAX) n = FRIGO_SONDAS_MAX;

    /* Cada rol busca SU sonda por direccion. */
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++) {
        r.asignacion[rol] = indice_de(role_addr[rol], encontradas, n);
        if (role_addr[rol] != 0 && r.asignacion[rol] == FRIGO_SONDA_AUSENTE)
            r.flags |= FRIGO_SCAN_FALTA_UNA;
    }

    /* Sondas presentes que no reclama ningun rol. */
    for (int i = 0; i < n; i++) {
        bool reclamada = false;
        for (int rol = 0; rol < FRIGO_SONDAS_MAX && !reclamada; rol++)
            if (r.asignacion[rol] == (uint8_t)i) reclamada = true;
        if (!reclamada) {
            r.n_sin_rol++;
            r.flags |= FRIGO_SCAN_HAY_NUEVA;
        }
    }
    return r;
}

bool frigo_sondas_migrar(const uint64_t *encontradas, int n,
                         const uint8_t asignacion[FRIGO_SONDAS_MAX],
                         uint64_t role_addr[FRIGO_SONDAS_MAX])
{
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++)
        if (role_addr[rol] != 0) return false;   /* ya migrado */

    if (n < 0) n = 0;
    if (n > FRIGO_SONDAS_MAX) n = FRIGO_SONDAS_MAX;

    bool escrito = false;
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++) {
        uint8_t idx = asignacion[rol];
        if (idx < (uint8_t)n) {
            role_addr[rol] = encontradas[idx];
            escrito = true;
        }
    }
    return escrito;
}

uint8_t frigo_sondas_evento(uint8_t flags_antes, uint8_t flags_ahora)
{
    return (uint8_t)(flags_ahora & ~flags_antes);
}
