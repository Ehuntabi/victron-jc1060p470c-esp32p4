#ifndef UI_UI_STATE_H
#define UI_UI_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>
#include "victron_ble.h"

struct ui_device_view;

#define UI_MAX_DEVICE_VIEWS 32
#define UI_RELAY_GPIO_UNASSIGNED UINT8_MAX
#define UI_MAX_VICTRON_DEVICES 8

typedef struct {
    lv_style_t small;
    lv_style_t value;
    lv_style_t big;
    lv_style_t medium;
    lv_style_t title;
} ui_styles_t;

typedef struct {
    lv_obj_t *ssid;
    lv_obj_t *password;
    lv_obj_t *ap_enable;
    lv_obj_t *password_toggle;
} ui_wifi_controls_t;

typedef enum {
    UI_SCREENSAVER_MODE_DIM = 0,    // Atenuar pantalla (modo actual)
    UI_SCREENSAVER_MODE_ROTATE = 1, // Rotar entre vistas
} ui_screensaver_mode_t;

typedef struct {
    bool enabled;
    uint8_t brightness;
    uint16_t timeout;
    bool active;
    uint8_t mode;                   // ui_screensaver_mode_t
    uint8_t rotate_period_min;      // 1..10 minutos por vista
    uint8_t rotate_index;           // indice actual de vista al rotar
    lv_timer_t *timer;
    lv_timer_t *rotate_timer;
    lv_obj_t *checkbox;
    lv_obj_t *slider_brightness;
    lv_obj_t *spinbox_timeout;
} ui_screensaver_state_t;

typedef enum {
    UI_VIEW_MODE_AUTO = 0,           // Automatic detection based on device type
    UI_VIEW_MODE_DEFAULT_BATTERY,    // Always show default battery view
    UI_VIEW_MODE_SOLAR_CHARGER,     // Always show solar charger view
    UI_VIEW_MODE_BATTERY_MONITOR,   // Always show battery monitor view
    UI_VIEW_MODE_INVERTER,          // Always show inverter view
    UI_VIEW_MODE_DCDC_CONVERTER,    // Always show DC/DC converter view
    UI_VIEW_MODE_COUNT              // Number of view modes
} ui_view_mode_t;

typedef struct {
    ui_view_mode_t mode;
    lv_obj_t *dropdown;
} ui_view_selection_t;


typedef struct ui_state {
    lv_obj_t *tabview;
    lv_obj_t *tab_live;
    lv_obj_t *tab_settings;
    lv_obj_t *frigo_page;
    lv_obj_t *settings_menu;
    lv_obj_t *keyboard;
    ui_styles_t styles;
    ui_wifi_controls_t wifi;
    ui_screensaver_state_t screensaver;
    ui_view_selection_t view_selection;
    lv_obj_t *lbl_error;        // Legacy - kept for compatibility
    lv_obj_t *lbl_device_type;  // Legacy - kept for compatibility
    lv_obj_t *lbl_product_name; // Legacy - kept for compatibility
    lv_obj_t *lbl_no_data;
    lv_obj_t *ta_mac;  // Legacy - kept for compatibility
    lv_obj_t *ta_key;  // Legacy - kept for compatibility
    struct ui_device_view *default_view;
    uint8_t brightness;
    bool victron_debug_enabled;
    lv_obj_t *victron_debug_checkbox;
    victron_record_type_t current_device_type;
    uint16_t current_product_id;
    char current_device_mac[18];  // Current device MAC address
    
    // Device activity tracking for status updates
    char last_active_devices[UI_MAX_VICTRON_DEVICES][18];  // MAC addresses of recently active devices
    uint32_t last_activity_time[UI_MAX_VICTRON_DEVICES];   // Timestamps of last activity
    lv_timer_t *device_timeout_timer;                      // Timer to check for offline devices
    
    struct ui_device_view *active_view;
    struct ui_device_view *views[UI_MAX_DEVICE_VIEWS];
    bool has_received_data;
    uint16_t tab_settings_index;
    lv_obj_t *lbl_clock;       // Reloj en barra superior
    lv_obj_t *lbl_ble;
    lv_obj_t *lbl_volume;
    lv_obj_t *lbl_wifi;
    lv_obj_t *bottom_bar;         // Indicador BLE
    /* About page dynamic info */
    lv_obj_t *lbl_about_uptime;
    lv_obj_t *lbl_about_heap;
    lv_obj_t *lbl_about_sd;
    lv_obj_t *lbl_about_ip;
    lv_obj_t *alarm_border;    // Borde rojo alarma congelador
    lv_obj_t *screen_chart;    // Pantalla grafica temperaturas
    
    // Victron devices configuration
    struct {
        uint8_t count;
        lv_obj_t *container;
        lv_obj_t *list;
        lv_obj_t *add_btn;
        lv_obj_t *remove_btn;
        lv_obj_t *rows[8];           // UI_MAX_VICTRON_DEVICES
        lv_obj_t *mac_textareas[8];
        lv_obj_t *key_textareas[8];
        lv_obj_t *name_textareas[8];
        lv_obj_t *enabled_checkboxes[8];
        lv_obj_t *device_type_labels[8];
        lv_obj_t *product_name_labels[8];
        lv_obj_t *error_labels[8];
        lv_obj_t *status_containers[8];
        bool updating;
    } victron_config;
} ui_state_t;

#endif /* UI_UI_STATE_H */
