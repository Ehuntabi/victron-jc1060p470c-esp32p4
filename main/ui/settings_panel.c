#include "settings_panel.h"
#include "settings_common.h"

/* Paginas ya separadas en sus propios ficheros (troceo de settings_panel.c). */
void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display);
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
#include "solar_daily.h"
#include "ne185_vlog.h"   /* ne185_vlog_flush: tambien se vuelca al finalizar viaje */

/* Vuelca a la tarjeta/NVS todo lo que vive en RAM, antes de un esp_restart.
 *
 * Lo comparten los DOS caminos de reinicio (el switch de Wi-Fi y el boton
 * Reiniciar) para que no puedan divergir: hasta ahora los dos volcaban solo el
 * datalogger y el historico de bateria, asi que cada reinicio se llevaba por
 * delante hasta ~5 min de contadores de viaje (van a NVS cada 5 min), el dia de
 * produccion solar en curso y hasta ~10 min del log de voltaje NE185.
 *
 * OJO: esto NO salva las vistas "HOY" de las graficas. Salen de anillos en RAM
 * (battery_history y datalogger) y arrancan vacias tras cualquier reinicio; lo
 * que sobrevive son los CSV de la tarjeta, que es justo lo que se vuelca aqui. */
static void flush_all_before_restart(void)
{
    ESP_LOGI("UI", "Flushing data before restart...");
    datalogger_flush();
    battery_history_flush();
    solar_daily_flush();
    trip_computer_flush();
    ne185_vlog_flush();
}
#include <time.h>

// Forward declaration for view update function
extern void ui_force_view_update(void);
extern void ui_start_capture_carousel(void);
extern bool ui_capture_carousel_running(void);
extern void ui_gallery_open(void);   /* visor de galeria en pantalla (gallery.c) */

#define WIFI_NAMESPACE "wifi"

const char *TAG_SETTINGS = "UI_SETTINGS";
/* La version mostrada en About sale de esp_app_get_description()->version, que
 * ESP-IDF rellena automaticamente con `git describe` (el tag mas reciente, p.ej.
 * "v1.0.0"; en builds fuera de un tag, "v1.0.0-3-gABCDEF"). Este texto es solo
 * un fallback por si la descripcion de la app no estuviera disponible. */
static const char *APP_VERSION_FALLBACK = "v?";


/* Pagina Wi-Fi: vive en settings_wifi.c. */
void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled);
void wifi_event_cb(lv_event_t *e);
void password_toggle_btn_event_cb(lv_event_t *e);
static void about_refresh_dynamic(ui_state_t *ui);
static void about_timer_cb(lv_timer_t *t);
/* Trip computer + backup: definidos mas abajo, usados por la pagina Tarjeta SD */
static lv_obj_t *s_trip_label = NULL;
static void trip_label_refresh(void);
static void trip_reset_btn_cb(lv_event_t *e);
static void trip_finish_btn_cb(lv_event_t *e);
static void backup_export_cb(lv_event_t *e);
static void backup_import_cb(lv_event_t *e);
static void sd_trip_timer_cb(lv_timer_t *t);
static void reboot_btn_cb(lv_event_t *e);
static void brightness_slider_event_cb(lv_event_t *e);



/* Switch "Carrusel captura pantalla": al encenderlo lanza el carrusel de
 * captura a la SD; la tarea lo devuelve a OFF al terminar. */
static void cb_capture_carousel_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (!ui) return;
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        if (ui_capture_carousel_running()) return;   /* ya en curso */
        if (ui->capture_status_lbl)
            lv_label_set_text(ui->capture_status_lbl, "Capturando pantallas...");
        ui_start_capture_carousel();
    }
    /* Apagado manual: no hacemos nada; la tarea lo dejara en OFF al terminar. */
}

/* Boton "Ver galeria en pantalla": abre el visor de las capturas de la SD. */
static void cb_open_gallery(lv_event_t *e)
{
    (void)e;
    ui_gallery_open();
}

static void cb_screensaver_event_cb(lv_event_t *e);
static void slider_ss_brightness_event_cb(lv_event_t *e);
static void spinbox_ss_time_increment_event_cb(lv_event_t *e);
static void spinbox_ss_time_decrement_event_cb(lv_event_t *e);
static void screensaver_timer_cb(lv_timer_t *timer);
static void view_selection_dropdown_event_cb(lv_event_t *e);

void screensaver_enable(ui_state_t *ui, bool enable);
static void screensaver_wake(ui_state_t *ui);
/* El timer del salvapantallas hace de detector de reposo. Debe correr si el
 * salvapantallas esta activado O si el modo nocturno esta activado (para poder
 * apagar de noche aunque el salvapantallas este off). */
static void screensaver_sync_timer_state(ui_state_t *ui);

/* ── Modo nocturno (auto brillo por hora del RTC) ─────────────── */
static void night_switch_cb(lv_event_t *e);
static void night_start_dec_cb(lv_event_t *e);
static void night_start_inc_cb(lv_event_t *e);
static void night_end_dec_cb(lv_event_t *e);
static void night_end_inc_cb(lv_event_t *e);

/* Aplica inmediatamente el brillo correcto según hora actual + config. */
static bool night_in_window(int h, uint8_t s, uint8_t e)
{
    if (s == e) return false;
    if (s < e)  return h >= s && h < e;
    return h >= s || h < e;     /* cruza medianoche */
}

/* True si el modo nocturno esta activo y estamos dentro de su franja horaria. */
static bool night_active_now(ui_state_t *ui)
{
    if (!ui || !ui->night_mode.enabled) return false;
    time_t now = time(NULL);
    if (now < 1000000000L) return false;      /* RTC aun sin hora valida */
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    return night_in_window(tm_local.tm_hour,
                           ui->night_mode.start_h,
                           ui->night_mode.end_h);
}

void apply_brightness_for_now(ui_state_t *ui)
{
    if (!ui) return;
    if (ui->screensaver.active) return;       /* el SS gestiona su brillo */
    /* Interactuando (SS inactivo) => brillo normal, AUNQUE sea de noche: el
     * apagado nocturno lo hace el salvapantallas al quedar en reposo
     * (screensaver_timer_cb), y el toque siempre despierta a normal. Antes se
     * ponia 0% aqui y, con el SS inactivo, el toque no lo recuperaba. */
    bsp_display_brightness_set(ui->brightness);
}


// Victron devices configuration functions
void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron);
static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page_about);
static void create_logs_settings_page(ui_state_t *ui, lv_obj_t *page);

/* ── Lazy populate de sub-paginas Settings ────────────────────────
 * Wi-Fi/Display/Keys/Logs/Consola/Sound/About se crean vacias en boot y
 * solo se popula su contenido la primera vez que el usuario navega a esa
 * pagina. Razon: con todas eager + N device cards en Keys, el pool LVGL
 * se llenaba y draw_shadow disparaba TASK_WDT en bucle (memo
 * feedback-lvgl-mem-custom-psram). Con LV_MEM_CUSTOM=y ya no hay techo,
 * pero el lazy reduce boot time y memoria ocupada si el usuario no entra
 * a una sub-pagina. Frigo se mantiene eager (es la mas usada). */
struct settings_page_ctx_s;
typedef struct settings_page_ctx_s settings_page_ctx_t;
/* El tipo settings_page_ctx_t vive ahora en settings_common.h (lo usan varias
 * paginas ya separadas). */
#define SETTINGS_PAGE_CTX_MAX 12
static settings_page_ctx_t s_page_ctxs[SETTINGS_PAGE_CTX_MAX];
static size_t s_page_ctx_count = 0;

/* Forward decls de los populate wrappers (defs cerca de settings_menu_add_entry). */
static void populate_wifi(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_display(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_sd(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_keys(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_logs(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_sound(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_about(settings_page_ctx_t *ctx, lv_obj_t *page);

/* ── Scrollbar visible en cualquier pagina de Settings ────────── */
/* Aplica scrollbar AUTO (visible cuando hay overflow) con estilo claro
 * para indicar que se puede deslizar. Llamar tras crear la page. */
void style_settings_scrollbar(lv_obj_t *page)
{
    if (!page) return;
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xFF9800), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(page, 6, LV_PART_SCROLLBAR);
}

/* === SoC umbrales (dropdowns) === */
static const int s_soc_crit_options[] = { 10, 20, 30, 40 };
static const int s_soc_warn_options[] = { 40, 50, 60, 70 };

static void soc_crit_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0])) {
        alerts_set_soc_critical(s_soc_crit_options[sel]);
    }
}

static void soc_warn_dd_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0])) {
        alerts_set_soc_warning(s_soc_warn_options[sel]);
    }
}

static void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page);
static void alarm_min_dd_cb_sound(lv_event_t *e);
static void alarm_temp_dd_cb_sound(lv_event_t *e);




static void portal_page_cb(lv_event_t *e);
static void reactivate_portal_cb(lv_event_t *e);
/* Definidas en settings_victron_keys.c (ver nota mas arriba). */
void victron_config_add_btn_event_cb(lv_event_t *e);
void victron_config_remove_btn_event_cb(lv_event_t *e);
void victron_config_create_row(ui_state_t *ui, size_t index);
void victron_config_update_controls(ui_state_t *ui);
void victron_config_persist(ui_state_t *ui);
void victron_config_load(ui_state_t *ui);
void victron_config_refresh(ui_state_t *ui);
void victron_enabled_checkbox_event_cb(lv_event_t *e);
void victron_field_ta_event_cb(lv_event_t *e);
static void victron_config_update_device_status(ui_state_t *ui, const char *mac_address, 
                                                const char *device_type, const char *product_name, 
                                                const char *error_info);
static int victron_config_find_device_by_mac(ui_state_t *ui, const char *mac_address);


/* --- Estilo botones del menu Settings --- */
lv_obj_t *s_settings_menu = NULL;
lv_obj_t *s_settings_main_page = NULL;
static lv_obj_t *s_settings_back_btn = NULL;
static lv_obj_t *s_settings_main_header = NULL;
static lv_style_t s_settings_btn_style;
static lv_style_t s_settings_btn_pressed_style;
static bool s_settings_styles_inited = false;
#define SETTINGS_DEFAULT_ACCENT 0xFF9800   /* naranja Victron */

void ui_settings_panel_go_to_main(void)
{
    if (s_settings_menu && s_settings_main_page) {
        lv_menu_set_page(s_settings_menu, s_settings_main_page);
    }
}

static void settings_btn_styles_init(void);
static settings_page_ctx_t *settings_menu_add_entry(
    ui_state_t *ui, lv_obj_t *main_page,
    lv_obj_t *menu, lv_obj_t *target_page,
    const char *title, const char *subtitle,
    const char *icon, uint32_t accent,
    void (*populate)(settings_page_ctx_t *ctx, lv_obj_t *page));
static void settings_menu_page_changed_cb(lv_event_t *e);



/* ── Splash dropdown ──────────────────────────────────────────── */

/* Switch de auto-encendido de cargas (luz interior + bomba de agua) al
 * arrancar el P4. Persiste en NVS via ne185_set_autostart(). Reubicado desde
 * la antigua pagina "Consola" (instrumental NE185 ya retirado). */
