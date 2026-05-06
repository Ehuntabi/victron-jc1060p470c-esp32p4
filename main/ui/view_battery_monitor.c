#include "view_battery_monitor.h"
#include <stdlib.h>
#include <string.h>
#include "ui_format.h"
#include "ui.h"

static void battery_view_root_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_battery_history_screen(ui);
}


typedef enum {
    BATTERY_PRIMARY_VOLTAGE = 0,
    BATTERY_PRIMARY_CURRENT,
    BATTERY_PRIMARY_SOC,
    BATTERY_PRIMARY_COUNT
} battery_primary_label_t;

typedef enum {
    BATTERY_SECONDARY_TTG = 0,
    BATTERY_SECONDARY_CONSUMED,
    BATTERY_SECONDARY_AUX,
    BATTERY_SECONDARY_COUNT
} battery_secondary_label_t;

typedef struct {
    lv_obj_t *header;
    lv_obj_t *value;
} ui_label_pair_t;

typedef struct {
    ui_device_view_t base;
    lv_obj_t *row_primary;
    lv_obj_t *row_secondary;
    ui_label_pair_t primary[BATTERY_PRIMARY_COUNT];
    ui_label_pair_t secondary[BATTERY_SECONDARY_COUNT];
} ui_battery_view_t;

static void battery_view_update(ui_device_view_t *view, const victron_data_t *data);
static void battery_view_show(ui_device_view_t *view);
static void battery_view_hide(ui_device_view_t *view);
static void battery_view_destroy(ui_device_view_t *view);
static void format_primary_voltage(lv_obj_t *label, const victron_data_t *data);
static void format_primary_current(lv_obj_t *label, const victron_data_t *data);
static void format_primary_soc(lv_obj_t *label, const victron_data_t *data);
static void format_secondary_ttg(lv_obj_t *label, const victron_data_t *data);
static void format_secondary_consumed(lv_obj_t *label, const victron_data_t *data);
static void format_secondary_aux(lv_obj_t *label, const victron_data_t *data);

static const ui_label_descriptor_t battery_primary_descriptors[BATTERY_PRIMARY_COUNT] = {
    { "battery_voltage", "Voltaje",   format_primary_voltage },
    { "battery_current", "Corriente", format_primary_current },
    { "battery_soc",     "SOC",       format_primary_soc },
};

static const ui_label_descriptor_t battery_secondary_descriptors[BATTERY_SECONDARY_COUNT] = {
    { "ttg",      "Autonomía",  format_secondary_ttg },
    { "consumed", "Consumido",  format_secondary_consumed },
    { "aux",      "Aux",        format_secondary_aux },
};

static ui_label_pair_t create_label_box(ui_state_t *ui, lv_obj_t *parent,
                                        const ui_label_descriptor_t *desc)
{
    ui_label_pair_t pair = {0};
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, lv_pct(30), 100);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_outline_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    pair.header = lv_label_create(box);
    lv_label_set_text(pair.header, desc->title ? desc->title : "");
    lv_obj_add_style(pair.header, &ui->styles.medium, 0);
    lv_obj_align(pair.header, LV_ALIGN_TOP_MID, 0, 15);

    pair.value = lv_label_create(box);
    lv_label_set_text(pair.value, "--");
    lv_obj_add_style(pair.value, &ui->styles.value, 0);
    lv_obj_align(pair.value, LV_ALIGN_BOTTOM_MID, 0, -15);

    return pair;
}

