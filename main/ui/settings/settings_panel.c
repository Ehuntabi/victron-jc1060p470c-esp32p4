#include "settings_panel.h"
#include "settings_common.h"

/* Paginas ya separadas en sus propios ficheros (troceo de settings_panel.c). */
void create_display_settings_page(ui_state_t *ui, lv_obj_t *page_display);
#include "ui.h"
#include "ui/widgets/ui_card.h"
#include "ui/vigilancia/ausente_mode.h"
#include "ui/vigilancia/gallery.h"
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
#include "portal/config_server.h"
#include "victron_ble.h"
#include "display.h"
#include "esp_log.h"
#include "datalogger.h"
#include "battery_history.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ui/views/frigo_panel.h"
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
#include "data/trip_computer.h"
#include "data/solar_daily.h"
#include "ne185_vlog.h"   /* ne185_vlog_flush: tambien se vuelca al finalizar viaje */

/* flush_all_before_restart: vive en settings_about.c (unico caller: reboot). */
#include <time.h>

// Forward declaration for view update function
extern void ui_force_view_update(void);
extern void ui_start_capture_carousel(void);
extern bool ui_capture_carousel_running(void);
extern void ui_gallery_open(void);   /* visor de galeria en pantalla (gallery.c) */

#define WIFI_NAMESPACE "wifi"

const char *TAG_SETTINGS = "UI_SETTINGS";

/* Pagina Wi-Fi: vive en settings_wifi.c. */
void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled);
void password_toggle_btn_event_cb(lv_event_t *e);
/* Energia del viaje + backup: definidos mas abajo, usados por la pagina Tarjeta SD */
/* Energia del viaje: vive en trip_manager.c. */
void create_trip_card(lv_obj_t *cont);
void trip_label_refresh(void);
void create_bombona_card(lv_obj_t *cont);
void bombona_label_refresh(void);
static void backup_export_cb(lv_event_t *e);
static void backup_import_cb(lv_event_t *e);
static void sd_trip_timer_cb(lv_timer_t *t);



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

static void screensaver_timer_cb(lv_timer_t *timer);

void screensaver_enable(ui_state_t *ui, bool enable);
static void screensaver_wake(ui_state_t *ui);
/* El timer del salvapantallas hace de detector de reposo. Debe correr si el
 * salvapantallas esta activado O si el modo nocturno esta activado (para poder
 * apagar de noche aunque el salvapantallas este off). */
static void screensaver_sync_timer_state(ui_state_t *ui);

