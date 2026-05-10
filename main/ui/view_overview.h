#ifndef UI_VIEW_OVERVIEW_H
#define UI_VIEW_OVERVIEW_H

#include "ui/device_view.h"
#include "ui/ui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vista Overview estilo Venus OS: diagrama de flujo de energía
 * (Solar → Bat ← Loads) con SOC grande en card inferior. */
ui_device_view_t *ui_overview_view_create(ui_state_t *ui, lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIEW_OVERVIEW_H */
