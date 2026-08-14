/* Pagina "Acerca de" de Ajustes: estado dinamico (uptime, RAM, IP, ultimo
 * reset), version/repo/creditos y el boton Reiniciar.
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver
 * settings_common.h). Es el mismo codigo movido de sitio, sin cambios de
 * comportamiento.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "fonts/fonts_es.h"

#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "watchdog.h"
#include "datalogger.h"
#include "battery_history.h"
#include "data/solar_daily.h"
#include "data/trip_computer.h"
#include "ne185_vlog.h"

/* La version mostrada en About sale de esp_app_get_description()->version, que
 * ESP-IDF rellena automaticamente con `git describe` (el tag mas reciente, p.ej.
 * "v1.0.0"; en builds fuera de un tag, "v1.0.0-3-gABCDEF"). Este texto es solo
 * un fallback por si la descripcion de la app no estuviera disponible. */
static const char *APP_VERSION_FALLBACK = "v?";

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

void create_about_settings_page(ui_state_t *ui, lv_obj_t *page)
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
