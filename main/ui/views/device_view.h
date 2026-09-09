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
    /* esp_timer_get_time() del ultimo dato REAL recibido (no de un
     * redibujado desde cache). 0 = nunca. Lo usa ui.c para atenuar la
     * tarjeta si el dispositivo deja de emitir; ver active_view_freshness_cb. */
    int64_t last_update_us;
    /* Tipo de record que pinta esta vista (lo fija ui_view_registry_ensure
     * al crearla). ui_on_panel_data lo compara con el tipo de CADA record
     * que llega antes de tocar last_update_us: si no coincide, update() ya
     * hace un return sin pintar nada (cada vista filtra por tipo), pero el
     * sello se seguia refrescando igual -- asi que si el dispositivo activo
     * se callaba y otro Victron distinto seguia emitiendo, la tarjeta nunca
     * llegaba a atenuarse. Detectado por el usuario el 09-sep-2026. */
    victron_record_type_t device_type;
};

typedef ui_device_view_t *(*ui_device_view_create_fn)(struct ui_state *ui, lv_obj_t *parent);

#endif /* UI_DEVICE_VIEW_H */
