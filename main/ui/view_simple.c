#include "view_simple.h"
#include "ui_card.h"
#include "fonts/fonts_es.h"

#include <stdlib.h>

typedef struct {
    ui_device_view_t base;
    const ui_simple_view_config_t *config;
    lv_obj_t *card;
    lv_obj_t **values;  /* labels que reciben el texto via formatter */
} ui_simple_view_t;

static ui_simple_view_t *simple_view_from_base(ui_device_view_t *base)
{
    return (ui_simple_view_t *)base;
}

static const char *simple_title_for_type(victron_record_type_t t)
{
    switch (t) {
        case VICTRON_BLE_RECORD_INVERTER:             return "Inverter";
        case VICTRON_BLE_RECORD_DCDC_CONVERTER:       return "DC/DC Converter";
        case VICTRON_BLE_RECORD_SMART_LITHIUM:        return "SmartLithium";
        case VICTRON_BLE_RECORD_INVERTER_RS:          return "Inverter RS";
        case VICTRON_BLE_RECORD_AC_CHARGER:           return "AC Charger";
        case VICTRON_BLE_RECORD_SMART_BATTERY_PROTECT:return "Smart Battery Protect";
        case VICTRON_BLE_RECORD_LYNX_SMART_BMS:       return "Lynx Smart BMS";
        case VICTRON_BLE_RECORD_MULTI_RS:             return "Multi RS";
        case VICTRON_BLE_RECORD_VE_BUS:               return "VE.Bus";
        case VICTRON_BLE_RECORD_DC_ENERGY_METER:      return "DC Energy Meter";
        case VICTRON_BLE_RECORD_ORION_XS:             return "Orion XS";
        default:                                       return "Dispositivo";
    }
}

static lv_color_t simple_color_for_type(victron_record_type_t t)
{
    switch (t) {
        case VICTRON_BLE_RECORD_INVERTER:
        case VICTRON_BLE_RECORD_INVERTER_RS:
        case VICTRON_BLE_RECORD_VE_BUS:
        case VICTRON_BLE_RECORD_MULTI_RS:
            return UI_COLOR_YELLOW;
        case VICTRON_BLE_RECORD_DCDC_CONVERTER:
        case VICTRON_BLE_RECORD_ORION_XS:
            return UI_COLOR_ORANGE;
        case VICTRON_BLE_RECORD_AC_CHARGER:
            return UI_COLOR_GREEN;
        case VICTRON_BLE_RECORD_SMART_LITHIUM:
        case VICTRON_BLE_RECORD_LYNX_SMART_BMS:
            return UI_COLOR_CYAN;
        case VICTRON_BLE_RECORD_SMART_BATTERY_PROTECT:
            return UI_COLOR_RED;
        case VICTRON_BLE_RECORD_DC_ENERGY_METER:
            return UI_COLOR_CYAN;
        default:
            return UI_COLOR_CYAN;
    }
}

static lv_obj_t *create_value_row(ui_simple_view_t *view, lv_obj_t *parent,
                                  const ui_simple_label_descriptor_t *desc)
{
    if (!view || !desc || !view->base.ui) return NULL;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 12, 0);

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(title, desc->title ? desc->title : "");

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(value, UI_COLOR_TEXT, 0);
    lv_label_set_text(value, "--");
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    return value;
}

static void simple_view_update(ui_device_view_t *base, const victron_data_t *data)
{
    ui_simple_view_t *view = simple_view_from_base(base);
    if (!view || !data || !view->config) return;
    if (data->type != view->config->type) return;

    if (base->ui && base->ui->lbl_error) {
        lv_label_set_text(base->ui->lbl_error, "");
    }

    for (size_t i = 0; i < view->config->label_count; ++i) {
        if (!view->values[i]) continue;
        const ui_simple_label_descriptor_t *desc = &view->config->labels[i];
        if (desc->formatter) {
            desc->formatter(view->values[i], data);
        } else {
            lv_label_set_text(view->values[i], "");
        }
    }
}

static void simple_view_show(ui_device_view_t *base)
{
    if (base && base->root) lv_obj_clear_flag(base->root, LV_OBJ_FLAG_HIDDEN);
}

static void simple_view_hide(ui_device_view_t *base)
{
    if (base && base->root) lv_obj_add_flag(base->root, LV_OBJ_FLAG_HIDDEN);
}

static void simple_view_destroy(ui_device_view_t *base)
{
    ui_simple_view_t *view = simple_view_from_base(base);
    if (!view) return;
    if (view->base.root) { lv_obj_del(view->base.root); view->base.root = NULL; }
    free(view->values);
    free(view);
}

ui_device_view_t *ui_simple_view_create(ui_state_t *ui,
                                        lv_obj_t *parent,
                                        const ui_simple_view_config_t *config)
{
    if (!ui || !parent || !config || !config->labels || config->label_count == 0) {
        return NULL;
    }

    ui_simple_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    view->values = calloc(config->label_count, sizeof(lv_obj_t *));
    if (!view->values) { free(view); return NULL; }

    view->config  = config;
    view->base.ui = ui;
    view->base.root = lv_obj_create(parent);
    if (!view->base.root) {
        free(view->values);
        free(view);
        return NULL;
    }

    /* Root: contenedor transparente con padding y un único card dentro */
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 12, 0);
    lv_obj_set_style_pad_gap(view->base.root, 12, 0);
    lv_obj_set_layout(view->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    /* Card que envuelve todas las filas */
    lv_color_t accent = simple_color_for_type(config->type);
    view->card = ui_card_create(view->base.root, accent);
    ui_card_set_title(view->card, LV_SYMBOL_LIST,
                      simple_title_for_type(config->type), accent);

    /* Permitimos scroll vertical en la card si hay muchas filas */
    lv_obj_set_scroll_dir(view->card, LV_DIR_VER);

    for (size_t i = 0; i < config->label_count; ++i) {
        view->values[i] = create_value_row(view, view->card, &config->labels[i]);
    }

    view->base.update  = simple_view_update;
    view->base.show    = simple_view_show;
    view->base.hide    = simple_view_hide;
    view->base.destroy = simple_view_destroy;

    return &view->base;
}
