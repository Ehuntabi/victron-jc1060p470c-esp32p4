#include "view_solar_charger.h"

#include <stdlib.h>
#include <string.h>

#include "ui_format.h"

LV_FONT_DECLARE(font_awesome_solar_panel_40);
LV_FONT_DECLARE(font_awesome_bolt_40);

typedef enum {
    SOLAR_LABEL_BATT_V = 0,
    SOLAR_LABEL_BATT_A,
    SOLAR_LABEL_LOAD_A,
    SOLAR_LABEL_COUNT
} solar_value_label_t;

typedef enum {
    SOLAR_BOTTOM_SOLAR_POWER = 0,
    SOLAR_BOTTOM_YIELD,
    SOLAR_BOTTOM_LOAD_POWER,
    SOLAR_BOTTOM_COUNT
} solar_bottom_label_t;

typedef struct {
    ui_device_view_t base;
    lv_obj_t *row_primary;
    lv_obj_t *value_labels[SOLAR_LABEL_COUNT];
    lv_obj_t *state_label;
    lv_obj_t *icon_solar;
    lv_obj_t *icon_bolt;
    lv_obj_t *bottom_labels[SOLAR_BOTTOM_COUNT];
} ui_solar_view_t;

static void solar_view_update(ui_device_view_t *view, const victron_data_t *data);
static void solar_view_show(ui_device_view_t *view);
static void solar_view_hide(ui_device_view_t *view);
static void solar_view_destroy(ui_device_view_t *view);

static void format_battery_voltage(lv_obj_t *label, const victron_data_t *data);
static void format_battery_current(lv_obj_t *label, const victron_data_t *data);
static void format_load_current(lv_obj_t *label, const victron_data_t *data);
static const char *solar_error_string(uint8_t code);
static const char *solar_state_string(uint8_t state);

static const ui_label_descriptor_t solar_primary_descriptors[SOLAR_LABEL_COUNT] = {
    { "battery_voltage", "Voltaje",   format_battery_voltage },
    { "battery_current", "Corriente", format_battery_current },
    { "load_current",    "Carga",     format_load_current },
};

static lv_obj_t *create_label_box(ui_state_t *ui, lv_obj_t *parent,
                                  const ui_label_descriptor_t *desc)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, lv_pct(30), 100);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_label_create(box);
    lv_label_set_text(header, desc->title ? desc->title : "");
    lv_obj_add_style(header, &ui->styles.medium, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *value = lv_label_create(box);
    lv_label_set_text(value, "--");
    lv_obj_add_style(value, &ui->styles.value, 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -5);

    return value;
}

typedef struct {
    unsigned input_power_w;
    unsigned long yield_wh;
    unsigned long load_w;
} solar_metrics_t;

static void compute_metrics(const victron_record_solar_charger_t *s, solar_metrics_t *out)
{
    if (!out || !s) return;
    out->input_power_w = s->pv_power_w;
    out->yield_wh      = (unsigned long)s->yield_today_centikwh * 10UL;

    /* Potencia carga con mayor precisión */
    long product = (long)s->load_current_deci * (long)s->battery_voltage_centi;
    out->load_w = (unsigned long)(product / 1000L);
}

