/* Pagina "Victron Keys" de Ajustes: aviso previo, lista de dispositivos BLE y
 * sus claves de cifrado.
 *
 * Sale de settings_panel.c, que habia llegado a 4.150 lineas y era incomodo de
 * tocar. Aqui esta SOLO esta pagina; lo unico que comparte con el resto son las
 * utilidades comunes de settings_common.h. El comportamiento no cambia: es el
 * mismo codigo movido de sitio.
 */
#include "settings_panel.h"
#include "ui.h"
#include "ui_card.h"
#include "ausente_mode.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "alerts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "config_storage.h"
#include "config_server.h"
#include "victron_ble.h"
#include "display.h"
#include "esp_log.h"
#include "datalogger.h"
#include "battery_history.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ui/frigo_panel.h"
#include "ne185/ne185.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "camera.h"   /* camera_sd_bus_lock: coordinar SD con el GDMA de la camara */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "watchdog.h"
#include "config_backup.h"
#include "trip_computer.h"
#include <time.h>
#include "settings_common.h"

/* Declaraciones adelantadas: dentro de este fichero unas se llaman a otras
 * antes de estar definidas. */
void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron);
void victron_config_load(ui_state_t *ui);
void victron_config_refresh(ui_state_t *ui);
void victron_config_create_row(ui_state_t *ui, size_t index);
void victron_config_update_controls(ui_state_t *ui);
void victron_config_persist(ui_state_t *ui);
void victron_config_add_btn_event_cb(lv_event_t *e);
void victron_config_remove_btn_event_cb(lv_event_t *e);
void victron_enabled_checkbox_event_cb(lv_event_t *e);
void victron_field_ta_event_cb(lv_event_t *e);


static void victron_keys_show_warning(ui_state_t *ui);
static void victron_warning_btn_cb(lv_event_t *e);
void victron_keys_clicked_cb(lv_event_t *e);
void victron_keys_clicked_cb(lv_event_t *e)
{
    ui_state_t *u = (ui_state_t *)lv_event_get_user_data(e);
    victron_keys_show_warning(u);
}


static lv_obj_t *s_victron_warning = NULL;

static void victron_keys_show_warning(ui_state_t *ui)
{
    (void)ui;
    if (s_victron_warning) return;
    /* Modal background */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_victron_warning = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  Atencion");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Los cambios en esta seccion pueden afectar al funcionamiento del sistema. Procede con cuidado.");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 220, 60);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, victron_warning_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 220, 60);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, "Continuar");
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, victron_warning_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void victron_warning_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    if (txt && strcmp(txt, "Cancelar") == 0) {
        /* Cerrar el msgbox y volver al menu principal */
        if (s_victron_warning) { lv_obj_del(s_victron_warning); s_victron_warning = NULL; }
        if (s_settings_menu && s_settings_main_page) {
            lv_menu_set_page(s_settings_menu, s_settings_main_page);
        }
    } else {
        if (s_victron_warning) { lv_obj_del(s_victron_warning); s_victron_warning = NULL; }
    }
}

