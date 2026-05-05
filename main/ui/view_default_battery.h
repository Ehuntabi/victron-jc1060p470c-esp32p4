#ifndef UI_VIEW_DEFAULT_BATTERY_H
#define UI_VIEW_DEFAULT_BATTERY_H

#include "device_view.h"
#include "ui_state.h"

/**
 * Create a default battery view that displays SOC with a circular arc
 * This view shows battery state of charge as a percentage with a 15px wide arc
 * and handles multiple device types that provide SOC data.
 * 
 * @param ui UI state structure
 * @param parent Parent LVGL object to attach the view to
 * @return Pointer to the created device view, or NULL on failure
 */
ui_device_view_t *ui_default_battery_view_create(ui_state_t *ui, lv_obj_t *parent);

#endif /* UI_VIEW_DEFAULT_BATTERY_H */