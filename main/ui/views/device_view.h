#ifndef UI_DEVICE_VIEW_H
#define UI_DEVICE_VIEW_H

#include <lvgl.h>
#include "victron_ble.h"

struct ui_state;

typedef struct ui_device_view ui_device_view_t;

typedef void (*ui_device_view_update_fn)(ui_device_view_t *view, const victron_data_t *data);
typedef void (*ui_device_view_lifecycle_fn)(ui_device_view_t *view);

typedef void (*ui_label_formatter_t)(lv_obj_t *label, const victron_data_t *data);

typedef struct {
    const char *id;
    const char *title;
    ui_label_formatter_t formatter;
} ui_label_descriptor_t;

struct ui_device_view {
    struct ui_state *ui;
    lv_obj_t *root;
    ui_device_view_update_fn update;
    ui_device_view_lifecycle_fn show;
    ui_device_view_lifecycle_fn hide;
    ui_device_view_lifecycle_fn destroy;
};

typedef ui_device_view_t *(*ui_device_view_create_fn)(struct ui_state *ui, lv_obj_t *parent);

#endif /* UI_DEVICE_VIEW_H */
