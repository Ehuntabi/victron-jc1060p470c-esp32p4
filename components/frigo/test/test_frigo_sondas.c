/* Test host de la logica de sondas del frigo.
 * Compilar: gcc -I. frigo_sondas.c test/test_frigo_sondas.c -o /tmp/tfs && /tmp/tfs */
#include "frigo_sondas.h"
#include <stdio.h>
#include <assert.h>

#define A 0x2800000011111111ULL   /* aletas */
#define C 0x2800000022222222ULL   /* congelador */
#define E 0x2800000033333333ULL   /* exterior */
#define X 0x2800000099999999ULL   /* una desconocida */

int main(void)
{
    /* 1) Todo en su sitio: sin cambios y cada rol en su indice. */
    {
        uint64_t hay[] = { A, C, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == 0);
        assert(r.asignacion[0] == 0 && r.asignacion[1] == 1 && r.asignacion[2] == 2);
        assert(r.n_sin_rol == 0);
    }
    /* 2) El bus las devuelve en otro orden: los roles NO se bailan. */
    {
        uint64_t hay[] = { E, A, C };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == 0);
        assert(r.asignacion[0] == 1);   /* aletas esta ahora en la posicion 1 */
        assert(r.asignacion[1] == 2);
        assert(r.asignacion[2] == 0);
    }
    /* 3) Falta el congelador: se marca ausente y se avisa. Los otros no se mueven. */
    {
        uint64_t hay[] = { A, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 2, roles);
        assert(r.flags == FRIGO_SCAN_FALTA_UNA);
        assert(r.asignacion[0] == 0);
        assert(r.asignacion[1] == FRIGO_SONDA_AUSENTE);
        assert(r.asignacion[2] == 1);
    }
    /* 4) Aparece una desconocida: se avisa, pero no se le da rol sola. */
    {
        uint64_t hay[] = { A, C, X };
        uint64_t roles[] = { A, C, 0 };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == FRIGO_SCAN_HAY_NUEVA);
        assert(r.n_sin_rol == 1);
        assert(r.asignacion[2] == FRIGO_SONDA_AUSENTE);
    }
    /* 5) Sustitucion: se va la del congelador y llega otra nueva. Las dos cosas. */
    {
        uint64_t hay[] = { A, X, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == (FRIGO_SCAN_HAY_NUEVA | FRIGO_SCAN_FALTA_UNA));
        assert(r.asignacion[1] == FRIGO_SONDA_AUSENTE);
        assert(r.n_sin_rol == 1);
    }
    /* 6) Bus vacio: todos ausentes, se avisa de que faltan. */
    {
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(NULL, 0, roles);
        assert(r.flags == FRIGO_SCAN_FALTA_UNA);
        for (int i = 0; i < FRIGO_SONDAS_MAX; i++)
            assert(r.asignacion[i] == FRIGO_SONDA_AUSENTE);
    }
    /* 7) Sin roles guardados y sin sondas: no hay nada que decidir, no se avisa. */
    {
        uint64_t roles[] = { 0, 0, 0 };
        frigo_scan_result_t r = frigo_sondas_resolver(NULL, 0, roles);
        assert(r.flags == 0);
    }
    /* 8) Migracion: la asignacion por posicion de toda la vida se ancla a las series. */
    {
        uint64_t hay[] = { A, C, E };
        uint8_t asig[] = { 2, 0, 1 };        /* aletas=idx2, congelador=idx0, exterior=idx1 */
        uint64_t roles[] = { 0, 0, 0 };
        assert(frigo_sondas_migrar(hay, 3, asig, roles) == true);
        assert(roles[0] == E && roles[1] == A && roles[2] == C);
    }
    /* 9) Migracion que NO debe repetirse: si ya hay direcciones, no toca nada. */
    {
        uint64_t hay[] = { A, C, E };
        uint8_t asig[] = { 0, 1, 2 };
        uint64_t roles[] = { E, 0, 0 };
        assert(frigo_sondas_migrar(hay, 3, asig, roles) == false);
        assert(roles[0] == E);
    }
    /* 10) El aviso salta al aparecer el problema, no mientras dura. */
    {
        assert(frigo_sondas_evento(0, FRIGO_SCAN_FALTA_UNA) == FRIGO_SCAN_FALTA_UNA);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA, FRIGO_SCAN_FALTA_UNA) == 0);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA, 0) == 0);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA,
                                   FRIGO_SCAN_FALTA_UNA | FRIGO_SCAN_HAY_NUEVA)
               == FRIGO_SCAN_HAY_NUEVA);
    }
    printf("test_frigo_sondas: OK\n");
    return 0;
}