static void autostart_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ne185_set_autostart(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Pagina "Tarjeta SD": carrusel de capturas + visor de imagenes de la SD. */
/* Refresco del Trip computer (card reubicada en Tarjeta SD): solo si esta
 * visible, para no gastar cada segundo cuando no se mira. */
static void sd_trip_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_state_t *ui = t ? (ui_state_t *)t->user_data : NULL;
    if (s_trip_label && lv_obj_is_visible(s_trip_label)) trip_label_refresh();

    /* Tamano/espacio libre de la SD: solo cuando la etiqueta esta visible. */
    if (ui && ui->lbl_about_sd && lv_obj_is_visible(ui->lbl_about_sd)) {
        /* f_getfree escanea la FAT (bloqueante): tomar el bus de la SD sin
         * esperar (timeout 0); si la camara lo tiene, saltar esta actualizacion
         * para no congelar el render. */
        if (!camera_sd_bus_lock(0)) return;
        FATFS *fs = NULL;
        DWORD free_clusters = 0;
        if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
            uint64_t sect_per_cluster = fs->csize;
            uint64_t total_sect = (fs->n_fatent - 2) * sect_per_cluster;
            uint64_t free_sect  = (uint64_t)free_clusters * sect_per_cluster;
            uint64_t total_mb = (total_sect * 512ULL) / (1024ULL * 1024ULL);
            uint64_t free_mb  = (free_sect  * 512ULL) / (1024ULL * 1024ULL);
            lv_label_set_text_fmt(ui->lbl_about_sd, "SD: %u/%u MB libres",
                (unsigned)free_mb, (unsigned)total_mb);
        } else {
            lv_label_set_text(ui->lbl_about_sd, "SD: no montada");
        }
        camera_sd_bus_unlock();
    }
}