/* Aplica inmediatamente el brillo correcto según hora actual + config. */
static bool night_in_window(int h, uint8_t s, uint8_t e)
{
    /* Inicio==Fin se trata como 24h (siempre de noche), no como "nunca".
     * Antes devolvia false sin avisar: activar el switch con Inicio==Fin
     * (llegar ahi con los botones +/- es facil, dan la vuelta a 0..23) dejaba
     * el modo nocturno completamente mudo, pareciendo roto. "Quiero que sea
     * de noche siempre" es la lectura natural de esa igualdad para quien
     * llega ahi. Detectado por el usuario el 09-sep-2026. */
    if (s == e) return true;
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
/* Pagina "Acerca de": vive en settings_about.c. */
void create_about_settings_page(ui_state_t *ui, lv_obj_t *page_about);
/* Pagina "GPS": vive en settings_gps.c. */
void create_gps_settings_page(ui_state_t *ui, lv_obj_t *page_gps);
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
/* Cual de las paginas registradas es la del GPS. Se apunta al darla de alta y
 * NO se escribe el numero a mano: el indice es el orden de registro, asi que
 * meter una seccion nueva por el medio lo cambiaria y el icono del GPS acabaria
 * abriendo otra cosa. */
static int s_page_gps_idx = -1;

/* Forward decls de los populate wrappers (defs cerca de settings_menu_add_entry). */
static void populate_wifi(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_display(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_sd(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_keys(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_logs(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_sound(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_about(settings_page_ctx_t *ctx, lv_obj_t *page);
static void populate_gps(settings_page_ctx_t *ctx, lv_obj_t *page);

/* ── Scrollbar visible en cualquier pagina de Settings ────────── */
/* Aplica scrollbar AUTO (visible cuando hay overflow) con estilo claro
 * para indicar que se puede deslizar. Llamar tras crear la page. */
void style_settings_scrollbar(lv_obj_t *page)
{
    if (!page) return;
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, UI_COLOR_ORANGE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(page, 6, LV_PART_SCROLLBAR);
}





/* Pagina "Sonido y alertas" + tarjeta "Modo ausente": viven en
 * settings_sound.c. */
void create_sound_settings_page(ui_state_t *ui, lv_obj_t *page);
void create_ausente_card(lv_obj_t *cont);

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
static lv_obj_t *s_settings_back_chev = NULL;    /* el chevron, del color de la seccion */
static lv_obj_t *s_settings_back_label = NULL;   /* el nombre del padre */
static lv_obj_t *s_settings_header_spacer = NULL;
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
/* Refresco del Energia del viaje (card reubicada en Tarjeta SD): solo si esta
 * visible, para no gastar cada segundo cuando no se mira. */
static void sd_trip_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_state_t *ui = t ? (ui_state_t *)t->user_data : NULL;
    trip_label_refresh();
    /* Los dias que lleva puesta la bombona cambian una vez al dia, pero la
     * etiqueta se recalcula aqui igual: la funcion sale sola si la tarjeta no
     * esta visible, asi que no cuesta nada y evita otro timer. */
    bombona_label_refresh();

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
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_gap(cont, 10, 0);

    /* === Card Carrusel captura pantalla === */
    lv_obj_t *card_cap = lv_obj_create(cont);
    lv_obj_set_width(card_cap, lv_pct(100));
    lv_obj_set_height(card_cap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_cap, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card_cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_cap, lv_color_hex(0x29B6F6), 0);  /* azul */
    lv_obj_set_style_border_width(card_cap, 2, 0);
    lv_obj_set_style_radius(card_cap, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card_cap, 12, 0);
    lv_obj_set_style_pad_gap(card_cap, 6, 0);
    lv_obj_set_layout(card_cap, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_cap, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *cap_title = lv_label_create(card_cap);
    lv_obj_set_style_text_font(cap_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(cap_title, lv_color_hex(0x29B6F6), 0);
    lv_label_set_text(cap_title, LV_SYMBOL_IMAGE "  Carrusel captura pantalla");

    ui->capture_switch = lv_switch_create(card_cap);
    lv_obj_set_size(ui->capture_switch, 50, 28);
    lv_obj_set_style_bg_color(ui->capture_switch, lv_color_hex(0x29B6F6),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(ui->capture_switch, cb_capture_carousel_cb,
                        LV_EVENT_VALUE_CHANGED, ui);
    ui_card_wrap_title_with(card_cap, cap_title, lv_color_hex(0x29B6F6), ui->capture_switch);

    ui->capture_status_lbl = lv_label_create(card_cap);
    lv_obj_set_style_text_font(ui->capture_status_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->capture_status_lbl, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->capture_status_lbl,
                      "Guarda una captura de cada pantalla (Live, históricos y Ajustes) en la SD");

    ui->lbl_about_sd = lv_label_create(card_cap);
    lv_obj_set_style_text_font(ui->lbl_about_sd, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->lbl_about_sd, lv_color_hex(0x888888), 0);
    lv_label_set_text(ui->lbl_about_sd, "SD: --");

    /* === Card Visor de imagenes (separado del carrusel) === */
    lv_obj_t *card_view = lv_obj_create(cont);
    lv_obj_set_width(card_view, lv_pct(100));
    lv_obj_set_height(card_view, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_view, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_view, lv_color_hex(0x26C6DA), 0);  /* cyan */
    lv_obj_set_style_border_width(card_view, 2, 0);
    lv_obj_set_style_radius(card_view, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card_view, 12, 0);
    lv_obj_set_style_pad_gap(card_view, 6, 0);
    lv_obj_set_layout(card_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_view, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *view_title = lv_label_create(card_view);
    lv_obj_set_flex_grow(view_title, 1);
    lv_obj_set_style_text_font(view_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(view_title, lv_color_hex(0x26C6DA), 0);
    lv_label_set_text(view_title, LV_SYMBOL_IMAGE "  Visor de imagenes");

    lv_obj_t *btn_gal = lv_btn_create(card_view);
    lv_obj_set_width(btn_gal, LV_SIZE_CONTENT);   /* acorde al texto + icono */
    lv_obj_set_height(btn_gal, 46);
    lv_obj_set_style_pad_hor(btn_gal, 24, 0);
    lv_obj_set_style_bg_color(btn_gal, lv_color_hex(0x0288D1), 0);
    lv_obj_set_style_radius(btn_gal, 8, 0);
    lv_obj_t *lbl_gal = lv_label_create(btn_gal);
    lv_obj_set_style_text_font(lbl_gal, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(lbl_gal, LV_SYMBOL_IMAGE "  Ver capturas");
    lv_obj_center(lbl_gal);
    ui_card_wrap_title_with(card_view, view_title, lv_color_hex(0x26C6DA), btn_gal);
    lv_obj_add_event_cb(btn_gal, cb_open_gallery, LV_EVENT_CLICKED, NULL);

    lv_obj_t *view_desc = lv_label_create(card_view);
    lv_obj_set_style_text_font(view_desc, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(view_desc, UI_COLOR_TEXT_SOFT, 0);
    lv_label_set_text(view_desc, "Vigilancia y capturas del carrusel");


    /* (La card "Energia del viaje" se movio al submenu Autocaravana:
     *  ver create_trip_card / populate_autocaravana.) */

    /* === Card 4: Backup/Restore configuracion === */
    lv_obj_t *card_bak = lv_obj_create(cont);
    lv_obj_set_width(card_bak, lv_pct(100));
    lv_obj_set_height(card_bak, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_bak, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card_bak, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_bak, lv_color_hex(0x9C27B0), 0);
    lv_obj_set_style_border_width(card_bak, 2, 0);
    lv_obj_set_style_radius(card_bak, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card_bak, 12, 0);
    lv_obj_set_style_pad_gap(card_bak, 8, 0);
    lv_obj_set_layout(card_bak, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card_bak, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bak_title = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(bak_title, lv_color_hex(0x9C27B0), 0);
    lv_label_set_text(bak_title, LV_SYMBOL_SD_CARD "  Copia de seguridad de la configuracion");
    ui_card_wrap_title(card_bak, bak_title, lv_color_hex(0x9C27B0));

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
    lv_obj_set_style_bg_color(btn_exp, UI_COLOR_GREEN, 0);
    lv_obj_set_style_radius(btn_exp, 10, 0);
    lv_obj_t *lbl_exp = lv_label_create(btn_exp);
    lv_label_set_text(lbl_exp, LV_SYMBOL_UPLOAD "  Exportar");
    lv_obj_set_style_text_font(lbl_exp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_exp);

    lv_obj_t *btn_imp = lv_btn_create(bak_row);
    lv_obj_set_size(btn_imp, 200, 50);
    lv_obj_set_style_bg_color(btn_imp, UI_COLOR_ORANGE, 0);
    lv_obj_set_style_radius(btn_imp, 10, 0);
    lv_obj_t *lbl_imp = lv_label_create(btn_imp);
    lv_label_set_text(lbl_imp, LV_SYMBOL_DOWNLOAD "  Importar");
    lv_obj_set_style_text_font(lbl_imp, &lv_font_montserrat_20_es, 0);
    lv_obj_center(lbl_imp);

    /* Status label */
    lv_obj_t *bak_status = lv_label_create(card_bak);
    lv_obj_set_style_text_font(bak_status, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(bak_status, UI_COLOR_YELLOW, 0);
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
    lv_obj_set_style_bg_color(card_auto, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card_auto, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_auto, lv_color_hex(0xFFAA00), 0);  /* ambar */
    lv_obj_set_style_border_width(card_auto, 2, 0);
    lv_obj_set_style_radius(card_auto, UI_RADIUS_CARD, 0);
    /* 16 -> 12 (22-sep-2026): la linea de abajo de esta tarjeta, que es la ultima
     * de la pagina Autocaravana, se cortaba por muy poco. */
    lv_obj_set_style_pad_all(card_auto, 6, 0);
    lv_obj_set_style_pad_gap(card_auto, 8, 0);
    lv_obj_set_layout(card_auto, LV_LAYOUT_FLEX);
    /* v3.10: columna con la cabecera centrada arriba y el interruptor debajo. */
    lv_obj_set_flex_flow(card_auto, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_auto, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_sw = lv_switch_create(card_auto);
    lv_obj_set_style_bg_color(auto_sw, lv_color_hex(0xFFAA00),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ne185_get_autostart()) lv_obj_add_state(auto_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(auto_sw, autostart_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *auto_title = lv_label_create(card_auto);
    lv_obj_set_style_text_font(auto_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(auto_title, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(auto_title, LV_SYMBOL_POWER "  Auto-encendido (luz + bomba)");
    ui_card_wrap_title_with(card_auto, auto_title, lv_color_hex(0xFFAA00), auto_sw);
}


void populate_autocaravana(settings_page_ctx_t *ctx, lv_obj_t *page)
{
    (void)ctx;
    /* Cards del vehiculo bajo las entradas "Opciones Frigo" y "Victron Keys"
     * (anadidas en el init). Orden: Modo ausente, Energia del viaje, Bombonas,
     * Auto-encendido.
     * El timer que refresca el trip se crea en el init. */
    create_ausente_card(page);
    create_trip_card(page);
    create_bombona_card(page);
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
    /* Fondo del menu = el de la paleta (22-sep-2026). Iba en negro puro: mas
     * oscuro que el fondo de las vistas y sin relacion con la paleta. */
    lv_obj_set_style_bg_color(menu, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(lv_menu_get_main_header(menu), UI_COLOR_BG, 0);
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

    /* MIGAS DE PAN en vez del boton "Volver" (v3.9). El boton lleno de color de
     * seccion, con borde blanco y halo, pesaba mas que el propio titulo y no
     * decia a donde volvia. Ahora es un chevron del color de la seccion y el
     * nombre del padre en gris apagado ("‹ Ajustes"), sin caja: mismo gesto,
     * mismo sitio, misma zona tactil (52 px de alto), y el titulo sigue centrado
     * porque el espaciador de la derecha se ajusta al ancho de las migas. */
    lv_obj_t *back_btn = lv_menu_get_main_header_back_btn(menu);
    s_settings_back_btn = back_btn;
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_set_style_pad_hor(back_btn, 8, 0);
    lv_obj_set_style_pad_ver(back_btn, 12, 0);
    lv_obj_set_style_pad_column(back_btn, 8, 0);
    /* Al pulsar, un tinte suave del acento: se nota el toque sin volver a la
     * pildora de color. */
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x4CD964), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20, LV_STATE_PRESSED);

    /* El chevron que crea lv_menu dentro del boton no lo usamos: lo dibujamos
     * nosotros para poder ponerlo del color de la seccion. */
    lv_obj_t *icono_menu = lv_obj_get_child(back_btn, 0);
    if (icono_menu) lv_obj_add_flag(icono_menu, LV_OBJ_FLAG_HIDDEN);

    s_settings_back_chev = lv_label_create(back_btn);
    lv_label_set_text(s_settings_back_chev, LV_SYMBOL_LEFT);
    /* El chevron va en la fuente NORMAL a proposito: la SemiBold se genero sin
     * los rangos de FontAwesome y sin fallback, asi que LV_SYMBOL_LEFT salia como
     * una caja. La normal lleva Montserrat de fallback y dibuja el simbolo. */
    lv_obj_set_style_text_font(s_settings_back_chev, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_settings_back_chev, lv_color_hex(0x4CD964), 0);

    s_settings_back_label = lv_label_create(back_btn);
    lv_label_set_text(s_settings_back_label, "Ajustes");
    lv_obj_set_style_text_font(s_settings_back_label, &lv_font_semibold_20, 0);
    lv_obj_set_style_text_color(s_settings_back_label, UI_COLOR_TEXT_SOFT, 0);
    lv_obj_set_style_text_color(s_settings_back_label, UI_COLOR_TEXT, LV_STATE_PRESSED);
    /* Spacer invisible para centrar el titulo via SPACE_BETWEEN */
    lv_obj_t *header_spacer = lv_obj_create(main_header);
    lv_obj_remove_style_all(header_spacer);
    lv_obj_set_size(header_spacer, 110, 1);
    s_settings_header_spacer = header_spacer;

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    s_settings_main_page = main_page;
    lv_obj_t *page_frigo = lv_menu_page_create(menu, "FRIGO");
    ui->frigo_page = page_frigo;
    lv_obj_t *page_logs = lv_menu_page_create(menu, "HISTORIAL EN GRÁFICOS");
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
    lv_obj_t *page_gps   = lv_menu_page_create(menu, "GPS");
    lv_obj_t *page_about = lv_menu_page_create(menu, "ACERCA DE Joint SPL 145 Control");
    
    /* Padding del main_page + layout 2 columnas */
    /* Horizontal 12 (unificado con las vistas y las subpaginas, 22-sep-2026) y
     * vertical 12 tambien desde la v3.7: aqui el contenido son 4 filas de 88 px
     * en 587, asi que sobra sitio. */
    lv_obj_set_style_pad_hor(main_page, 12, 0);
    /* 12 arriba, igual que el margen lateral y que las vistas (Overview 24,
     * Bateria 32 de primer contenido medido): con el flex ya sin centrar el
     * bloque, 4 px dejaban la primera tarjeta pegada al borde (y=17). Aqui
     * sobra sitio: la pagina son 4 filas de 88 px en 587. */
    lv_obj_set_style_pad_top(main_page, 12, 0);
    lv_obj_set_style_pad_bottom(main_page, 12, 0);
    lv_obj_set_style_pad_row(main_page, 12, 0);
    lv_obj_set_style_pad_column(main_page, 12, 0);
    lv_obj_set_layout(main_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    /* El TERCER parametro (track_place) coloca las FILAS de un flujo ROW_WRAP en
     * el eje cruzado. En la v3.7 se puso en START para quitar la banda de arriba
     * (el contenido empezaba en y=108); ahora, con las entradas mas bajas, el
     * menu dejaba ~140 px muertos abajo y el usuario lo quiere CENTRADO: en
     * CENTER el bloque queda con el mismo aire arriba y abajo. Horizontal ya esta
     * centrado: las dos columnas son del 48% con margenes iguales a los lados. */
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Subpagina Autocaravana: mismo layout de 2 columnas que el menu principal.
     * Aqui el vertical se queda en 4: el contenido llega a y=528 de 538 y no
     * cabe un margen mayor sin que aparezca la barra de scroll. */
    lv_obj_set_style_pad_hor(page_autocaravana, 12, 0);
    lv_obj_set_style_pad_top(page_autocaravana, 2, 0);
    lv_obj_set_style_pad_bottom(page_autocaravana, 4, 0);
    lv_obj_set_style_pad_row(page_autocaravana, 4, 0);   /* 2 px menos por hueco: el borde de 2 px de las tarjetas suma 8 en esta pagina */
    lv_obj_set_style_pad_column(page_autocaravana, 12, 0);
    lv_obj_set_layout(page_autocaravana, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page_autocaravana, LV_FLEX_FLOW_ROW_WRAP);
    /* Mismo caso que main_page: ver el comentario de arriba. */
    lv_obj_set_flex_align(page_autocaravana, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    /* Frigo: eager (es la pagina mas usada). populate=NULL para que el lazy
     * dispatch no haga nada; ui_frigo_panel_init mas abajo construye el
     * contenido. */
    settings_menu_add_entry(ui, page_autocaravana, menu, page_frigo,
        "Opciones Frigo", "Sensores, ventilador y umbrales",
        LV_SYMBOL_REFRESH,    0x00C851, NULL);
    settings_menu_add_entry(ui, main_page, menu, page_logs,
        "Historial en gráficos", "Histórico SD: batería, nevera y placa solar",
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
    /* v3.8: fuera el tinte propio del submenu (0x2E2E38): todas las entradas
     * de Ajustes usan el mismo fondo de la paleta. */
    settings_menu_add_entry(ui, main_page, menu, page_gps,
        "GPS",           "Posición, satélites y puesta en hora",
        LV_SYMBOL_GPS,        0x4CD964, populate_gps);
    s_page_gps_idx = (int)s_page_ctx_count - 1;
    settings_menu_add_entry(ui, main_page, menu, page_about,
        "Acerca de",     "Sistema, uptime y reinicio",
        LV_SYMBOL_LIST,       0x90A4AE, populate_about);


    lv_menu_set_page(menu, main_page);
    /* Recolorea header + back btn al cambiar de pagina segun acento de seccion
     * y dispara el lazy populate de la pagina destino la primera vez. */
    lv_obj_add_event_cb(menu, settings_menu_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Frigo eager (lazy dispatch skipped via populate=NULL). El resto se
     * construye al navegar por primera vez. */
    ui_frigo_panel_init(ui);

    /* Timer 1 s: refresca el Energia del viaje y el espacio libre de la SD (cada
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
                /* Modales huerfanos: mismo motivo que en
                 * idle_to_live_timer_cb (ui.c) -- se crean en
                 * lv_layer_top() y solo se cerraban con sus propios
                 * botones, asi que este otro camino de vuelta a Live (el
                 * salvapantallas rotando) tambien podia dejarlos flotando
                 * encima de la pantalla siguiente. Detectado por el
                 * usuario el 09-sep-2026. */
                ui_close_confirm_dialog();
                victron_keys_close_modals();
                ui_gallery_close();
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
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], UI_COLOR_GREEN, 0); // Green for active
        } else {
            lv_label_set_text(ui->victron_config.device_type_labels[index], "Device: --");
            lv_obj_set_style_text_color(ui->victron_config.device_type_labels[index], lv_color_hex(0x888888), 0); // Gray for inactive
        }
    }

    /* Update product name */
    if (ui->victron_config.product_name_labels[index]) {
        if (product_name && product_name[0] != '\0') {
            lv_label_set_text_fmt(ui->victron_config.product_name_labels[index], "Product: %s", product_name);
            lv_obj_set_style_text_color(ui->victron_config.product_name_labels[index], UI_COLOR_GREEN, 0); // Green for active
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
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], UI_COLOR_GREEN, 0); // Green for OK
            } else {
                lv_obj_set_style_text_color(ui->victron_config.error_labels[index], UI_COLOR_ORANGE, 0); // Orange for warnings
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
            err == ESP_OK ? UI_COLOR_GREEN : UI_COLOR_RED_DARK, 0);
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
            err == ESP_OK ? UI_COLOR_GREEN : UI_COLOR_RED_DARK, 0);
    }
}



static void settings_btn_styles_init(void)
{
    if (s_settings_styles_inited) return;
    /* Cuerpo neutro: el color del rol sale a traves de la barra lateral y el
     * icono coloreado, no del fondo de la card. */
    lv_style_init(&s_settings_btn_style);
    lv_style_set_bg_opa(&s_settings_btn_style, LV_OPA_COVER);
    /* v3.8: el fondo de las entradas del menu es el de la paleta, el mismo que
     * el de las tarjetas de dentro: antes eran dos grises propios (0x1A1A1A con
     * borde 0x2A2A2A) y se notaba al entrar en una pagina. */
    lv_style_set_bg_color(&s_settings_btn_style, UI_COLOR_CARD);
    lv_style_set_border_color(&s_settings_btn_style, UI_COLOR_CARD_BORDER);
    lv_style_set_border_width(&s_settings_btn_style, 1);
    lv_style_set_radius(&s_settings_btn_style, 12);
    lv_style_set_pad_all(&s_settings_btn_style, 0);
    lv_style_set_pad_column(&s_settings_btn_style, 12);
    lv_style_set_min_height(&s_settings_btn_style, 88);

    /* Pressed: simplemente un poco mas claro, sin pisar el color de rol. */
    lv_style_init(&s_settings_btn_pressed_style);
    lv_style_set_bg_color(&s_settings_btn_pressed_style, lv_color_hex(0x2A3446));  /* paleta, un punto mas clara al pulsar */
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

    /* Icono grande coloreado con el acento. El nombre lv_font_montserrat_28 esta
     * aliasado a Inter en fonts_es.h, y los LV_SYMBOL_* (LIST en About, SAVE en
     * Logs, WIFI, GPS, EYE_OPEN, VOLUME_MAX...) salen por su fallback a
     * Montserrat. El comentario viejo hablaba de la Montserrat de LVGL, que ya
     * no se usa. */
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
    /* Titulo en SemiBold (v3.9): misma letra y mismo tamano que antes, con mas
     * presencia frente al subtitulo, que sigue en 14 regular. */
    lv_obj_set_style_text_font(t1, &lv_font_semibold_24, 0);
    lv_obj_set_style_text_color(t1, lv_color_white(), 0);

    if (subtitle && *subtitle) {
        lv_obj_t *t2 = lv_label_create(txt);
        lv_label_set_text(t2, subtitle);
        lv_obj_set_style_text_font(t2, &lv_font_montserrat_14_es, 0);
        lv_obj_set_style_text_color(t2, UI_COLOR_TEXT_SOFT, 0);
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

static void populate_gps(settings_page_ctx_t *ctx, lv_obj_t *page) {
    create_gps_settings_page(ctx->ui, page);
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
    lv_obj_set_height(cont, 58);
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
        /* Los dos unicos contenedores con entradas son el menu principal y la
         * subpagina Autocaravana; de ahi el nombre que enseña la miga de pan. */
        ctx->padre = (main_page == s_settings_main_page) ? "Ajustes" : "Autocaravana";
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

/* Abre directamente la pagina del GPS. La usa el icono de la barra: si el GPS
 * no va, lo primero que uno hace es tocarlo, y hasta ahora eso no llevaba a
 * ninguna parte -- habia que ir a Ajustes y buscarla. */
void ui_settings_panel_show_gps(void)
{
    ui_settings_panel_show_page(s_page_gps_idx);
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
    lv_obj_set_height(cont, 58);
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
    settings_page_ctx_t *ctx = NULL;
    if (cur && cur != s_settings_main_page) {
        ctx = (settings_page_ctx_t *)lv_obj_get_user_data(cur);
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
        /* Miga de pan: chevron del acento de la seccion, nombre del padre en
         * gris y, al pulsar, el tinte suave de ese mismo acento. */
        lv_obj_set_style_bg_color(s_settings_back_btn, lv_color_hex(accent), LV_STATE_PRESSED);
        if (s_settings_back_chev) {
            lv_obj_set_style_text_color(s_settings_back_chev, lv_color_hex(accent), 0);
        }
        if (s_settings_back_label) {
            lv_label_set_text(s_settings_back_label,
                              (ctx && ctx->padre) ? ctx->padre : "Ajustes");
            /* El titulo se centra con SPACE_BETWEEN entre las migas y el
             * espaciador de la derecha: si el padre se llama "Autocaravana" el
             * espaciador tiene que crecer con el, o el titulo baila. */
            if (s_settings_header_spacer) {
                lv_obj_update_layout(s_settings_back_label);
                lv_coord_t ancho = lv_obj_get_width(s_settings_back_btn);
                if (ancho > 0) lv_obj_set_width(s_settings_header_spacer, ancho);
            }
        }
    }
    if (s_settings_main_header) {
        /* text_color hereda hacia el label de titulo de la pagina. El back
         * label tiene color explicito y no se ve afectado. */
        lv_obj_set_style_text_color(s_settings_main_header, lv_color_hex(accent), 0);
    }
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
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_gap(cont, 10, 0);

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

