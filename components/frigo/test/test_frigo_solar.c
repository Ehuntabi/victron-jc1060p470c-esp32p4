/* Test host de la maquina de estados del modo excedente solar.
 * Compilar: gcc -I. frigo_solar.c test/test_frigo_solar.c -o /tmp/tfs && /tmp/tfs */
#include "frigo_solar.h"
#include <stdio.h>
#include <assert.h>

/* Entradas por defecto: modo ON, todo fresco, sin red, SoC 96%, PV 120W,
 * umbrales 95/80. now_ms lo fija cada caso. */
static frigo_solar_in_t base(uint32_t now_ms) {
    frigo_solar_in_t in = {
        .enabled = true, .soc_deci = 960, .pv_w = 120, .shore = false,
        .fresh = true, .soc_on_pct = 95, .soc_off_pct = 80, .now_ms = now_ms,
    };
    return in;
}

int main(void) {
    /* 1) Modo OFF: nunca activa aunque las condiciones sean buenas. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(0); in.enabled = false;
        assert(frigo_solar_eval(&in, &sm) == false);
        in.now_ms = 200000; assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 2) Activacion tras debounce de 60 s de condiciones sostenidas. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        assert(frigo_solar_eval(&in, &sm) == false);      /* arma */
        in.now_ms = 1000 + 59000; assert(frigo_solar_eval(&in, &sm) == false);
        in.now_ms = 1000 + 60000; assert(frigo_solar_eval(&in, &sm) == true);
    }
    /* 3) Con 230V (shore) nunca activa; y si estaba ON, corta al instante. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000); in.shore = true;
        in.now_ms = 1000 + 120000; assert(frigo_solar_eval(&in, &sm) == false);
        /* forzar ON y luego meter shore */
        sm = (frigo_solar_sm_t){0}; in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);
        in.shore = true; in.now_ms = 61100;
        assert(frigo_solar_eval(&in, &sm) == false);      /* inmediato, sin MIN_ON */
    }
    /* 4) SoC bajo el suelo: corte inmediato. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);
        in.soc_deci = 790; in.now_ms = 61100;             /* 79% < 80% */
        assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 5) Sin datos frescos: corte/off. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000); in.fresh = false;
        in.now_ms = 200000; assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 6) Histeresis + MIN_ON: aguanta nube antes de 30 min; corta despues. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);       /* ON en t=61000 */
        in.pv_w = 40; in.now_ms = 61000 + 100000;         /* nube, <MIN_ON */
        assert(frigo_solar_eval(&in, &sm) == true);       /* aguanta */
        in.now_ms = 61000 + FRIGO_SOLAR_MIN_ON_MS + 1;    /* pasado MIN_ON, sin sol */
        assert(frigo_solar_eval(&in, &sm) == false);      /* corta */
    }
    printf("ALL PASS\n");
    return 0;
}