void create_sd_settings_page(ui_state_t *ui, lv_obj_t *page_sd)
{
    style_settings_scrollbar(page_sd);
    lv_obj_t *cont = lv_obj_create(page_sd);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card Carrusel captura pantalla === */
    lv_obj_t *card_cap = lv_obj_create(cont);
    lv_obj_set_width(card_cap, lv_pct(100));
    lv_obj_set_height(card_cap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_cap, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_cap, lv_color_hex(0x29B6F6), 0);  /* azul */
    lv_obj_set_style_border_width(card_cap, 1, 0);
    lv_obj_set_style_radius(card_cap, 12, 0);
    lv_obj_set_style_pad_all(card_cap, 16, 0);
    lv_obj_set_style_pad_gap(card_cap, 8, 0);
    lv_obj_set_layout(card_cap, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_cap, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *cap_row = lv_obj_create(card_cap);
    lv_obj_remove_style_all(cap_row);
    lv_obj_set_size(cap_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(cap_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cap_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap_title = lv_label_create(cap_row);
    lv_obj_set_style_text_font(cap_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(cap_title, lv_color_hex(0x29B6F6), 0);
    lv_label_set_text(cap_title, LV_SYMBOL_IMAGE "  Carrusel captura pantalla");

    ui->capture_switch = lv_switch_create(cap_row);
    lv_obj_set_size(ui->capture_switch, 50, 28);
    lv_obj_set_style_bg_color(ui->capture_switch, lv_color_hex(0x29B6F6),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(ui->capture_switch, cb_capture_carousel_cb,
                        LV_EVENT_VALUE_CHANGED, ui);

    ui->capture_status_lbl = lv_label_create(card_cap);
    lv_obj_set_style_text_font(ui->capture_status_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->capture_status_lbl, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->capture_status_lbl,
                      "Guarda las 8 pantallas de datos en la SD");

    ui->lbl_about_sd = lv_label_create(card_cap);
    lv_obj_set_style_text_font(ui->lbl_about_sd, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->lbl_about_sd, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->lbl_about_sd, "SD: --");

    /* === Card Visor de imagenes (separado del carrusel) === */
    lv_obj_t *card_view = lv_obj_create(cont);
    lv_obj_set_width(card_view, lv_pct(100));
    lv_obj_set_height(card_view, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_view, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_view, lv_color_hex(0x26C6DA), 0);  /* cyan */
    lv_obj_set_style_border_width(card_view, 1, 0);
    lv_obj_set_style_radius(card_view, 12, 0);
    lv_obj_set_style_pad_all(card_view, 16, 0);
    lv_obj_set_style_pad_gap(card_view, 8, 0);
    lv_obj_set_layout(card_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_view, LV_FLEX_FLOW_COLUMN);

    /* Cabecera: titulo a la izquierda, boton a la derecha */
    lv_obj_t *view_head = lv_obj_create(card_view);
    lv_obj_remove_style_all(view_head);
    lv_obj_set_width(view_head, lv_pct(100));
    lv_obj_set_height(view_head, LV_SIZE_CONTENT);
    lv_obj_set_layout(view_head, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *view_title = lv_label_create(view_head);
    lv_obj_set_flex_grow(view_title, 1);
    lv_obj_set_style_text_font(view_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(view_title, lv_color_hex(0x26C6DA), 0);
    lv_label_set_text(view_title, LV_SYMBOL_IMAGE "  Visor de imagenes");

    lv_obj_t *btn_gal = lv_btn_create(view_head);
    lv_obj_set_width(btn_gal, LV_SIZE_CONTENT);   /* acorde al texto + icono */
    lv_obj_set_height(btn_gal, 46);
    lv_obj_set_style_pad_hor(btn_gal, 24, 0);
    lv_obj_set_style_bg_color(btn_gal, lv_color_hex(0x0288D1), 0);
    lv_obj_set_style_radius(btn_gal, 8, 0);
    lv_obj_t *lbl_gal = lv_label_create(btn_gal);
    lv_obj_set_style_text_font(lbl_gal, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_gal, LV_SYMBOL_IMAGE "  Ver capturas");
    lv_obj_center(lbl_gal);
    lv_obj_add_event_cb(btn_gal, cb_open_gallery, LV_EVENT_CLICKED, NULL);

    lv_obj_t *view_desc = lv_label_create(card_view);
    lv_obj_set_style_text_font(view_desc, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(view_desc, lv_color_hex(0x888888), 0);
    lv_label_set_text(view_desc, "Vigilancia y capturas del carrusel");


    /* (La card "Trip computer" se movio al submenu Autocaravana:
     *  ver create_trip_card / populate_autocaravana.) */

    /* === Card 4: Backup/Restore configuracion === */
    lv_obj_t *card_bak = lv_obj_create(cont);
    lv_obj_set_width(card_bak, lv_pct(100));
    lv_obj_set_height(card_bak, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_bak, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_bak, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_bak, lv_color_hex(0x9C27B0), 0);
    lv_obj_set_style_border_width(card_bak, 1, 0);
    lv_obj_set_style_radius(card_bak, 12, 0);
    lv_obj_set_style_pad_all(card_bak, 16, 0);
    lv_obj_set_style_pad_gap(card_bak, 12, 0);
    lv_obj_set_layout(card_bak, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_bak, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bak_title = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bak_title, lv_color_hex(0x9C27B0), 0);
    lv_label_set_text(bak_title, LV_SYMBOL_SD_CARD "  Copia de seguridad de la configuracion");

    lv_obj_t *bak_desc = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_desc, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(bak_desc, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_width(bak_desc, lv_pct(100));
    /* WRAP (ancho fijo pct 100, no flex_grow -> sin riesgo WDT): las lineas
     * se reparten llenando todo el ancho de la card. */
    lv_label_set_long_mode(bak_desc, LV_LABEL_LONG_WRAP);
    lv_label_set_text(bak_desc,
        "Exporta toda la configuracion. El password WI-FI no se exporta por seguridad.");

    lv_obj_t *bak_row = lv_obj_create(card_bak);
    lv_obj_remove_style_all(bak_row);
    lv_obj_set_size(bak_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(bak_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bak_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bak_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_exp = lv_btn_create(bak_row);
    lv_obj_set_size(btn_exp, 200, 50);
    lv_obj_set_style_bg_color(btn_exp, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn_exp, 10, 0);
    lv_obj_t *lbl_exp = lv_label_create(btn_exp);
    lv_label_set_text(lbl_exp, LV_SYMBOL_UPLOAD "  Exportar");
    lv_obj_set_style_text_font(lbl_exp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_exp);

    lv_obj_t *btn_imp = lv_btn_create(bak_row);
    lv_obj_set_size(btn_imp, 200, 50);
    lv_obj_set_style_bg_color(btn_imp, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(btn_imp, 10, 0);
    lv_obj_t *lbl_imp = lv_label_create(btn_imp);
    lv_label_set_text(lbl_imp, LV_SYMBOL_DOWNLOAD "  Importar");
    lv_obj_set_style_text_font(lbl_imp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_imp);

    /* Status label */
    lv_obj_t *bak_status = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_status, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(bak_status, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text(bak_status, "");

    lv_obj_set_user_data(btn_exp, bak_status);
    lv_obj_set_user_data(btn_imp, bak_status);
    lv_obj_add_event_cb(btn_exp, backup_export_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(btn_imp, backup_import_cb, LV_EVENT_CLICKED, ui);

    /* El refresco en vivo (trip + espacio libre de la SD) lo lleva un timer
     * creado en ui_settings_panel_init (sirve a ambas cards estén donde estén). */
}



/* La pagina Victron Keys vive ahora en settings_victron_keys.c (se saco de aqui
 * para adelgazar este fichero). Estas son las funciones suyas que se usan desde
 * el resto de Ajustes. */
void create_victron_keys_settings_page(ui_state_t *ui, lv_obj_t *page_victron);
void victron_config_load(ui_state_t *ui);
void victron_config_refresh(ui_state_t *ui);
void victron_config_create_row(ui_state_t *ui, size_t index);
void victron_config_update_controls(ui_state_t *ui);
void victron_config_persist(ui_state_t *ui);
void victron_keys_clicked_cb(lv_event_t *e);

/* ── Cards del vehiculo, reubicadas al submenu Autocaravana ─────────────────
 * (antes vivian en las paginas Pantalla / Tarjeta SD). Se crean de forma
 * perezosa al abrir Autocaravana, bajo las entradas Frigo y Victron Keys. */
void create_autostart_card(lv_obj_t *cont)
{
    /* Auto-encendido de cargas al arranque: luz interior + bomba de agua via
     * NE185. Estado persistido en NVS. */
    lv_obj_t *card_auto = lv_obj_create(cont);
    lv_obj_set_width(card_auto, lv_pct(100));
    lv_obj_set_height(card_auto, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_auto, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_auto, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_auto, lv_color_hex(0xFFAA00), 0);  /* ambar */
    lv_obj_set_style_border_width(card_auto, 1, 0);
    lv_obj_set_style_radius(card_auto, 12, 0);
    lv_obj_set_style_pad_all(card_auto, 16, 0);
    lv_obj_set_style_pad_gap(card_auto, 10, 0);
    lv_obj_set_layout(card_auto, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_auto, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card_auto, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_title = lv_label_create(card_auto);
    lv_obj_set_style_text_font(auto_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(auto_title, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(auto_title, LV_SYMBOL_POWER "  Auto-encendido (luz + bomba)");

    lv_obj_t *auto_sw = lv_switch_create(card_auto);
    lv_obj_set_style_bg_color(auto_sw, lv_color_hex(0xFFAA00),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ne185_get_autostart()) lv_obj_add_state(auto_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(auto_sw, autostart_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_trip_card(lv_obj_t *cont)
{
    /* Trip computer: contadores reseteables del viaje. */
    lv_obj_t *card_trip = lv_obj_create(cont);
    lv_obj_set_width(card_trip, lv_pct(100));
    lv_obj_set_height(card_trip, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_trip, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_trip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_trip, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_style_border_width(card_trip, 1, 0);
    lv_obj_set_style_radius(card_trip, 12, 0);
    lv_obj_set_style_pad_hor(card_trip, 16, 0);
    lv_obj_set_style_pad_ver(card_trip, 8, 0);     /* menos alto: menos relleno arriba/abajo */
    lv_obj_set_style_pad_gap(card_trip, 4, 0);
    lv_obj_set_layout(card_trip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_trip, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *trip_head = lv_obj_create(card_trip);
    lv_obj_remove_style_all(trip_head);
    lv_obj_set_size(trip_head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_head, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trip_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *trip_title = lv_label_create(trip_head);
    lv_obj_set_style_text_font(trip_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(trip_title, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(trip_title, LV_SYMBOL_REFRESH "  Trip computer");

    /* Dos botones: empezar viaje (pone a cero) y finalizarlo (guarda y suelta
     * la tarjeta). Van juntos porque son las dos acciones del viaje. */
    lv_obj_t *trip_btns = lv_obj_create(trip_head);
    lv_obj_remove_style_all(trip_btns);
    lv_obj_set_size(trip_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(trip_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(trip_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(trip_btns, 8, 0);

    lv_obj_t *btn_trip_rst = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_rst, 140, 44);
    lv_obj_set_style_bg_color(btn_trip_rst, lv_color_hex(0x00897B), 0);
    lv_obj_set_style_radius(btn_trip_rst, 8, 0);
    lv_obj_t *lbl_trip_rst = lv_label_create(btn_trip_rst);
    lv_label_set_text(lbl_trip_rst, "Inicio");
    lv_obj_set_style_text_font(lbl_trip_rst, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_rst);
    lv_obj_add_event_cb(btn_trip_rst, trip_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_trip_fin = lv_btn_create(trip_btns);
    lv_obj_set_size(btn_trip_fin, 150, 44);
    lv_obj_set_style_bg_color(btn_trip_fin, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_trip_fin, 8, 0);
    lv_obj_t *lbl_trip_fin = lv_label_create(btn_trip_fin);
    lv_label_set_text(lbl_trip_fin, "Finalizar");
    lv_obj_set_style_text_font(lbl_trip_fin, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_trip_fin);
    lv_obj_add_event_cb(btn_trip_fin, trip_finish_btn_cb, LV_EVENT_CLICKED, NULL);

    s_trip_label = lv_label_create(card_trip);
    lv_obj_set_style_text_font(s_trip_label, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_trip_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(s_trip_label, lv_pct(100));
    /* LONG_DOT en vez de WRAP por riesgo WDT al construir. */
    lv_label_set_long_mode(s_trip_label, LV_LABEL_LONG_DOT);
    trip_label_refresh();
}

static void create_ausente_card(lv_obj_t *cont);  /* def mas abajo (usa ausente_switch_cb) */

void populate_autocaravana(settings_page_ctx_t *ctx, lv_obj_t *page)
{
    (void)ctx;
    /* Cards del vehiculo bajo las entradas "Opciones Frigo" y "Victron Keys"
     * (anadidas en el init). Orden: Modo ausente, Trip computer, Auto-encendido.
     * El timer que refresca el trip se crea en el init. */
    create_ausente_card(page);
    create_trip_card(page);
    create_autostart_card(page);
}

void ui_settings_panel_init(ui_state_t *ui,
                            const char *default_ssid,
                            const char *default_pass,
                            uint8_t ap_enabled)
{
    if (ui == NULL || ui->tab_settings == NULL) {
        return;
    }

    lv_obj_t *menu = lv_menu_create(ui->tab_settings);
    lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
    lv_obj_center(menu);
    /* Fondo coherente con el resto de pestanas */
    lv_obj_set_style_bg_color(menu, lv_color_black(), 0);
    lv_obj_set_style_bg_color(lv_menu_get_main_header(menu), lv_color_black(), 0);
    lv_obj_set_style_text_color(lv_menu_get_main_header(menu), lv_color_white(), 0);
    ui->settings_menu = menu;
    s_settings_menu = menu;  /* referencia static para diálogos modales */

    lv_obj_t *main_header = lv_menu_get_main_header(menu);
    lv_obj_set_style_text_font(main_header, &lv_font_montserrat_28_es, 0);
    lv_obj_set_flex_align(main_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Banda del header mas compacta (sin tocar boton Volver ni titulo): sube el
     * contenido de todas las paginas un poco hacia arriba. */
    lv_obj_set_style_pad_ver(main_header, 4, 0);
    s_settings_main_header = main_header;

    lv_obj_t *back_btn = lv_menu_get_main_header_back_btn(menu);
    s_settings_back_btn = back_btn;
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    /* Botón naranja cálido con sombra naranja exterior */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(back_btn, 2, 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_set_style_pad_hor(back_btn, 18, 0);
    lv_obj_set_style_pad_ver(back_btn, 10, 0);
    lv_obj_set_style_shadow_width(back_btn, 12, 0);
    lv_obj_set_style_shadow_color(back_btn, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_shadow_opa(back_btn, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(back_btn, 0, 0);
    /* Estado pulsado: naranja oscuro */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE65100), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(back_btn, lv_color_white(), 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  Volver");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    /* Spacer invisible para centrar el titulo via SPACE_BETWEEN */
    lv_obj_t *header_spacer = lv_obj_create(main_header);
    lv_obj_remove_style_all(header_spacer);
    lv_obj_set_size(header_spacer, 110, 1);

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    s_settings_main_page = main_page;
    lv_obj_t *page_frigo = lv_menu_page_create(menu, "FRIGO");
    ui->frigo_page = page_frigo;
    lv_obj_t *page_logs = lv_menu_page_create(menu, "HISTORIAL EN GRAFICOS");
    lv_obj_t *page_sound = lv_menu_page_create(menu, "SONIDO Y ALERTAS");
    lv_obj_t *page_wifi = lv_menu_page_create(menu, "WI-FI");

    lv_obj_t *page_display = lv_menu_page_create(menu, "PANTALLA");
    lv_obj_t *page_sd = lv_menu_page_create(menu, "TARJETA SD");
    lv_obj_t *page_victron = lv_menu_page_create(menu, "VICTRON KEYS");
    /* Submenu que agrupa las opciones de la autocaravana (Victron Keys y, mas
     * adelante, mas opciones del vehiculo). */
    lv_obj_t *page_autocaravana = lv_menu_page_create(menu, "AUTOCARAVANA");
    /* Sin LV_SYMBOL_LIST en el titulo del page: el header del menu usa
     * fuente Inter aliased que no tiene el glyph y se ve como rectangulo. */
    lv_obj_t *page_about = lv_menu_page_create(menu, "ACERCA DE Joint SPL 145 Control");
    
    /* Padding del main_page + layout 2 columnas */
    lv_obj_set_style_pad_all(main_page, 16, 0);
    lv_obj_set_style_pad_top(main_page, 4, 0);      /* menos hueco arriba: sube el contenido */
    lv_obj_set_style_pad_row(main_page, 12, 0);
    lv_obj_set_style_pad_column(main_page, 12, 0);
    lv_obj_set_layout(main_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Subpagina Autocaravana: mismo layout de 2 columnas que el menu principal. */
    lv_obj_set_style_pad_all(page_autocaravana, 16, 0);
    lv_obj_set_style_pad_top(page_autocaravana, 4, 0);
    lv_obj_set_style_pad_row(page_autocaravana, 12, 0);
    lv_obj_set_style_pad_column(page_autocaravana, 12, 0);
    lv_obj_set_layout(page_autocaravana, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page_autocaravana, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page_autocaravana, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Frigo: eager (es la pagina mas usada). populate=NULL para que el lazy
     * dispatch no haga nada; ui_frigo_panel_init mas abajo construye el
     * contenido. */
    settings_menu_add_entry(ui, page_autocaravana, menu, page_frigo,
        "Opciones Frigo", "Sensores, ventilador y umbrales",
        LV_SYMBOL_REFRESH,    0x00C851, NULL);
    settings_menu_add_entry(ui, main_page, menu, page_logs,
        "Historial en graficos", "Histórico SD: batería, nevera y placa solar",
        LV_SYMBOL_SAVE,       0x9C27B0, populate_logs);
    settings_page_ctx_t *ctx_wifi = settings_menu_add_entry(
        ui, main_page, menu, page_wifi,
        "Wi-Fi",         "Modo AP y credenciales",
        LV_SYMBOL_WIFI,       0xFF9100, populate_wifi);
    if (ctx_wifi) {
        if (default_ssid) {
            strncpy(ctx_wifi->wifi_ssid, default_ssid, sizeof(ctx_wifi->wifi_ssid) - 1);
        }
        if (default_pass) {
            strncpy(ctx_wifi->wifi_pass, default_pass, sizeof(ctx_wifi->wifi_pass) - 1);
        }
        ctx_wifi->wifi_ap_enabled = ap_enabled;
    }
    settings_menu_add_entry(ui, main_page, menu, page_display,
        "Pantalla",      "Brillo, salvapantallas, modo noche",
        LV_SYMBOL_EYE_OPEN,   0x00BFA5, populate_display);
    settings_menu_add_entry(ui, main_page, menu, page_sd,
        "Tarjeta SD",    "Carrusel de capturas y visor de imagenes",
        LV_SYMBOL_SD_CARD,    0x2979FF, populate_sd);
    settings_menu_add_entry(ui, main_page, menu, page_sound,
        "Sonido y alertas","Volumen, jingles y alertas",
        LV_SYMBOL_VOLUME_MAX, 0xFF1744, populate_sound);
    /* Autocaravana: submenu que agrupa las opciones del vehiculo. Por ahora solo
     * contiene "Victron Keys"; se iran anadiendo mas entradas dentro. */
    settings_menu_add_entry(ui, main_page, menu, page_autocaravana,
        "Autocaravana",  "Frigo, Victron, cargas y viaje",
        LV_SYMBOL_HOME,       0xEC407A, populate_autocaravana);
    /* Victron Keys ahora vive DENTRO de la subpagina Autocaravana */
    settings_menu_add_entry(ui, page_autocaravana, menu, page_victron,
        "Victron Keys",  "Dispositivos BLE y claves",
        LV_SYMBOL_GPS,        0xEC407A, populate_keys);
    /* Mostrar warning al entrar en Victron Keys (card ahora en la subpagina) */
    {
        lv_obj_t *cont_vk = lv_obj_get_child(page_autocaravana,
                                             lv_obj_get_child_cnt(page_autocaravana) - 1);
        if (cont_vk) {
            lv_obj_add_event_cb(cont_vk, victron_keys_clicked_cb, LV_EVENT_CLICKED, ui);
        }
    }
    /* Cards de entrada de Autocaravana un poco menos negras que las del menu
     * principal (estilo compartido = 0x1A1A1A), para distinguir el submenu. */
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(page_autocaravana); i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(page_autocaravana, i),
                                  lv_color_hex(0x2E2E38), 0);
    }
    settings_menu_add_entry(ui, main_page, menu, page_about,
        "Acerca de",     "Sistema, uptime, IP y reinicio",
        LV_SYMBOL_LIST,       0x90A4AE, populate_about);


    lv_menu_set_page(menu, main_page);
    /* Recolorea header + back btn al cambiar de pagina segun acento de seccion
     * y dispara el lazy populate de la pagina destino la primera vez. */
    lv_obj_add_event_cb(menu, settings_menu_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Frigo eager (lazy dispatch skipped via populate=NULL). El resto se
     * construye al navegar por primera vez. */
    ui_frigo_panel_init(ui);

    /* Timer 1 s: refresca el Trip computer y el espacio libre de la SD (cada
     * card solo cuando esta visible). Antes se creaba al abrir la pagina SD. */
    lv_timer_create(sd_trip_timer_cb, 1000, ui);

    lv_obj_t *tab = ui->tab_settings;

    // remove default padding and layout effects
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_style_pad_row(tab, 0, 0);
    lv_obj_set_style_pad_column(tab, 0, 0);
    lv_obj_set_style_border_width(tab, 0, 0);

    // also ensure the menu expands fully
    lv_obj_set_size(menu, lv_pct(100), lv_pct(100));
    lv_obj_align(menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

}

void ui_settings_panel_on_user_activity(ui_state_t *ui)
{
    screensaver_wake(ui);
}

void ui_settings_panel_set_mac(ui_state_t *ui, const char *mac_str)
{
    if (ui == NULL || ui->ta_mac == NULL || mac_str == NULL) {
        return;
    }
    lv_textarea_set_text(ui->ta_mac, mac_str);
}

void ui_settings_panel_update_victron_device_status(ui_state_t *ui, const char *mac_address, 
                                                     const char *device_type, const char *product_name, 
                                                     const char *error_info)
{
    if (ui == NULL) {
        return;
    }
    victron_config_update_device_status(ui, mac_address, device_type, product_name, error_info);
}

void ta_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (ui == NULL || ui->keyboard == NULL) {
        return;
    }

    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(ui->keyboard, ta);
        lv_obj_move_foreground(ui->keyboard);
        lv_obj_clear_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_coord_t kb_height = lv_obj_get_height(ui->keyboard);
        lv_coord_t available_h = screen_h - kb_height;
        if (available_h < screen_h / 3) {
            available_h = screen_h / 3;
        }
        lv_obj_update_layout(ui->tabview);
        lv_obj_set_height(ui->tabview, available_h);
        lv_obj_update_layout(ui->tabview);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        if (ta == NULL) {
            return;
        }
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        lv_keyboard_set_textarea(ui->keyboard, NULL);
        lv_obj_add_flag(ui->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_disp_t *disp = lv_disp_get_default();
        lv_coord_t screen_h = disp ? lv_disp_get_ver_res(disp) : LV_VER_RES;
        lv_obj_set_height(ui->tabview, screen_h);
        lv_obj_update_layout(ui->tabview);
        lv_indev_reset(NULL, ta);
    }
}




/* ── Callbacks Modo nocturno ───────────────────────────────────── */
void night_save_and_apply(ui_state_t *ui)
{
    save_night_mode(ui->night_mode.enabled,
                    ui->night_mode.start_h,
                    ui->night_mode.end_h);
    /* Si se acaba de activar el modo nocturno con el salvapantallas apagado, el
     * timer de reposo debe arrancar igualmente (y pararse si ya no hace falta). */
    screensaver_sync_timer_state(ui);
    apply_brightness_for_now(ui);
}









/* Periodo del timer de reposo en ms. Con timeout 0 devuelve 0xFFFFFFFF (~49
 * dias, "nunca") en vez de 0: un lv_timer con periodo 0 dispara en CADA frame,
 * lo que de noche apagaria la pantalla en bucle sin dejarla despertar. */
static uint32_t ss_period_ms(ui_state_t *ui)
{
    return ui->screensaver.timeout > 0 ? ui->screensaver.timeout * 1000U
                                       : 0xFFFFFFFF;
}

/* Aplica el cambio de timeout: persiste en NVS y reprograma el timer */
void ss_timeout_apply(ui_state_t *ui)
{
    if (ui->screensaver.spinbox_timeout) {
        lv_label_set_text_fmt(ui->screensaver.spinbox_timeout,
                              "%d", ui->screensaver.timeout / 60);
    }
    save_screensaver_settings(ui->screensaver.enabled,
                              ui->screensaver.brightness,
                              ui->screensaver.timeout);
    if (ui->screensaver.timer) {
        lv_timer_set_period(ui->screensaver.timer, ss_period_ms(ui));
    }
}



/* Arranca o pausa el timer de reposo segun haga falta (salvapantallas O modo
 * nocturno activados). Solo toca el timer, NUNCA el brillo, asi que es seguro
 * llamarlo en boot sin provocar parpadeos. */
static void screensaver_sync_timer_state(ui_state_t *ui)
{
    if (!ui || !ui->screensaver.timer) return;
    if (ui->screensaver.enabled || ui->night_mode.enabled) {
        lv_timer_set_period(ui->screensaver.timer, ss_period_ms(ui));
        lv_timer_reset(ui->screensaver.timer);
        lv_timer_resume(ui->screensaver.timer);
    } else {
        lv_timer_pause(ui->screensaver.timer);
    }
}

void ui_settings_screensaver_create_timer(ui_state_t *ui)
{
    if (!ui || ui->screensaver.timer) return;
    /* Crear timer y arrancarlo/pausarlo segun estado. No usamos
     * screensaver_enable() aqui porque tocaba el brillo del sistema en boot ->
     * aparecia un parpadeo (80% -> ui->brightness -> 80%) antes de mostrar el
     * splash. El brillo se aplica una sola vez al final del boot via
     * night_mode_timer_cb. screensaver_sync_timer_state NO toca el brillo. */
    ui->screensaver.timer = lv_timer_create(screensaver_timer_cb,
                                             ss_period_ms(ui), ui);
    ui->screensaver.active = false;
    screensaver_sync_timer_state(ui);
}

void screensaver_enable(ui_state_t *ui, bool enable)
{
    if (ui == NULL || ui->screensaver.timer == NULL) {
        return;
    }
    if (enable) {
        ui->screensaver.active = false;
        bsp_display_brightness_set(ui->brightness);
    } else if (ui->screensaver.active && !night_active_now(ui)) {
        /* Al desactivar el salvapantallas restauramos el brillo SOLO si no
         * estamos en franja nocturna: de noche el timer sigue corriendo (lo
         * mantiene screensaver_sync_timer_state) para apagar en reposo, y el
         * toque despierta igual. */
        bsp_display_brightness_set(ui->brightness);
        ui->screensaver.active = false;
    }
    screensaver_sync_timer_state(ui);
}

/* Forward declarations para rotacion */
extern void ui_show_battery_history_screen(ui_state_t *ui);
extern void ui_show_solar_history_screen(ui_state_t *ui);
extern void ui_show_chart_screen(ui_state_t *ui);

extern void ui_close_chart_screen(void);
extern void ui_close_battery_history_screen(void);

static void screensaver_rotate_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : NULL;
    if (!ui) return;
    int prev_idx = ui->screensaver.rotate_index;
    int next_idx = (prev_idx + 1) % 3;
    ESP_LOGI("SAVER", "rotate fired idx=%d->%d", prev_idx, next_idx);

    /* 3 vistas: 0=Live (tab del tabview), 1=LogFrigo (overlay chart),
     * 2=LogBateria (pantalla independiente).
     * Cerrar SOLO la vista actual antes de abrir la siguiente, para que la
     * transición sea limpia (Frigo→Bateria cierra Frigo; Bateria→Live cierra Bateria). */
    switch (prev_idx) {
        case 1:
            ui_close_chart_screen();
            break;
        case 2:
            ui_close_battery_history_screen();
            break;
        default:
            break;  /* 0 = Live: no hay overlay que cerrar */
    }

    ui->screensaver.rotate_index = next_idx;
    switch (next_idx) {
        case 0:
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            break;
        case 1:
            ui_show_chart_screen(ui);
            break;
        case 2:
            ui_show_battery_history_screen(ui);
            break;
    }
}

static void screensaver_timer_cb(lv_timer_t *timer)
{
    ui_state_t *ui = timer ? (ui_state_t *)timer->user_data : NULL;
    if (ui == NULL) {
        return;
    }
    /* El modo ausente ya gestiona la pantalla (apagada): no activar screensaver. */
    if (ausente_is_active()) return;
    if (ui->screensaver.active) return;

    /* Franja nocturna: pantalla APAGADA en reposo (sin rotar). Ocurre AUNQUE el
     * salvapantallas este desactivado (el timer sigue corriendo si el modo
     * nocturno esta activo). El toque la enciende a brillo normal via
     * screensaver_wake y, tras el timeout, vuelve aqui a apagarse. */
    if (night_active_now(ui)) {
        /* active=true ANTES de bajar el brillo: night_mode_timer_cb (esp_timer,
         * otro core) lee 'active' sin mutex; si lo viera false en la ventana
         * entre ambas sentencias, reencenderia la pantalla toda la noche. */
        ui->screensaver.active = true;
        bsp_display_brightness_set(0);
        return;
    }

    /* Fuera de la franja nocturna, solo el salvapantallas (si esta activado). */
    if (!ui->screensaver.enabled) return;

    if (ui->screensaver.mode == UI_SCREENSAVER_MODE_ROTATE) {
        /* No entrar en rotacion si hay una alarma activa: hay que mantener
         * Live+Overview visible hasta que se despeje (Feature A). El timer es
         * periodico, asi que reintentara cuando la alarma se aclare. */
        if (ui_overview_alarm_active()) return;
        /* Modo Rotar: ciclar Live/Frigo/Bateria cada rotate_period_min via
         * screensaver_rotate_timer_cb. La fragmentacion del pool LVGL de
         * 128KB que obligo a desactivarlo ya NO aplica: con LV_MEM_CUSTOM=y
         * LVGL asigna del heap del sistema (verificado, 15 ciclos estables). */
        ui->screensaver.active = true;
        ui->screensaver.rotate_index = 0;
        if (ui->tabview && lv_tabview_get_tab_act(ui->tabview) != 0) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
        }
        uint8_t per_min = ui->screensaver.rotate_period_min ?
                          ui->screensaver.rotate_period_min : 1;
        if (!ui->screensaver.rotate_timer) {
            ui->screensaver.rotate_timer =
                lv_timer_create(screensaver_rotate_timer_cb, per_min * 60000U, ui);
        }
        ESP_LOGI("SAVER", "Rotar activo (periodo %u min)", per_min);
    } else {
        /* Modo atenuar (default) */
        bsp_display_brightness_set(ui->screensaver.brightness > ui->brightness ? ui->brightness : ui->screensaver.brightness);
        ui->screensaver.active = true;
    }
}

static void screensaver_wake(ui_state_t *ui)
{
    if (ui == NULL || ui->screensaver.timer == NULL) {
        return;
    }
    /* En modo ausente el toque normal NO despierta la pantalla. */
    if (ausente_is_active()) return;
    /* El timer de reposo puede estar corriendo por el salvapantallas O por el
     * modo nocturno: en ambos casos el toque resetea el reloj de inactividad. */
    if (ui->screensaver.enabled || ui->night_mode.enabled) {
        lv_timer_reset(ui->screensaver.timer);
    }
    /* Si la pantalla esta apagada/atenuada por reposo, el toque la restaura,
     * venga del salvapantallas o del modo nocturno (active solo lo pone el
     * timer de reposo, asi que restaurar aqui siempre es correcto). */
    if (ui->screensaver.active) {
        {
            bool was_rotating = (ui->screensaver.rotate_timer != NULL);
            bsp_display_brightness_set(ui->brightness);
            ui->screensaver.active = false;
            /* Parar timer de rotacion si estaba activo */
            if (ui->screensaver.rotate_timer) {
                lv_timer_del(ui->screensaver.rotate_timer);
                ui->screensaver.rotate_timer = NULL;
            }
            /* En modo rotación: al despertar, cerrar TODOS los overlays
             * (chart frigo, histórico batería) y volver a la pestaña Live. */
            if (was_rotating) {
                ui_close_chart_screen();
                ui_close_battery_history_screen();
                if (ui->tabview &&
                    lv_tabview_get_tab_act(ui->tabview) != 0) {
                    lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
                }
            }
            ui->screensaver.rotate_index = 0;
        }
    }
}




static int victron_config_find_device_by_mac(ui_state_t *ui, const char *mac_address)
{
    if (ui == NULL || mac_address == NULL) {
        return -1;
    }

    for (size_t i = 0; i < ui->victron_config.count; ++i) {
        if (ui->victron_config.mac_textareas[i] != NULL) {
            const char *configured_mac = lv_textarea_get_text(ui->victron_config.mac_textareas[i]);
            if (configured_mac != NULL && strcmp(configured_mac, mac_address) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void victron_config_update_device_status(ui_state_t *ui, const char *mac_address, 
                                                const char *device_type, const char *product_name, 
                                                const char *error_info)
{
    if (ui == NULL) {
        return;
    }

    int device_index = victron_config_find_device_by_mac(ui, mac_address);
    if (device_index < 0) {
        return;  // Device not found in configuration
    }

    size_t index = (size_t)device_index;

    /* Update device type */
    if (ui->victron_config.device_type_labels[index]) {
        if (device_type && device_type[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.device_type_labels[index], "Device: %s", device_type);
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], lv_color_hex(0x00C851), 0); // Green for active
        } else {
            lv_label_set_text(ui->victron_config.device_type_labels[index], "Device: --");
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], lv_color_hex(0x888888), 0); // Gray for inactive
        }
    }

    /* Update product name */
    if (ui->victron_config.product_name_labels[index]) {
        if (product_name && product_name[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.product_name_labels[index], "Product: %s", product_name);
            lv_obj_set_style_text_color(ui->victron_config.product_name_labels[index], lv_color_hex(0x00C851), 0); // Green for active
        } else {
            lv_label_set_text(ui->victron_config.product_name_labels[index], "Product: --");
            lv_obj_set_style_text_color(ui->victron_config.product_name_labels[index], lv_color_hex(0x888888), 0); // Gray for inactive
        }
    }

    /* Update error/status info */
    if (ui->victron_config.error_labels[index]) {
        if (error_info && error_info[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.error_labels[index], "Status: %s", error_info);
            /* Color code based on content */
            if (strstr(error_info, "error") || strstr(error_info, "Error") || strstr(error_info, "ERROR")) {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0xF44336), 0); // Red for errors
            } else if (strstr(error_info, "Active") || strstr(error_info, "OK") || strstr(error_info, "Connected")) {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0x00C851), 0); // Green for OK
            } else {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0xFF9800), 0); // Orange for warnings
            }
        } else {
            lv_label_set_text(ui->victron_config.error_labels[index], "Status: No data");
            lv_obj_set_style_text_color(ui->victron_config.error_labels[index], lv_color_hex(0x888888), 0); // Gray for no data
        }
    }
}

void ui_settings_panel_refresh_victron_devices(ui_state_t *ui)
{
    if (ui == NULL) {
        return;
    }
    
    ESP_LOGI(TAG_SETTINGS, "Public function called to refresh Victron device list");
    victron_config_refresh(ui);
}

/* ── Trip computer: refresco periodico y reset ─────────────────── */
static void trip_label_refresh(void)
{
    if (!s_trip_label) return;
    trip_computer_t t;
    trip_computer_get(&t);
    char start_str[24] = "--";
    if (t.reset_epoch > 0) {
        struct tm tm_l;
        localtime_r((time_t *)&t.reset_epoch, &tm_l);
        strftime(start_str, sizeof(start_str), "%d/%m %H:%M", &tm_l);
    }
    /* Tiempo de viaje = reloj transcurrido desde el ultimo "Nuevo viaje"
     * (siempre avanza, aunque no llegue telemetria del BMV). */
    int64_t elapsed = 0;
    if (t.reset_epoch > 0) {
        time_t now = time(NULL);
        if (now >= (time_t)t.reset_epoch) elapsed = (int64_t)now - t.reset_epoch;
    }
    /* Los viajes duran dias: "130h 18m" no se lee de un vistazo. Los dias solo
     * aparecen cuando los hay, para que un viaje recien empezado no ensene
     * un "0d" que no aporta nada. */
    int e_days  = (int)(elapsed / 86400);
    int hours   = (int)((elapsed % 86400) / 3600);
    int minutes = (int)((elapsed % 3600) / 60);
    char elapsed_str[24];
    if (e_days > 0) {
        snprintf(elapsed_str, sizeof(elapsed_str), "%dd %dh %02dm",
                 e_days, hours, minutes);
    } else {
        snprintf(elapsed_str, sizeof(elapsed_str), "%dh %02dm", hours, minutes);
    }

    /* Medias solares por dia (proyeccion: divide por los dias exactos
     * transcurridos, aunque sean horas). Guardamos contra division por cero. */
    double days = (elapsed > 0) ? (double)elapsed / 86400.0 : 0.0;
    double solar_h_day  = (days > 0.0) ? (t.solar_seconds / 3600.0) / days : 0.0;
    double solar_ah_day = (days > 0.0) ? t.ah_solar / days : 0.0;

    char buf[288];
    snprintf(buf, sizeof(buf),
        "Iniciado %s   |   %s\n"
        "Total cargado: %.2f kWh %.1f Ah  "
        "(Solar: %.2f kWh %.1f Ah, %.1f h/dia, %.0f Ah/dia)\n"
        "Consumido: %.2f kWh  %.1f Ah",
        start_str, elapsed_str,
        t.wh_charged / 1000.0, t.ah_charged,
        t.wh_solar / 1000.0, t.ah_solar, solar_h_day, solar_ah_day,
        t.wh_discharged / 1000.0, t.ah_discharged);
    lv_label_set_text(s_trip_label, buf);
}

/* === Dialogo de confirmacion con el estilo del aviso de Victron Keys =======
 * Modal grande (600x280), fondo oscurecido, borde y titulo rosa, texto grande
 * centrado y botones 220x60. Un unico helper para TODAS las confirmaciones, asi
 * quedan identicas en estilo y tamano. La accion (on_confirm) se ejecuta solo si
 * se pulsa el boton derecho; el modal se cierra siempre. */
typedef void (*ui_confirm_action_t)(void);
static void ui_show_confirm_dialog(const char *title, const char *msg,
                                   const char *ok_txt, ui_confirm_action_t on_confirm);
static lv_obj_t *s_confirm_modal = NULL;
static ui_confirm_action_t s_confirm_action = NULL;

static void ui_confirm_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    bool ok = (txt && strcmp(txt, "Cancelar") != 0);  /* el izquierdo siempre es Cancelar */
    ui_confirm_action_t action = s_confirm_action;
    if (s_confirm_modal) { lv_obj_del(s_confirm_modal); s_confirm_modal = NULL; }
    s_confirm_action = NULL;
    if (ok && action) action();
}

/* Aviso de solo lectura: el mismo modal pero con un unico boton y sin accion.
 * Se usa al finalizar viaje para decir que ya se puede sacar la tarjeta. */
static void ui_show_info_dialog(const char *title, const char *msg)
{
    ui_show_confirm_dialog(title, msg, "Entendido", NULL);
}

static void ui_show_confirm_dialog(const char *title, const char *msg,
                                   const char *ok_txt, ui_confirm_action_t on_confirm)
{
    if (s_confirm_modal) return;
    s_confirm_action = on_confirm;

    /* Fondo modal a pantalla completa (mismo que el aviso de Victron Keys) */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_confirm_modal = modal;

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

    lv_obj_t *title_lbl = lv_label_create(dlg);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xE91E63), 0);
    lv_label_set_text(title_lbl, title);

    lv_obj_t *msg_lbl = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl, lv_pct(100));
    lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg_lbl, msg);

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
    lv_obj_add_event_cb(btn_cancel, ui_confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_btn_create(row_btns);
    lv_obj_set_size(btn_ok, 220, 60);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xE91E63), 0);
    lv_obj_set_style_radius(btn_ok, 12, 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, ok_txt);
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, ui_confirm_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void do_trip_reset_action(void)
{
    trip_computer_reset();
    trip_label_refresh();
}

static void trip_reset_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_WARNING "  Trip computer",
        "Empezar un viaje nuevo?\nLos contadores vuelven a cero.",
        "Empezar", do_trip_reset_action);
}

/* Cierre de viaje: se vuelca a la tarjeta TODO lo que hay pendiente en memoria y
 * se desmonta, para poder sacarla sin corromper nada. Despues ya no se escribe
 * mas hasta reiniciar; el aviso final lo deja claro. */
static void do_trip_finish_action(void)
{
    ESP_LOGI(TAG_SETTINGS, "Finalizar viaje: volcando todo a la tarjeta");
    battery_history_flush();     /* historico de corriente/tension/panel */
    solar_daily_flush();         /* dia de produccion en curso */
    ne185_vlog_flush();          /* comparativa de voltaje NE185 (hasta 10 min en RAM) */
    trip_computer_end();         /* guarda los contadores Y cierra el viaje */

    const esp_err_t err = datalogger_close_sd();   /* incluye su propio flush */
    if (err == ESP_OK) {
        ui_show_info_dialog(LV_SYMBOL_SD_CARD "  Viaje finalizado",
            "Todo guardado.\n\nYa puedes sacar la tarjeta.\n\n"
            "Para volver a registrar, reinicia la pantalla.");
    } else {
        ui_show_info_dialog(LV_SYMBOL_WARNING "  Viaje finalizado",
            "Se ha guardado todo lo pendiente, pero la tarjeta\n"
            "no se ha podido soltar (puede estar ocupada).\n\n"
            "Espera unos segundos y vuelve a intentarlo.");
    }
}

static void trip_finish_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_SD_CARD "  Finalizar viaje",
        "Se guarda todo y se suelta la tarjeta\npara poder sacarla.\n\n"
        "Despues no se registra nada mas\nhasta reiniciar la pantalla.",
        "Finalizar", do_trip_finish_action);
}

/* ── Aviso de arranque "Nuevo viaje?" ─────────────────────────────
 * Misma estetica que ui_show_confirm_dialog, pero con dos botones
 * explicitos: "Seguir viaje" (no toca nada) y "Nuevo viaje" (resetea).
 * No reutiliza ui_show_confirm_dialog porque ese detecta el boton por el
 * texto "Cancelar"; aqui queremos etiquetas propias. */
static lv_obj_t *s_newtrip_modal = NULL;

static void newtrip_close(void)
{
    if (s_newtrip_modal) { lv_obj_del(s_newtrip_modal); s_newtrip_modal = NULL; }
}

static void newtrip_keep_cb(lv_event_t *e)
{
    (void)e;
    /* "Seguir viaje" tambien abre el viaje: si no, el aviso volveria a salir en
     * el siguiente arranque preguntando lo mismo. */
    trip_computer_mark_active();
    newtrip_close();
}

static void newtrip_reset_cb(lv_event_t *e)
{
    (void)e;
    trip_computer_reset();
    trip_label_refresh();   /* seguro aunque la card aun no exista (chequea s_trip_label) */
    newtrip_close();
}

void ui_show_new_trip_dialog(void)
{
    if (s_newtrip_modal) return;

    /* Fondo modal a pantalla completa (identico al resto de dialogos). */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_newtrip_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0x00C851), 0);  /* verde: viaje */
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(title, LV_SYMBOL_REFRESH "  Nuevo viaje");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Empezar un viaje nuevo?\nSe pondran a cero los contadores del viaje.");

    lv_obj_t *row_btns = lv_obj_create(dlg);
    lv_obj_remove_style_all(row_btns);
    lv_obj_set_size(row_btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row_btns, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_keep = lv_btn_create(row_btns);
    lv_obj_set_size(btn_keep, 240, 60);
    lv_obj_set_style_bg_color(btn_keep, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_keep, 12, 0);
    lv_obj_t *lk = lv_label_create(btn_keep);
    lv_label_set_text(lk, "Seguir viaje");
    lv_obj_set_style_text_font(lk, &lv_font_montserrat_24_es, 0);
    lv_obj_center(lk);
    lv_obj_add_event_cb(btn_keep, newtrip_keep_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_new = lv_btn_create(row_btns);
    lv_obj_set_size(btn_new, 240, 60);
    lv_obj_set_style_bg_color(btn_new, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_radius(btn_new, 12, 0);
    lv_obj_t *ln = lv_label_create(btn_new);
    lv_label_set_text(ln, LV_SYMBOL_REFRESH "  Nuevo viaje");
    lv_obj_set_style_text_font(ln, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(ln, lv_color_hex(0x0A0A0A), 0);  /* texto oscuro sobre verde */
    lv_obj_center(ln);
    lv_obj_add_event_cb(btn_new, newtrip_reset_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Callbacks Backup/Restore configuración ───────────────────── */
static void backup_export_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *status = (lv_obj_t *)lv_obj_get_user_data(btn);
    esp_err_t err = config_backup_export(CONFIG_BACKUP_PATH);
    if (status) {
        lv_label_set_text(status, err == ESP_OK
            ? "Exportado a " CONFIG_BACKUP_PATH
            : "ERROR exportando (¿SD montada?)");
        lv_obj_set_style_text_color(status,
            err == ESP_OK ? lv_color_hex(0x00C851) : lv_color_hex(0xCC3333), 0);
    }
}

static void backup_import_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *status = (lv_obj_t *)lv_obj_get_user_data(btn);
    esp_err_t err = config_backup_import(CONFIG_BACKUP_PATH);
    if (status) {
        lv_label_set_text(status, err == ESP_OK
            ? "Importado. Reinicia para aplicar todos los cambios."
            : "ERROR importando (¿fichero existe?)");
        lv_obj_set_style_text_color(status,
            err == ESP_OK ? lv_color_hex(0x00C851) : lv_color_hex(0xCC3333), 0);
    }
}

static void create_about_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    style_settings_scrollbar(page);
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(cont, 16, 0);
    lv_obj_set_style_pad_all(cont, 16, 0);


    /* === Card 2: Info dinamica === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 10, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_REFRESH "  Estado");

    ui->lbl_about_uptime = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_uptime, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_uptime, "Uptime: --");

    /* RAM libre (la info de SD se movio a la pagina Tarjeta SD) */
    ui->lbl_about_heap = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_heap, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_heap, "RAM libre: --");

    ui->lbl_about_ip = lv_label_create(card2);
    lv_obj_set_style_text_font(ui->lbl_about_ip, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(ui->lbl_about_ip, "IP AP: --");

    /* Diagnostico de salud: causa del ultimo reset + total de resets WDT/panic */
    lv_obj_t *lbl_wd = lv_label_create(card2);
    lv_obj_set_style_text_font(lbl_wd, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_wd, lv_color_hex(0xFFD54F), 0);
    lv_label_set_text_fmt(lbl_wd, "Ultimo reset: %s   |   Resets WDT/panic: %lu",
                          watchdog_last_reset_reason(),
                          (unsigned long)watchdog_get_reset_count());

    /* === Card 3: Credits === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 6, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_COLUMN);

    /* Header row: titulo a la izquierda, boton reiniciar a la derecha */
    lv_obj_t *card3_header = lv_obj_create(card3);
    lv_obj_remove_style_all(card3_header);
    lv_obj_set_size(card3_header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card3_header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3_header);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_LIST "  Version, Repo y Creditos");

    /* Boton Reiniciar pequeno en la esquina */
    lv_obj_t *btn_reboot_hdr = lv_btn_create(card3_header);
    lv_obj_set_size(btn_reboot_hdr, 130, 40);
    lv_obj_set_style_bg_color(btn_reboot_hdr, lv_color_hex(0xCC3333), 0);
    lv_obj_set_style_radius(btn_reboot_hdr, 8, 0);
    lv_obj_t *lbl_reboot_hdr = lv_label_create(btn_reboot_hdr);
    lv_label_set_text(lbl_reboot_hdr, LV_SYMBOL_POWER "  Reiniciar");
    lv_obj_set_style_text_font(lbl_reboot_hdr, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_reboot_hdr);
    lv_obj_add_event_cb(btn_reboot_hdr, reboot_btn_cb, LV_EVENT_CLICKED, ui);

    /* Version + fecha/hora de compilacion, todo en una linea. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *raw_ver = app_desc ? app_desc->version : APP_VERSION_FALLBACK;
    /* git describe da "v1.0.0" en un tag exacto, o "v1.0.0-<n>-g<hash>[-dirty]"
     * en un build intermedio. Mostramos el tag base y, si hay sufijo (no es un
     * release limpio), "-dev" -> "v1.0.0-dev" en vez del hash feo. */
    char ver_disp[48];
    const char *dash = strchr(raw_ver, '-');
    if (dash) snprintf(ver_disp, sizeof(ver_disp), "%.*s-dev",
                       (int)(dash - raw_ver), raw_ver);
    else      snprintf(ver_disp, sizeof(ver_disp), "%s", raw_ver);
    lv_obj_t *lbl_ver_top = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_ver_top, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_ver_top, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text_fmt(lbl_ver_top, "Version: %s    Compilado: %s  %s",
                          ver_disp,
                          app_desc ? app_desc->date : __DATE__,
                          app_desc ? app_desc->time : __TIME__);

    /* Chip + IDF */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    lv_obj_t *lbl_chip = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_chip, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_chip, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text_fmt(lbl_chip, "ESP32 model=%d cores=%d rev=%d  |  IDF: %s",
        chip.model, chip.cores, chip.revision, esp_get_idf_version());

    lv_obj_t *lbl_port = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_port, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_port, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(lbl_port, "Port para Guition JC1060P470C_I por Ehuntabi");

    lv_obj_t *lbl_gh = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_gh, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_gh, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(lbl_gh, "github.com/Ehuntabi/victron-jc1060p470c-esp32p4");

    lv_obj_t *lbl_cred = lv_label_create(card3);
    lv_obj_set_style_text_font(lbl_cred, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_cred, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_cred, "Basado en: CamdenSutherland, wytr");

    /* Refrescar y crear timer */
    about_refresh_dynamic(ui);
    lv_timer_create(about_timer_cb, 1000, ui);

}



static void settings_btn_styles_init(void)
{
    if (s_settings_styles_inited) return;
    /* Cuerpo neutro: el color del rol sale a traves de la barra lateral y el
     * icono coloreado, no del fondo de la card. */
    lv_style_init(&s_settings_btn_style);
    lv_style_set_bg_opa(&s_settings_btn_style, LV_OPA_COVER);
    lv_style_set_bg_color(&s_settings_btn_style, lv_color_hex(0x1A1A1A));
    lv_style_set_border_color(&s_settings_btn_style, lv_color_hex(0x2A2A2A));
    lv_style_set_border_width(&s_settings_btn_style, 1);
    lv_style_set_radius(&s_settings_btn_style, 12);
    lv_style_set_pad_all(&s_settings_btn_style, 0);
    lv_style_set_pad_column(&s_settings_btn_style, 12);
    lv_style_set_min_height(&s_settings_btn_style, 88);

    /* Pressed: simplemente un poco mas claro, sin pisar el color de rol. */
    lv_style_init(&s_settings_btn_pressed_style);
    lv_style_set_bg_color(&s_settings_btn_pressed_style, lv_color_hex(0x2D2D2D));
    s_settings_styles_inited = true;
}

/* Construye el "look" de la card de Settings (barra de acento + icono +
 * titulo + subtitulo) dentro del contenedor pasado. Lo usan tanto las
 * entradas del menu principal como los botones de subpaginas que comparten
 * la misma estetica. */
static void settings_card_decor(lv_obj_t *cont, const char *title,
                                const char *subtitle, const char *icon,
                                uint32_t accent)
{
    lv_obj_add_style(cont, &s_settings_btn_style, 0);
    lv_obj_add_style(cont, &s_settings_btn_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Barra vertical de acento (4 px, altura completa). */
    lv_obj_t *bar = lv_obj_create(cont);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);  /* deja pasar el click al cont */
    lv_obj_set_size(bar, 4, LV_PCT(100));
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);

    /* Icono grande coloreado con el acento. Montserrat built-in (no _es)
     * porque Inter aliased no tiene los LV_SYMBOL_* y se ven como rectangulo
     * (LIST en About, SAVE en Logs, WIFI, GPS, EYE_OPEN, VOLUME_MAX, etc). */
    lv_obj_t *ico = lv_label_create(cont);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ico, lv_color_hex(accent), 0);
    lv_obj_set_style_pad_left(ico, 12, 0);

    /* Columna de texto: titulo en blanco + subtitulo en gris medio. */
    lv_obj_t *txt = lv_obj_create(cont);
    lv_obj_remove_style_all(txt);
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_right(txt, 12, 0);
    lv_obj_set_style_pad_row(txt, 4, 0);

    lv_obj_t *t1 = lv_label_create(txt);
    lv_label_set_text(t1, title);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(t1, lv_color_white(), 0);

    if (subtitle && *subtitle) {
        lv_obj_t *t2 = lv_label_create(txt);
        lv_label_set_text(t2, subtitle);
        lv_obj_set_style_text_font(t2, &lv_font_montserrat_14_es, 0);
        lv_obj_set_style_text_color(t2, lv_color_hex(0x8A93A6), 0);
    }
}

/* ── Populate wrappers (firma uniforme para el lazy dispatch) ──────── */
static void populate_wifi(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_wifi_settings_page(ctx->ui, page,
                              ctx->wifi_ssid, ctx->wifi_pass,
                              ctx->wifi_ap_enabled);
}
static void populate_display(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_display_settings_page(ctx->ui, page);
}
static void populate_sd(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_sd_settings_page(ctx->ui, page);
}
static void populate_keys(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_victron_keys_settings_page(ctx->ui, page);
}
static void populate_logs(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_logs_settings_page(ctx->ui, page);
}
static void populate_sound(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_sound_settings_page(ctx->ui, page);
}
static void populate_about(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_about_settings_page(ctx->ui, page);
}

static settings_page_ctx_t *settings_menu_add_entry(
    ui_state_t *ui, lv_obj_t *main_page,
    lv_obj_t *menu, lv_obj_t *target_page,
    const char *title, const char *subtitle,
    const char *icon, uint32_t accent,
    void (*populate)(settings_page_ctx_t *ctx, lv_obj_t *page))
{
    settings_btn_styles_init();
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_set_width(cont, lv_pct(48));
    lv_obj_set_height(cont, 88);
    settings_card_decor(cont, title, subtitle, icon, accent);

    /* Reservar ctx y guardarlo como user_data del page; el handler de
     * cambio de pagina lo lee para (1) tenir el header con el acento y
     * (2) popular la pagina la primera vez si tiene populate fn. */
    settings_page_ctx_t *ctx = NULL;
    if (s_page_ctx_count < SETTINGS_PAGE_CTX_MAX) {
        ctx = &s_page_ctxs[s_page_ctx_count++];
        memset(ctx, 0, sizeof(*ctx));
        ctx->accent = accent;
        ctx->ui = ui;
        ctx->page = target_page;
        ctx->populate = populate;
        lv_obj_set_user_data(target_page, ctx);
    }

    lv_menu_set_load_page_event(menu, cont, target_page);
    return ctx;
}

/* ── Navegacion programatica de sub-paginas (tour de capturas) ─────────── */
int ui_settings_panel_page_count(void)
{
    return (int)s_page_ctx_count;
}

void ui_settings_panel_show_page(int idx)
{
    if (idx < 0 || (size_t)idx >= s_page_ctx_count) return;
    settings_page_ctx_t *ctx = &s_page_ctxs[idx];
    if (!s_settings_menu || !ctx->page) return;
    lv_menu_set_page(s_settings_menu, ctx->page);
    /* Asegurar el populate perezoso por si lv_menu_set_page no disparo el
     * evento de cambio de pagina (el flag 'populated' evita duplicar). */
    if (!ctx->populated && ctx->populate) {
        ctx->populate(ctx, ctx->page);
        ctx->populated = true;
    }
}

/* Card clickable con la misma estetica que settings_menu_add_entry pero con
 * callback en lugar de navegacion de menu. Para subpaginas que necesitan
 * acciones (e.g. Logs -> Frigo/Bateria). */
static lv_obj_t *settings_card_btn(lv_obj_t *parent,
                                   const char *title, const char *subtitle,
                                   const char *icon, uint32_t accent,
                                   lv_event_cb_t cb, void *user_data)
{
    settings_btn_styles_init();
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(cont, lv_pct(95));
    lv_obj_set_height(cont, 88);
    settings_card_decor(cont, title, subtitle, icon, accent);
    if (cb) lv_obj_add_event_cb(cont, cb, LV_EVENT_CLICKED, user_data);
    return cont;
}

/* Recolorea el back button y el titulo del header del menu segun el acento
 * de la subpagina actual. Cuando volvemos a la pagina principal restauramos
 * el naranja por defecto. */
static void settings_menu_page_changed_cb(lv_event_t *e)
{
    lv_obj_t *menu = lv_event_get_target(e);
    if (!menu) return;
    lv_obj_t *cur = lv_menu_get_cur_main_page(menu);
    uint32_t accent = SETTINGS_DEFAULT_ACCENT;
    if (cur && cur != s_settings_main_page) {
        settings_page_ctx_t *ctx = (settings_page_ctx_t *)lv_obj_get_user_data(cur);
        if (ctx) {
            accent = ctx->accent;
            /* Lazy populate: la primera vez que se navega a la pagina,
             * construir su contenido. */
            if (!ctx->populated && ctx->populate) {
                ctx->populate(ctx, cur);
                ctx->populated = true;
            }
        }
    }
    if (s_settings_back_btn) {
        /* Variante mas oscura para el estado pulsado: ~60% de cada canal. */
        uint8_t r = (accent >> 16) & 0xFF;
        uint8_t g = (accent >>  8) & 0xFF;
        uint8_t b =  accent        & 0xFF;
        uint32_t darker = ((uint32_t)(r * 6 / 10) << 16) |
                          ((uint32_t)(g * 6 / 10) << 8)  |
                          ((uint32_t)(b * 6 / 10));
        lv_obj_set_style_bg_color(s_settings_back_btn, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_color(s_settings_back_btn, lv_color_hex(darker), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_color(s_settings_back_btn, lv_color_hex(accent), 0);
    }
    if (s_settings_main_header) {
        /* text_color hereda hacia el label de titulo de la pagina. El back
         * label tiene color explicito y no se ve afectado. */
        lv_obj_set_style_text_color(s_settings_main_header, lv_color_hex(accent), 0);
    }
}


/* --- About page: info dinamica + reboot --- */
static void about_refresh_dynamic(ui_state_t *ui)
{
    if (!ui) return;
    /* Uptime */
    if (ui->lbl_about_uptime) {
        int64_t up_s = esp_timer_get_time() / 1000000;
        int d = up_s / 86400;
        int h = (up_s % 86400) / 3600;
        int m = (up_s % 3600) / 60;
        int s = up_s % 60;
        if (d > 0)
            lv_label_set_text_fmt(ui->lbl_about_uptime, "Uptime: %dd %02dh %02dm %02ds", d, h, m, s);
        else
            lv_label_set_text_fmt(ui->lbl_about_uptime, "Uptime: %02dh %02dm %02ds", h, m, s);
    }
    /* RAM libre */
    if (ui->lbl_about_heap) {
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_spi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        lv_label_set_text_fmt(ui->lbl_about_heap,
            "RAM libre: int %u KB  |  PSRAM %u KB",
            (unsigned)(free_int / 1024), (unsigned)(free_spi / 1024));
    }
    /* IP */
    if (ui->lbl_about_ip) {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info = {0};
        if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
            lv_label_set_text_fmt(ui->lbl_about_ip, "IP AP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            lv_label_set_text(ui->lbl_about_ip, "IP AP: --");
        }
    }
}

static void about_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)t->user_data;
    /* Skip si la pagina About no esta visible: refrescar uptime + RAM + SD
     * + IP cada segundo es costoso (heap_caps_get_free_size, statvfs SD)
     * y solo tiene sentido si el usuario esta mirando. */
    if (!ui || !ui->lbl_about_uptime ||
        !lv_obj_is_visible(ui->lbl_about_uptime)) {
        return;
    }
    about_refresh_dynamic(ui);
}

static void do_reboot_action(void)
{
    ESP_LOGW(TAG_SETTINGS, "Reboot confirmed by user");
    flush_all_before_restart();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void reboot_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_POWER "  Reiniciar",
        "Estas seguro de que quieres reiniciar el dispositivo?",
        "Reiniciar", do_reboot_action);
}

static void logs_btn_solar_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_solar_history_screen(ui);
}
static void logs_btn_bat_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_battery_history_screen(ui);
}

static void logs_btn_frigo_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui) ui_show_chart_screen(ui);
}

static void create_logs_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    style_settings_scrollbar(page);
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    lv_obj_t *btn_frigo = settings_card_btn(cont,
        "Nevera",  "Histórico de temperaturas y ventilador",
        LV_SYMBOL_REFRESH,       0xFFAA00,
        logs_btn_frigo_cb, ui);
    lv_obj_set_width(btn_frigo, 500);
    lv_obj_t *btn_bat = settings_card_btn(cont,
        "Batería", "Histórico de SoC, V, I y potencia",
        LV_SYMBOL_BATTERY_FULL,  0x4FC3F7,
        logs_btn_bat_cb,   ui);
    lv_obj_set_width(btn_bat, 500);
    lv_obj_t *btn_solar = settings_card_btn(cont,
        "Placa Solar", "Producción por días, horas de sol y consumo",
        LV_SYMBOL_CHARGE,        0xFFC107,
        logs_btn_solar_cb, ui);
    lv_obj_set_width(btn_solar, 500);
}

/* === Pagina Sonido === */
/* Refs para que el switch 'Silenciar avisos' maneje el slider de volumen:
 * ON -> guarda el volumen actual y lo pone a 0; OFF -> retoma el guardado. */
static lv_obj_t *s_vol_slider  = NULL;
static lv_obj_t *s_vol_label   = NULL;
static lv_obj_t *s_mute_switch = NULL;
static int       s_vol_saved   = 50;

static void sound_volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    /* Redondeo a multiplos de 5 */
    v = (v / 5) * 5;
    lv_slider_set_value(slider, v, LV_ANIM_OFF);
    audio_set_volume(v);
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "Volumen: %d%%", v);
    /* Si suben el volumen con el silencio activo, se desactiva el silencio. */
    if (v > 0 && s_mute_switch && lv_obj_is_valid(s_mute_switch) &&
        lv_obj_has_state(s_mute_switch, LV_STATE_CHECKED)) {
        lv_obj_clear_state(s_mute_switch, LV_STATE_CHECKED);
        audio_set_mute(false);
    }
}

/* Aplica mute/unmute con guardado y restauracion del volumen, y sincroniza los
 * widgets de Settings (slider, etiqueta y switch) si ya existen. La llaman el
 * switch "Silenciar avisos" y el icono del altavoz de la barra inferior, para
 * que el comportamiento sea identico desde ambos sitios. */
void ui_settings_apply_mute(bool muted)
{
    audio_set_mute(muted);
    if (muted) {
        s_vol_saved = audio_get_volume();   /* recordar antes de poner a 0 */
        audio_set_volume(0);
    } else {
        audio_set_volume(s_vol_saved);      /* retomar el ultimo valor guardado */
    }
    int v = audio_get_volume();
    if (s_vol_slider && lv_obj_is_valid(s_vol_slider))
        lv_slider_set_value(s_vol_slider, v, LV_ANIM_OFF);
    if (s_vol_label && lv_obj_is_valid(s_vol_label))
        lv_label_set_text_fmt(s_vol_label, "Volumen: %d%%", v);
    if (s_mute_switch && lv_obj_is_valid(s_mute_switch)) {
        if (muted) lv_obj_add_state(s_mute_switch, LV_STATE_CHECKED);
        else       lv_obj_clear_state(s_mute_switch, LV_STATE_CHECKED);
    }
}

static void sound_mute_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ui_settings_apply_mute(lv_obj_has_state(sw, LV_STATE_CHECKED));
}


/* Puntero al switch de ausente para poder sincronizarlo cuando se sale del modo
 * por el gesto de 4 toques (si no, el switch queda CHECKED y hay que pulsarlo dos
 * veces para re-armar). La pagina de Settings se cachea, asi que persiste. */
static lv_obj_t *s_ausente_sw = NULL;

/* Sincroniza el switch con el estado real del modo ausente. La llama ausente_mode
 * al entrar/salir. Debe ejecutarse en la tarea LVGL (lo garantizan sus llamadores:
 * el gesto corre en LVGL; la salida por HTTP toma lvgl_port_lock). */
void settings_ausente_sync_switch(bool on)
{
    if (!s_ausente_sw) return;
    if (on) lv_obj_add_state(s_ausente_sw, LV_STATE_CHECKED);
    else    lv_obj_clear_state(s_ausente_sw, LV_STATE_CHECKED);
}

/* Switch del modo ausente/vigilancia: al activar, ausente_request inicia la
 * cuenta atras de 10 s; al desactivar, cancela (la salida real del modo activo
 * es con 4 toques en la esquina, no por este switch). */
static void ausente_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    ausente_request(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Card "Modo ausente / vigilancia", reubicada al submenu Autocaravana (antes
 * estaba en la pagina "Sonido y alertas"). */
static void create_ausente_card(lv_obj_t *cont)
{
    lv_obj_t *card_aus = lv_obj_create(cont);
    lv_obj_set_width(card_aus, lv_pct(100));
    lv_obj_set_height(card_aus, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_aus, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card_aus, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_aus, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_border_width(card_aus, 1, 0);
    lv_obj_set_style_radius(card_aus, 12, 0);
    lv_obj_set_style_pad_all(card_aus, 16, 0);
    lv_obj_set_style_pad_gap(card_aus, 8, 0);
    lv_obj_set_layout(card_aus, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_aus, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *aus_row = lv_obj_create(card_aus);
    lv_obj_remove_style_all(aus_row);
    lv_obj_set_size(aus_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(aus_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(aus_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(aus_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *aus_title = lv_label_create(aus_row);
    lv_obj_set_style_text_font(aus_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(aus_title, lv_color_hex(0x4FC3F7), 0);
    lv_label_set_text(aus_title, LV_SYMBOL_EYE_OPEN "  Modo ausente");

    lv_obj_t *aus_sw = lv_switch_create(aus_row);
    lv_obj_set_style_bg_color(aus_sw, lv_color_hex(0x4FC3F7), LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(aus_sw, ausente_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ausente_sw = aus_sw;   /* para sincronizarlo al salir por gesto (U1) */

    lv_obj_t *aus_hint = lv_label_create(card_aus);
    lv_obj_set_style_text_font(aus_hint, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(aus_hint, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_width(aus_hint, lv_pct(100));
    lv_label_set_long_mode(aus_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(aus_hint,
                      "Apaga la pantalla y vigila por movimiento. Se activa tras 10 s.\n"
                      "Para salir: 4 toques en la esquina superior izquierda.");
}

static void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;
    style_settings_scrollbar(page);
    /* Contenedor principal vertical */
    lv_obj_t *cont = lv_obj_create(page);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 16, 0);
    lv_obj_set_style_pad_gap(cont, 16, 0);

    /* === Card 1: Sonido === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(100));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(0xFF7043), 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_radius(card1, 12, 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_set_style_pad_gap(card1, 12, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Fila titulo: 'Sonido' (izda) + 'Volumen: X%' (dcha, sobre el slider) */
    lv_obj_t *title_row = lv_obj_create(card1);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(title_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card1_title = lv_label_create(title_row);
    /* Montserrat built-in para que el LV_SYMBOL_VOLUME_MAX se renderice. */
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(card1_title, lv_color_hex(0xFF7043), 0);
    lv_label_set_text(card1_title, LV_SYMBOL_VOLUME_MAX "  Sonido");

    lv_obj_t *lbl_vol = lv_label_create(title_row);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20_es, 0);
    lv_label_set_text_fmt(lbl_vol, "Volumen: %d%%", audio_get_volume());

    /* Fila: silenciar avisos (izda, texto+switch) + slider de volumen (dcha) */
    lv_obj_t *ctl_row = lv_obj_create(card1);
    lv_obj_remove_style_all(ctl_row);
    lv_obj_set_size(ctl_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(ctl_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(ctl_row, 10, 0);     /* separa un poco 'Silenciar avisos' */
    lv_obj_set_style_pad_column(ctl_row, 14, 0);   /* hueco entre el texto y el switch */

    /* Silenciar avisos a la IZQUIERDA (texto + switch) */
    lv_obj_t *lbl_mute = lv_label_create(ctl_row);
    lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_mute, "Silenciar avisos");

    lv_obj_t *sw = lv_switch_create(ctl_row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFF7043), LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (audio_is_muted()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sound_mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui->sound_mute_switch = sw;

    /* Espaciador flexible: empuja el slider al borde derecho */
    lv_obj_t *spacer = lv_obj_create(ctl_row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    /* Slider de volumen a la DERECHA */
    lv_obj_t *slider = lv_slider_create(ctl_row);
    lv_obj_set_width(slider, 440);
    lv_obj_set_height(slider, 26);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF7043), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF7043), LV_PART_KNOB);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, sound_volume_changed_cb, LV_EVENT_VALUE_CHANGED, lbl_vol);

    /* Refs para que 'Silenciar avisos' maneje el slider (guardar/0/restaurar). */
    s_vol_slider  = slider;
    s_vol_label   = lbl_vol;
    s_mute_switch = sw;
    if (!audio_is_muted()) s_vol_saved = audio_get_volume();

    /* (La card "Modo ausente / vigilancia" se movio al submenu Autocaravana:
     *  ver create_ausente_card / populate_autocaravana.) */

    /* === Card 2: Bateria === */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 12, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_set_style_pad_gap(card2, 16, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_title = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_title, lv_color_hex(0xFF9800), 0);
    lv_label_set_text(card2_title, LV_SYMBOL_BATTERY_FULL "  Bateria");

    /* row_soc deja de ser child de card2; usa card2 como flex padre directamente */
    lv_obj_t *row_soc = card2;

    /* SoC Critico */
    lv_obj_t *col_crit = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_crit);
    lv_obj_set_layout(col_crit, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_crit, LV_FLEX_FLOW_ROW);   /* icono+texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_crit, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_crit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_crit, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_crit = lv_label_create(col_crit);
    lv_obj_set_style_text_font(lbl_crit, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_crit, lv_color_hex(0xFF4444), 0);
    lv_label_set_text(lbl_crit, LV_SYMBOL_WARNING " Critico");
    lv_obj_t *dd_crit = lv_dropdown_create(col_crit);
    lv_obj_set_width(dd_crit, 130);
    lv_obj_set_style_text_font(dd_crit, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_crit, "10 %\n20 %\n30 %\n40 %");
    {
        int cur = alerts_get_soc_critical();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_crit_options)/sizeof(s_soc_crit_options[0]); ++k) {
            if (s_soc_crit_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_crit, idx);
    }
    lv_obj_add_event_cb(dd_crit, soc_crit_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* SoC Aviso */
    lv_obj_t *col_warn = lv_obj_create(row_soc);
    lv_obj_remove_style_all(col_warn);
    lv_obj_set_layout(col_warn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_warn, LV_FLEX_FLOW_ROW);   /* icono+texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_warn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_warn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_warn, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_warn = lv_label_create(col_warn);
    lv_obj_set_style_text_font(lbl_warn, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(lbl_warn, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(lbl_warn, LV_SYMBOL_BELL " Aviso");
    lv_obj_t *dd_warn = lv_dropdown_create(col_warn);
    lv_obj_set_width(dd_warn, 130);
    lv_obj_set_style_text_font(dd_warn, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_warn, "40 %\n50 %\n60 %\n70 %");
    {
        int cur = alerts_get_soc_warning();
        int idx = 2;
        for (size_t k = 0; k < sizeof(s_soc_warn_options)/sizeof(s_soc_warn_options[0]); ++k) {
            if (s_soc_warn_options[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_warn, idx);
    }
    lv_obj_add_event_cb(dd_warn, soc_warn_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* === Card 3: Congelador === */
    lv_obj_t *card3 = lv_obj_create(cont);
    lv_obj_set_width(card3, lv_pct(100));
    lv_obj_set_height(card3, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x00C851), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 12, 0);
    lv_obj_set_style_pad_all(card3, 16, 0);
    lv_obj_set_style_pad_gap(card3, 16, 0);
    lv_obj_set_layout(card3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card3_title = lv_label_create(card3);
    lv_obj_set_style_text_font(card3_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card3_title, lv_color_hex(0x00C851), 0);
    lv_label_set_text(card3_title, LV_SYMBOL_CHARGE "  Congelador");

    lv_obj_t *row_frigo = card3;

    /* Col minutos */
    lv_obj_t *col_min_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_min_a);
    lv_obj_set_layout(col_min_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_min_a, LV_FLEX_FLOW_ROW);   /* texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_min_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_min_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_min_a, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_min_a = lv_label_create(col_min_a);
    lv_obj_set_style_text_font(lbl_min_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_min_a, "Tras subir (min)");
    lv_obj_t *dd_min_a = lv_dropdown_create(col_min_a);
    lv_obj_set_width(dd_min_a, 130);
    lv_obj_set_style_text_font(dd_min_a, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_min_a, "15\n30\n45\n60\n90");
    {
        static const int opts[] = { 15, 30, 45, 60, 90 };
        int cur = alerts_get_freezer_minutes();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_min_a, idx);
    }
    lv_obj_add_event_cb(dd_min_a, alarm_min_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);

    /* Col temp umbral */
    lv_obj_t *col_t_a = lv_obj_create(row_frigo);
    lv_obj_remove_style_all(col_t_a);
    lv_obj_set_layout(col_t_a, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_t_a, LV_FLEX_FLOW_ROW);   /* texto a la izda, selector a la dcha */
    lv_obj_set_flex_align(col_t_a, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(col_t_a, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col_t_a, 10, 0);   /* un poco separado del selector */
    lv_obj_t *lbl_t_a = lv_label_create(col_t_a);
    lv_obj_set_style_text_font(lbl_t_a, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_t_a, "Si supera");
    lv_obj_t *dd_t_a = lv_dropdown_create(col_t_a);
    lv_obj_set_width(dd_t_a, 140);
    lv_obj_set_style_text_font(dd_t_a, &lv_font_montserrat_20_es, 0);
    lv_dropdown_set_options(dd_t_a, "-5 \xc2\xb0""C\n-2 \xc2\xb0""C\n0 \xc2\xb0""C\n+2 \xc2\xb0""C");
    {
        static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
        float cur = alerts_get_freezer_temp_c();
        int idx = 1;
        for (size_t k = 0; k < sizeof(opts)/sizeof(opts[0]); ++k) {
            if (opts[k] == cur) { idx = (int)k; break; }
        }
        lv_dropdown_set_selected(dd_t_a, idx);
    }
    lv_obj_add_event_cb(dd_t_a, alarm_temp_dd_cb_sound, LV_EVENT_VALUE_CHANGED, NULL);
}


static void alarm_min_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const int opts[] = { 15, 30, 45, 60, 90 };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_minutes(opts[sel]);
}

static void alarm_temp_dd_cb_sound(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    static const float opts[] = { -5.0f, -2.0f, 0.0f, 2.0f };
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(opts)/sizeof(opts[0])) alerts_set_freezer_temp_c(opts[sel]);
}