/* Callback en el boton del menu principal para mostrar warning antes */
void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron)
{
    style_settings_scrollbar(page_victron);
    /* Asignar evento al lv_obj para detectar cuando se carga */
    lv_obj_add_event_cb(page_victron, (lv_event_cb_t)NULL, LV_EVENT_SCREEN_LOADED, NULL);
    /* Root container — aprovecha todo el ancho del page */
    lv_obj_t *victron_container = lv_obj_create(page_victron);
    lv_obj_remove_style_all(victron_container);
    lv_obj_set_size(victron_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(victron_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(victron_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(victron_container, 16, 0);
    lv_obj_set_style_pad_gap(victron_container, 16, 0);
    lv_obj_set_scroll_dir(victron_container, LV_DIR_VER);

    /* === Card de controles — border magenta de la seccion Victron Keys === */
    lv_obj_t *card_ctrl = ui_card_create(victron_container, lv_color_hex(0xE91E63));
    lv_obj_t *header = ui_card_set_title(card_ctrl, LV_SYMBOL_LIST,
                                         "Dispositivos Victron",
                                         lv_color_hex(0xE91E63));

    /* Botones +/- a la derecha del header */
    lv_obj_t *controls_row = lv_obj_create(header);
    lv_obj_remove_style_all(controls_row);
    lv_obj_set_size(controls_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(controls_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(controls_row, 10, 0);

    ui->victron_config.add_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.add_btn, 44, 44);
    lv_obj_set_style_bg_color(ui->victron_config.add_btn, UI_COLOR_GREEN, 0);
    lv_obj_set_style_radius(ui->victron_config.add_btn, 8, 0);
    lv_obj_t *lbl_add = lv_label_create(ui->victron_config.add_btn);
    lv_label_set_text(lbl_add, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_add);
    lv_obj_add_event_cb(ui->victron_config.add_btn,
                        victron_config_add_btn_event_cb, LV_EVENT_CLICKED, ui);

    ui->victron_config.remove_btn = lv_btn_create(controls_row);
    lv_obj_set_size(ui->victron_config.remove_btn, 44, 44);
    lv_obj_set_style_bg_color(ui->victron_config.remove_btn, UI_COLOR_RED_DARK, 0);
    lv_obj_set_style_radius(ui->victron_config.remove_btn, 8, 0);
    lv_obj_t *lbl_remove = lv_label_create(ui->victron_config.remove_btn);
    lv_label_set_text(lbl_remove, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(lbl_remove, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lbl_remove);
    lv_obj_add_event_cb(ui->victron_config.remove_btn,
                        victron_config_remove_btn_event_cb, LV_EVENT_CLICKED, ui);

    /* Texto descriptivo dentro del card.
     * \n manual + LONG_CLIP: LONG_WRAP+pct(100) en scroll_cont con hermanos
     * dispara TASK_WDT al construir (memo feedback-lvgl-label-wrap-flex-grow-wdt). */
    lv_obj_t *lbl_header = lv_label_create(card_ctrl);
    lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_header, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_long_mode(lbl_header, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_header, lv_pct(100));
    lv_label_set_text(lbl_header, "Configura hasta 8 dispositivos Victron\ncon su dirección MAC y clave AES.");

    ui->victron_config.container = victron_container;

    /* Lista vertical de cards de dispositivo */
    ui->victron_config.list = lv_obj_create(victron_container);
    lv_obj_remove_style_all(ui->victron_config.list);
    lv_obj_set_width(ui->victron_config.list, lv_pct(100));
    lv_obj_set_height(ui->victron_config.list, LV_SIZE_CONTENT);
    lv_obj_set_layout(ui->victron_config.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui->victron_config.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ui->victron_config.list, 14, 0);
    lv_obj_set_scroll_dir(ui->victron_config.list, LV_DIR_VER);

    /* Initialize victron config state */
    ui->victron_config.count = 0;
    ui->victron_config.updating = false;
    for (size_t i = 0; i < UI_MAX_VICTRON_DEVICES; ++i) {
        ui->victron_config.rows[i] = NULL;
        ui->victron_config.mac_textareas[i] = NULL;
        ui->victron_config.key_textareas[i] = NULL;
        ui->victron_config.name_textareas[i] = NULL;
        ui->victron_config.enabled_checkboxes[i] = NULL;
        ui->victron_config.device_type_labels[i] = NULL;
        ui->victron_config.product_name_labels[i] = NULL;
        ui->victron_config.error_labels[i] = NULL;
        ui->victron_config.status_containers[i] = NULL;
    }

    /* Load existing configuration */
    victron_config_load(ui);

    /* Update controls state */
    victron_config_update_controls(ui);
}

void victron_config_load(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }

    victron_device_config_t devices[UI_MAX_VICTRON_DEVICES];
    uint8_t count = 0;
    
    esp_err_t err = load_victron_devices(devices, &count, UI_MAX_VICTRON_DEVICES);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "Failed to load Victron devices: %s", esp_err_to_name(err));
        count = 0;
    }

    ui->victron_config.count = count;
    
    /* Create UI rows for loaded devices */
    for (size_t i = 0; i < count; ++i) {
        victron_config_create_row(ui, i);
        
        /* Set the loaded values */
        if (ui->victron_config.mac_textareas[i]) {
            lv_textarea_set_text(ui->victron_config.mac_textareas[i], devices[i].mac_address);
        }
        
        if (ui->victron_config.name_textareas[i]) {
            lv_textarea_set_text(ui->victron_config.name_textareas[i], devices[i].device_name);
        }
        
        if (ui->victron_config.key_textareas[i]) {
            char hex_key[33] = {0};
            for (int j = 0; j < 16; ++j) {
                sprintf(hex_key + j * 2, "%02X", devices[i].aes_key[j]);
            }
            lv_textarea_set_text(ui->victron_config.key_textareas[i], hex_key);
        }
        
        if (ui->victron_config.enabled_checkboxes[i]) {
            if (devices[i].enabled) {
                lv_obj_add_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

void victron_config_refresh(ui_state_t *ui)
{
    if (ui == NULL || ui->victron_config.list == NULL) {
        return;
    }

    ESP_LOGI(TAG_SETTINGS, "Refreshing Victron device configuration, free heap: %lu PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Clear existing device rows - borrar hijos del container uno a uno
    while(lv_obj_get_child_cnt(ui->victron_config.list) > 0) {
        lv_obj_del(lv_obj_get_child(ui->victron_config.list, 0));
    }
    for (size_t i = 0; i < UI_MAX_VICTRON_DEVICES; ++i) {
        ui->victron_config.rows[i] = NULL;
        ui->victron_config.mac_textareas[i] = NULL;
        ui->victron_config.key_textareas[i] = NULL;
        ui->victron_config.name_textareas[i] = NULL;
        ui->victron_config.enabled_checkboxes[i] = NULL;
        ui->victron_config.device_type_labels[i] = NULL;
        ui->victron_config.product_name_labels[i] = NULL;
        ui->victron_config.error_labels[i] = NULL;
        ui->victron_config.status_containers[i] = NULL;
    }

    // Reset count
    ui->victron_config.count = 0;

    // Reload configuration from storage
    victron_config_load(ui);

    // Update controls state
    victron_config_update_controls(ui);

    ESP_LOGI(TAG_SETTINGS, "Victron device refresh completed, showing %d devices", ui->victron_config.count);
}

void victron_config_create_row(ui_state_t *ui, size_t index)
{
    if (ui == NULL || ui->victron_config.list == NULL || index >= UI_MAX_VICTRON_DEVICES) {
        return;
    }

    /* Card por dispositivo (border cyan, mismo estilo que las demás) */
    lv_obj_t *row = ui_card_create(ui->victron_config.list, UI_COLOR_CYAN);
    if (row == NULL) { ESP_LOGE("UI", "row create failed idx=%d", (int)index); return; }

    /* Header con título "Device N" + switch enabled a la derecha */
    char title_buf[20];
    snprintf(title_buf, sizeof(title_buf), "Device %d", (int)(index + 1));
    lv_obj_t *header_row = ui_card_set_title(row, LV_SYMBOL_BLUETOOTH,
                                             title_buf, UI_COLOR_CYAN);

    /* Switch en vez de checkbox: el LV_SYMBOL_OK del indicador no renderiza
     * bien con la fuente Inter aliased (lv_font_montserrat_20_es ->
     * lv_font_inter_20_es) -> el tick salia cortado. lv_switch no usa
     * caracter de tick y queda consistente con el resto de toggles de la UI
     * (Wi-Fi, brillo, screensaver, etc). API igual: LV_STATE_CHECKED +
     * LV_EVENT_VALUE_CHANGED.
     *
     * Layout: [label "Activo"] + [switch], en el header_row.
     * El nombre del puntero (enabled_cb) se mantiene para no tocar
     * referencias en victron_config.enabled_checkboxes[]. */
    lv_obj_t *enabled_lbl = lv_label_create(header_row);
    lv_label_set_text(enabled_lbl, "Activo");
    lv_obj_set_style_text_font(enabled_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(enabled_lbl, UI_COLOR_TEXT, 0);

    lv_obj_t *enabled_cb = lv_switch_create(header_row);
    /* Sin set_size explicito: el theme LVGL le da forma de pill bien
     * proporcionada (con set_size 50x26 se renderizaba como dos rayas).
     * Acento cyan en CHECKED para coherencia con el card de Keys. */
    lv_obj_set_style_bg_color(enabled_cb, UI_COLOR_CYAN,
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(enabled_cb, victron_enabled_checkbox_event_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    /* Body en 2 columnas: izquierda inputs, derecha estado en vivo */
    lv_obj_t *body = lv_obj_create(row);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 14, 0);

    /* Columna izquierda — inputs (flex_grow=1 para mitad ancho) */
    lv_obj_t *col_left = lv_obj_create(body);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_height(col_left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col_left, 1);
    lv_obj_set_layout(col_left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col_left, 6, 0);

    lv_obj_t *name_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(name_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(name_label, "Nombre:");

    lv_obj_t *name_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(name_ta, 31);
    lv_obj_set_width(name_ta, lv_pct(100));
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_placeholder_text(name_ta, "ej. Solar Charger 1");
    lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(name_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    lv_obj_t *mac_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(mac_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(mac_label, "Dirección MAC:");

    lv_obj_t *mac_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(mac_ta, 17);
    lv_obj_set_width(mac_ta, lv_pct(100));
    lv_textarea_set_one_line(mac_ta, true);
    lv_textarea_set_placeholder_text(mac_ta, "XX:XX:XX:XX:XX:XX");
    lv_obj_set_style_text_font(mac_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(mac_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    lv_obj_t *key_label = lv_label_create(col_left);
    lv_obj_set_style_text_font(key_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(key_label, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(key_label, "Clave AES (32 hex):");

    lv_obj_t *key_ta = lv_textarea_create(col_left);
    lv_textarea_set_max_length(key_ta, 32);
    lv_obj_set_width(key_ta, lv_pct(100));
    lv_textarea_set_one_line(key_ta, true);
    lv_textarea_set_placeholder_text(key_ta, "00000000000000000000000000000000");
    lv_obj_set_style_text_font(key_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(key_ta, victron_field_ta_event_cb, LV_EVENT_READY, ui);

    /* Columna derecha — sub-card de estado en vivo (border verde) */
    lv_obj_t *status_container = lv_obj_create(body);
    lv_obj_remove_style_all(status_container);
    lv_obj_set_height(status_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(status_container, 1);
    lv_obj_set_layout(status_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(status_container, 6, 0);
    lv_obj_set_style_pad_all(status_container, 12, 0);
    lv_obj_set_style_bg_color(status_container, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_opa(status_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_container, 8, 0);
    lv_obj_set_style_border_width(status_container, 1, 0);
    lv_obj_set_style_border_opa(status_container, LV_OPA_50, 0);
    lv_obj_set_style_border_color(status_container, UI_COLOR_GREEN, 0);

    lv_obj_t *device_type_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(device_type_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(device_type_lbl, "Tipo: --");
    lv_obj_set_style_text_color(device_type_lbl, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *product_name_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(product_name_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(product_name_lbl, "Producto: --");
    lv_obj_set_style_text_color(product_name_lbl, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *error_lbl = lv_label_create(status_container);
    lv_obj_set_style_text_font(error_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(error_lbl, "Estado: esperando datos...");
    lv_obj_set_style_text_color(error_lbl, UI_COLOR_TEXT_DIM, 0);
    /* LONG_DOT en vez de LONG_WRAP: status_container tiene flex_grow=1 + flex
     * column con 3 hermanos dentro de scroll => combo exacto del WDT al
     * construir (memo feedback-lvgl-label-wrap-flex-grow-wdt). */
    lv_label_set_long_mode(error_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(error_lbl, lv_pct(100));

    /* Store references */
    ui->victron_config.rows[index] = row;
    ui->victron_config.mac_textareas[index] = mac_ta;
    ui->victron_config.key_textareas[index] = key_ta;
    ui->victron_config.name_textareas[index] = name_ta;
    ui->victron_config.enabled_checkboxes[index] = enabled_cb;
    ui->victron_config.device_type_labels[index] = device_type_lbl;
    ui->victron_config.product_name_labels[index] = product_name_lbl;
    ui->victron_config.error_labels[index] = error_lbl;
    ui->victron_config.status_containers[index] = status_container;
}

void victron_config_update_controls(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }

    bool can_add = ui->victron_config.count < UI_MAX_VICTRON_DEVICES;
    bool can_remove = ui->victron_config.count > 0;

    if (ui->victron_config.add_btn != NULL) {
        if (can_add) {
            lv_obj_clear_state(ui->victron_config.add_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ui->victron_config.add_btn, LV_STATE_DISABLED);
        }
    }

    if (ui->victron_config.remove_btn != NULL) {
        if (can_remove) {
            lv_obj_clear_state(ui->victron_config.remove_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ui->victron_config.remove_btn, LV_STATE_DISABLED);
        }
    }
}

void victron_config_persist(ui_state_t *ui)
{
    if (ui == NULL || ui->victron_config.updating) {
        return;
    }

    victron_device_config_t devices[UI_MAX_VICTRON_DEVICES];
    memset(devices, 0, sizeof(devices));

    /* Collect data from UI */
    for (size_t i = 0; i < ui->victron_config.count; ++i) {
        /* Device name */
        if (ui->victron_config.name_textareas[i]) {
            const char *name = lv_textarea_get_text(ui->victron_config.name_textareas[i]);
            if (name && name[0] != '\0') {
                strncpy(devices[i].device_name, name, sizeof(devices[i].device_name) - 1);
            }
        }

        /* MAC address */
        if (ui->victron_config.mac_textareas[i]) {
            const char *mac = lv_textarea_get_text(ui->victron_config.mac_textareas[i]);
            if (mac && strlen(mac) == 17) {
                strncpy(devices[i].mac_address, mac, sizeof(devices[i].mac_address) - 1);
            } else {
                strcpy(devices[i].mac_address, "00:00:00:00:00:00");
            }
        }

        /* AES Key */
        if (ui->victron_config.key_textareas[i]) {
            const char *hex = lv_textarea_get_text(ui->victron_config.key_textareas[i]);
            if (hex && strlen(hex) == 32) {
                for (int j = 0; j < 16; ++j) {
                    char tmp[3] = { hex[j * 2], hex[j * 2 + 1], 0 };
                    devices[i].aes_key[j] = (uint8_t)strtol(tmp, NULL, 16);
                }
            }
        }

        /* Enabled state */
        if (ui->victron_config.enabled_checkboxes[i]) {
            devices[i].enabled = lv_obj_has_state(ui->victron_config.enabled_checkboxes[i], LV_STATE_CHECKED);
        }
    }

    esp_err_t err = save_victron_devices(devices, ui->victron_config.count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "Failed to save Victron devices: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_SETTINGS, "Saved %d Victron devices", ui->victron_config.count);
        // Reload BLE configuration to use updated device settings
        victron_ble_reload_device_config();
    }
}



/* ── Modal de confirmación reutilizable para acciones en Victron Keys ──
 * Cualquier modificación (añadir, quitar, toggle activo) pasa por aquí. */
typedef void (*victron_confirm_fn)(void *ud);

static lv_obj_t *s_victron_confirm_modal = NULL;
static victron_confirm_fn s_victron_confirm_ok = NULL;
static victron_confirm_fn s_victron_confirm_cancel = NULL;
static void *s_victron_confirm_ud = NULL;

static void victron_confirm_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    bool confirmed = (txt && strstr(txt, "Confirmar") != NULL);

    victron_confirm_fn ok = s_victron_confirm_ok;
    victron_confirm_fn cancel = s_victron_confirm_cancel;
    void *ud = s_victron_confirm_ud;
    s_victron_confirm_ok = NULL;
    s_victron_confirm_cancel = NULL;
    s_victron_confirm_ud = NULL;

    if (s_victron_confirm_modal) {
        lv_obj_del(s_victron_confirm_modal);
        s_victron_confirm_modal = NULL;
    }
    if (confirmed) { if (ok) ok(ud); }
    else           { if (cancel) cancel(ud); }
}

static void victron_show_confirm_modal(const char *msg,
                                       victron_confirm_fn on_ok,
                                       victron_confirm_fn on_cancel,
                                       void *ud)
{
    if (s_victron_confirm_modal) return;
    s_victron_confirm_ok = on_ok;
    s_victron_confirm_cancel = on_cancel;
    s_victron_confirm_ud = ud;

    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_victron_confirm_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 560, 240);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  ¿Confirmar cambio?");

    lv_obj_t *m = lv_label_create(dlg);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(m, lv_color_white(), 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(m, msg ? msg : "Vas a modificar la configuración. ¿Continuar?");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_cancel = lv_btn_create(row_btns);
    lv_obj_set_size(btn_cancel, 200, 56);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancelar");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, victron_confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 200, 56);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, "Confirmar");
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, victron_confirm_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Acciones reales tras confirmación ─────────────────────────────── */

static void victron_do_add(void *ud)
{
    ui_state_t *ui = (ui_state_t *)ud;
    if (!ui || ui->victron_config.count >= UI_MAX_VICTRON_DEVICES) return;
    size_t index = ui->victron_config.count;
    ui->victron_config.count++;
    victron_config_create_row(ui, index);
    victron_config_update_controls(ui);
    victron_config_persist(ui);
}

static void victron_do_remove(void *ud)
{
    ui_state_t *ui = (ui_state_t *)ud;
    if (!ui || ui->victron_config.count == 0) return;
    size_t index = ui->victron_config.count - 1;
    if (ui->victron_config.rows[index] != NULL) {
        lv_obj_del(ui->victron_config.rows[index]);
    }
    ui->victron_config.rows[index] = NULL;
    ui->victron_config.mac_textareas[index] = NULL;
    ui->victron_config.key_textareas[index] = NULL;
    ui->victron_config.name_textareas[index] = NULL;
    ui->victron_config.enabled_checkboxes[index] = NULL;
    ui->victron_config.device_type_labels[index] = NULL;
    ui->victron_config.product_name_labels[index] = NULL;
    ui->victron_config.error_labels[index] = NULL;
    ui->victron_config.status_containers[index] = NULL;
    ui->victron_config.count--;
    victron_config_update_controls(ui);
    victron_config_persist(ui);
}

/* Para el toggle: guardamos el checkbox + nuevo estado para revertir */
typedef struct {
    ui_state_t *ui;
    lv_obj_t *cb;
    bool new_state;  /* estado al que se cambió antes de mostrar el modal */
} victron_toggle_ctx_t;

static victron_toggle_ctx_t s_toggle_ctx;

static void victron_do_toggle_confirm(void *ud)
{
    victron_toggle_ctx_t *ctx = (victron_toggle_ctx_t *)ud;
    if (!ctx || !ctx->ui) return;
    victron_config_persist(ctx->ui);
}

static void victron_do_toggle_cancel(void *ud)
{
    victron_toggle_ctx_t *ctx = (victron_toggle_ctx_t *)ud;
    if (!ctx || !ctx->cb) return;
    /* Revertir el estado del checkbox al anterior (opuesto al nuevo) */
    if (ctx->new_state) lv_obj_clear_state(ctx->cb, LV_STATE_CHECKED);
    else                lv_obj_add_state(ctx->cb, LV_STATE_CHECKED);
}

/* ── Wrappers de los event_cb originales con confirmación ──────────── */

void victron_config_add_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui || ui->victron_config.count >= UI_MAX_VICTRON_DEVICES) return;
    victron_show_confirm_modal("Vas a añadir un nuevo dispositivo Victron. ¿Continuar?",
                               victron_do_add, NULL, ui);
}

void victron_config_remove_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_state_t *ui = lv_event_get_user_data(e);
    if (!ui || ui->victron_config.count == 0) return;
    victron_show_confirm_modal("Vas a eliminar el último dispositivo Victron. ¿Continuar?",
                               victron_do_remove, NULL, ui);
}

void victron_enabled_checkbox_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    if (!ui || !cb || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Capturar nuevo estado para poder revertir si cancela */
    s_toggle_ctx.ui = ui;
    s_toggle_ctx.cb = cb;
    s_toggle_ctx.new_state = lv_obj_has_state(cb, LV_STATE_CHECKED);
    const char *msg = s_toggle_ctx.new_state
        ? "Vas a activar este dispositivo. ¿Continuar?"
        : "Vas a desactivar este dispositivo. ¿Continuar?";
    victron_show_confirm_modal(msg,
        victron_do_toggle_confirm, victron_do_toggle_cancel, &s_toggle_ctx);
}

void victron_field_ta_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);
    if (!(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY)) {
        return;
    }

    victron_config_persist(ui);
}