ui_device_view_t *ui_battery_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    if (ui == NULL || parent == NULL) return NULL;

    ui_battery_view_t *view = calloc(1, sizeof(*view));
    if (view == NULL) return NULL;

    view->base.ui   = ui;
    view->base.root = lv_obj_create(parent);
    lv_obj_set_size(view->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->base.root, 0, 0);
    lv_obj_set_style_outline_width(view->base.root, 0, 0);
    lv_obj_set_style_pad_all(view->base.root, 0, 0);
    lv_obj_clear_flag(view->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_HIDDEN);

    /* Fila primaria — 3 valores grandes */
    view->row_primary = lv_obj_create(view->base.root);
    lv_obj_set_size(view->row_primary, lv_pct(100), 120);
    lv_obj_set_flex_flow(view->row_primary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->row_primary,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->row_primary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(view->row_primary, 0, 0);
    lv_obj_set_style_outline_width(view->row_primary, 0, 0);
    lv_obj_set_style_bg_opa(view->row_primary, LV_OPA_TRANSP, 0);
    lv_obj_align(view->row_primary, LV_ALIGN_TOP_MID, 0, 10);

    for (size_t i = 0; i < BATTERY_PRIMARY_COUNT; ++i) {
        view->primary[i] = create_label_box(ui, view->row_primary,
                                            &battery_primary_descriptors[i]);
    }

    /* Fila secundaria — TTG, Consumido, Aux */
    view->row_secondary = lv_obj_create(view->base.root);
    lv_obj_set_size(view->row_secondary, lv_pct(100), 120);
    lv_obj_set_flex_flow(view->row_secondary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->row_secondary,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->row_secondary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(view->row_secondary, 0, 0);
    lv_obj_set_style_outline_width(view->row_secondary, 0, 0);
    lv_obj_set_style_bg_opa(view->row_secondary, LV_OPA_TRANSP, 0);
    lv_obj_align(view->row_secondary, LV_ALIGN_TOP_MID, 0, 140);

    for (size_t i = 0; i < BATTERY_SECONDARY_COUNT; ++i) {
        view->secondary[i] = create_label_box(ui, view->row_secondary,
                                              &battery_secondary_descriptors[i]);
    }

    /* Alarma */
    if (ui->lbl_error) {
        lv_label_set_text(ui->lbl_error, "");
    }

    view->base.update  = battery_view_update;
    view->base.show    = battery_view_show;
    view->base.hide    = battery_view_hide;
    view->base.destroy = battery_view_destroy;

    /* Tap sobre la vista BM abre el historico de corriente */
    lv_obj_add_flag(view->base.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(view->base.root, battery_view_root_click_cb,
                        LV_EVENT_CLICKED, ui);

    return &view->base;
}

static ui_battery_view_t *battery_view_from_base(ui_device_view_t *base)
{
    return (ui_battery_view_t *)base;
}

static void battery_view_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_battery_view_t *bat = battery_view_from_base(view);
    if (bat == NULL || data == NULL ||
        data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;

    for (size_t i = 0; i < BATTERY_PRIMARY_COUNT; ++i) {
        if (bat->primary[i].value)
            battery_primary_descriptors[i].formatter(bat->primary[i].value, data);
    }
    for (size_t i = 0; i < BATTERY_SECONDARY_COUNT; ++i) {
        if (bat->secondary[i].value)
            battery_secondary_descriptors[i].formatter(bat->secondary[i].value, data);
    }
}

static void battery_view_show(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void battery_view_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void battery_view_destroy(ui_device_view_t *view)
{
    if (view == NULL) return;
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}

/* ── Formatters primarios ────────────────────────────────────── */
static void format_primary_voltage(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;
    const victron_record_battery_monitor_t *b = &data->record.battery;
    ui_label_set_unsigned_fixed(label, (unsigned)b->battery_voltage_centi, 100, 2, " V");
}

static void format_primary_current(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;
    const victron_record_battery_monitor_t *b = &data->record.battery;
    int current_cent = ui_round_div_signed((int)b->battery_current_milli, 10);
    lv_obj_set_style_text_color(label,
        current_cent >= 0 ? lv_color_hex(0x00C851) : lv_color_hex(0xFF9800), 0);
    ui_label_set_signed_fixed(label, current_cent, 100, 2, " A");
}

static void format_primary_soc(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;
    const victron_record_battery_monitor_t *b = &data->record.battery;
    uint16_t soc = b->soc_deci_percent;
    /* Color según SOC */
    lv_color_t col;
    if      (soc >= 700) col = lv_color_hex(0x00C851); /* verde  ≥70% */
    else if (soc >= 300) col = lv_color_hex(0xFF9800); /* naranja 30-70% */
    else                 col = lv_color_hex(0xFF4444); /* rojo   <30% */
    lv_obj_set_style_text_color(label, col, 0);
    ui_label_set_unsigned_fixed(label, (unsigned)soc, 10, 1, " %");
}

/* ── Formatters secundarios ──────────────────────────────────── */
static void format_secondary_ttg(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;
    const victron_record_battery_monitor_t *b = &data->record.battery;
    uint16_t ttg = b->time_to_go_minutes;
    /* 0xFFFF = no disponible (cargando o sin datos) */
    if (ttg == 0xFFFF) {
        lv_label_set_text(label, "--");
    } else if (ttg == 0) {
        lv_label_set_text(label, "0m");
    } else {
        lv_label_set_text_fmt(label, "%uh %02um",
                              (unsigned)(ttg / 60), (unsigned)(ttg % 60));
    }
}

static void format_secondary_consumed(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;
    const victron_record_battery_monitor_t *b = &data->record.battery;
    ui_label_set_signed_fixed(label, (int)b->consumed_ah_deci, 10, 1, " Ah");
}

static void format_secondary_aux(lv_obj_t *label, const victron_data_t *data)
{
    if (!label || !data || data->type != VICTRON_BLE_RECORD_BATTERY_MONITOR) return;

    lv_obj_t *box    = lv_obj_get_parent(label);
    lv_obj_t *header = NULL;
    uint32_t  n      = lv_obj_get_child_cnt(box);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(box, i);
        if (child != label) { header = child; break; }
    }

    const victron_record_battery_monitor_t *b = &data->record.battery;

    if ((b->aux_input & 0x03u) == 0x03u) {
        /* Potencia instantánea */
        if (header) lv_label_set_text(header, "Amps");
       int32_t current_tenths = ui_round_div_signed(b->battery_current_milli, 100);
        lv_obj_set_style_text_color(label,
            current_tenths >= 0 ? lv_color_hex(0x00C851) : lv_color_hex(0xFF9800), 0);
        ui_label_set_signed_fixed(label, current_tenths, 10, 1, " A");
    } else {
        if (header) lv_label_set_text(header, "Aux");
        char aux_buf[32];
        ui_format_aux_value(b->aux_input, b->aux_value, aux_buf, sizeof(aux_buf));
        lv_label_set_text(label, aux_buf);
    }
}