ui_device_view_t *ui_solar_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (!ui || !parent) return NULL;

    ui_solar_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    view->base.ui   = ui;
    view->base.root = lv_obj_create(parent);
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_outline_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 0, 0);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    view->row_primary = lv_obj_create(view->base.root);
    lv_obj_set_size(view->row_primary, lv_pct(100), 100);
    lv_obj_set_flex_flow(view->row_primary, LV_STYLE_PAD_ROW);
    lv_obj_set_flex_align(view->row_primary,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->row_primary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(view->row_primary, 0, 0);
    lv_obj_set_style_outline_width(view->row_primary, 0, 0);

    for (size_t i = 0; i < SOLAR_LABEL_COUNT; ++i) {
        view->value_labels[i] = create_label_box(ui, view->row_primary,
                                                 &solar_primary_descriptors[i]);
    }

    view->state_label = lv_label_create(view->base.root);
    lv_obj_add_style(view->state_label, &ui->styles.big, 0);
    lv_label_set_text(view->state_label, "Estado");
    lv_obj_align(view->state_label, LV_ALIGN_CENTER, 0, 50);

    view->icon_solar = lv_label_create(view->base.root);
    lv_obj_set_style_text_font(view->icon_solar, &font_awesome_solar_panel_40, 0);
    lv_obj_set_style_transform_zoom(view->icon_solar, 512, 0);
    lv_label_set_text(view->icon_solar, "\xEF\x96\xBA");
    lv_obj_align(view->icon_solar, LV_ALIGN_BOTTOM_LEFT, 25, -55);

    view->icon_bolt = lv_label_create(view->base.root);
    lv_obj_set_style_text_font(view->icon_bolt, &font_awesome_bolt_40, 0);
    lv_obj_set_style_transform_zoom(view->icon_bolt, 512, 0);
    lv_label_set_text(view->icon_bolt, "\xEF\x83\xA7");
    lv_obj_align(view->icon_bolt, LV_ALIGN_BOTTOM_RIGHT, -28, -55);

    /* Labels inferiores: PV power | Yield | Load power */
    view->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER] = lv_label_create(view->base.root);
    lv_obj_add_style(view->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER], &ui->styles.small, 0);
    lv_label_set_text(view->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER], "");
    lv_obj_align(view->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER], LV_ALIGN_BOTTOM_LEFT, 20, -8);

    view->bottom_labels[SOLAR_BOTTOM_YIELD] = lv_label_create(view->base.root);
    lv_obj_add_style(view->bottom_labels[SOLAR_BOTTOM_YIELD], &ui->styles.small, 0);
    lv_label_set_text(view->bottom_labels[SOLAR_BOTTOM_YIELD], "");
    lv_obj_align(view->bottom_labels[SOLAR_BOTTOM_YIELD], LV_ALIGN_BOTTOM_MID, 0, -8);

    view->bottom_labels[SOLAR_BOTTOM_LOAD_POWER] = lv_label_create(view->base.root);
    lv_obj_add_style(view->bottom_labels[SOLAR_BOTTOM_LOAD_POWER], &ui->styles.small, 0);
    lv_label_set_text(view->bottom_labels[SOLAR_BOTTOM_LOAD_POWER], "");
    lv_obj_align(view->bottom_labels[SOLAR_BOTTOM_LOAD_POWER], LV_ALIGN_BOTTOM_RIGHT, -20, -8);

    view->base.update  = solar_view_update;
    view->base.show    = solar_view_show;
    view->base.hide    = solar_view_hide;
    view->base.destroy = solar_view_destroy;

    return &view->base;
}

static ui_solar_view_t *solar_view_from_base(ui_device_view_t *base)
{
    return (ui_solar_view_t *)base;
}

static void solar_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_solar_view_t *solar = solar_view_from_base(view);
    if (!solar || !data || data->type != VICTRON_BLE_RECORD_SOLAR_CHARGER) return;

    for (size_t i = 0; i < SOLAR_LABEL_COUNT; ++i) {
        if (solar->value_labels[i])
            solar_primary_descriptors[i].formatter(solar->value_labels[i], data);
    }

    const victron_record_solar_charger_t *s = &data->record.solar;
    solar_metrics_t metrics = {0};
    compute_metrics(s, &metrics);

    if (solar->state_label)
        lv_label_set_text(solar->state_label, solar_state_string(s->device_state));

    /* PV power — color verde si produce */
    if (solar->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER]) {
        lv_obj_set_style_text_color(solar->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER],
            metrics.input_power_w > 0 ? lv_color_hex(0x00C851) : lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text_fmt(solar->bottom_labels[SOLAR_BOTTOM_SOLAR_POWER],
                              "PV: %u W", metrics.input_power_w);
    }

    /* Yield — en Wh o kWh según el valor */
    if (solar->bottom_labels[SOLAR_BOTTOM_YIELD]) {
        if (metrics.yield_wh >= 1000) {
            lv_label_set_text_fmt(solar->bottom_labels[SOLAR_BOTTOM_YIELD],
                                  "Hoy: %lu.%01lu kWh",
                                  metrics.yield_wh / 1000,
                                  (metrics.yield_wh % 1000) / 100);
        } else {
            lv_label_set_text_fmt(solar->bottom_labels[SOLAR_BOTTOM_YIELD],
                                  "Hoy: %lu Wh", metrics.yield_wh);
        }
    }

    /* Load power */
    if (solar->bottom_labels[SOLAR_BOTTOM_LOAD_POWER]) {
        lv_label_set_text_fmt(solar->bottom_labels[SOLAR_BOTTOM_LOAD_POWER],
                              "Carga: %lu W", metrics.load_w);
    }

    if (view->ui && view->ui->lbl_error)
        lv_label_set_text(view->ui->lbl_error, solar_error_string(s->charger_error));
}

