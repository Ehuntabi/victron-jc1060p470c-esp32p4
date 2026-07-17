#include "frigo_solar.h"

/* Precondiciones de seguridad: si cualquiera falla, el rele va a OFF de
 * inmediato (sin esperar al bloque de MIN_ON). */
static bool preconds_ok(const frigo_solar_in_t *in)
{
    if (!in->enabled) return false;
    if (!in->fresh)   return false;
    if (in->shore)    return false;                       /* hay 230V -> no 12V */
    if (in->soc_deci < (uint16_t)in->soc_off_pct * 10)    /* suelo de SoC */
        return false;
    return true;
}

bool frigo_solar_eval(const frigo_solar_in_t *in, frigo_solar_sm_t *sm)
{
    if (!preconds_ok(in)) {
        if (sm->active) { sm->active = false; sm->active_since_ms = in->now_ms; }
        sm->arming = false;
        return false;
    }

    /* Condiciones de activacion (ademas de precondiciones). */
    bool act_cond = (in->soc_deci >= (uint16_t)in->soc_on_pct * 10) &&
                    (in->pv_w >= FRIGO_SOLAR_PV_MIN_W);

    if (!sm->active) {
        if (act_cond) {
            if (!sm->arming) { sm->arming = true; sm->arming_since_ms = in->now_ms; }
            if ((uint32_t)(in->now_ms - sm->arming_since_ms) >= FRIGO_SOLAR_ACT_DEBOUNCE_MS) {
                sm->active = true;
                sm->active_since_ms = in->now_ms;
                sm->arming = false;
            }
        } else {
            sm->arming = false;
        }
    } else {
        /* ON: aguanta MIN_ON salvo seguridad (ya cubierta arriba). Corte por
         * perdida de sol solo tras cumplir el bloque minimo. */
        bool min_on_done = (uint32_t)(in->now_ms - sm->active_since_ms) >= FRIGO_SOLAR_MIN_ON_MS;
        if (min_on_done && in->pv_w < FRIGO_SOLAR_PV_MIN_W) {
            sm->active = false;
            sm->active_since_ms = in->now_ms;
        }
    }
    return sm->active;
}