static void solar_view_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void solar_view_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void solar_view_destroy(ui_device_view_t *view)
{
    if (!view) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}

/* ── Formatters ──────────────────────────────────────────────── */
static void format_battery_voltage(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_SOLAR_CHARGER) return;
    const victron_record_solar_charger_t *s = &data->record.solar;
    ui_label_set_unsigned_fixed(label, (unsigned)s->battery_voltage_centi, 100, 2, " V");
}

static void format_battery_current(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_SOLAR_CHARGER) return;
    const victron_record_solar_charger_t *s = &data->record.solar;
    /* Color verde si está cargando */
    lv_obj_set_style_text_color(label,
        s->battery_current_deci > 0 ? lv_color_hex(0x00C851) : lv_color_hex(0xAAAAAA), 0);
    ui_label_set_signed_fixed(label, s->battery_current_deci, 10, 1, " A");
}

static void format_load_current(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_SOLAR_CHARGER) return;
    const victron_record_solar_charger_t *s = &data->record.solar;
    ui_label_set_signed_fixed(label, s->load_current_deci, 10, 1, " A");
}

static const char *solar_error_string(uint8_t e)
{
    switch (e) {
    case 0:   return "";
    case 1:   return "Temp batería alta";
    case 2:   return "Voltaje batería alto";
    case 3:
    case 4:   return "Fallo sensor temp remoto";
    case 5:   return "Sensor temp remoto perdido";
    case 6:
    case 7:   return "Fallo sensor voltaje remoto";
    case 8:   return "Sensor voltaje remoto perdido";
    case 11:  return "Ripple de voltaje alto";
    case 14:  return "Batería demasiado fría";
    case 17:  return "Controlador sobrecalentado";
    case 18:  return "Sobrecorriente controlador";
    case 20:  return "Tiempo bulk máximo excedido";
    case 21:  return "Sensor de corriente fuera de rango";
    case 24:  return "Fallo ventilador";
    case 26:  return "Terminal de potencia sobrecalentado";
    case 27:  return "Cortocircuito lado batería";
    case 28:  return "Problema hardware etapa potencia";
    case 33:  return "Sobrevoltaje PV";
    case 34:  return "Sobrecorriente PV";
    case 35:  return "Sobrepotencia PV";
    case 38:
    case 39:  return "Entrada PV cortocircuitada";
    default:  return "Error desconocido";
    }
}

static const char *solar_state_string(uint8_t s)
{
    switch (s) {
    case 0:  return "Apagado";
    case 1:  return "Bajo consumo";
    case 2:  return "Fallo";
    case 3:  return "Bulk";
    case 4:  return "Absorción";
    case 5:  return "Float";
    case 6:  return "Almacenamiento";
    case 7:  return "Ecualización (Manual)";
    case 8:  return "Ecualización (Auto)";
    case 9:  return "Inversor";
    case 10: return "Fuente de alimentación";
    case 11: return "Iniciando";
    default: return "Desconocido";
    }
}
