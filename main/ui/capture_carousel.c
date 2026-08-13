/* capture_carousel.c - ver capture_carousel.h
 *
 * Navegacion de pantallas para captura (carrusel a demanda de Settings y
 * /captura por WiFi). LVGL no es thread-safe: se toma lvgl_port_lock y se
 * suelta durante las esperas para que lleguen datos BLE reales y se dibuje
 * la vista antes de fotografiarla.
 */
#include "ui/capture_carousel.h"
#include "ui.h"
#include "ui/gallery.h"
#include "ui/settings_panel.h"
#include "screenshot.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <lvgl.h>
#include <stdio.h>

#define TOUR_DIR        "/sdcard/screenshots"
#define TOUR_SETTLE_MS        1500   /* dejar que la vista se actualice/dibuje */

static void tour_set_view(ui_state_t *ui, ui_view_mode_t mode)
{
    if (lvgl_port_lock(1000)) {
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);  /* Live */
        ui->view_selection.mode = mode;
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }
}

/* Espera a que la vista se dibuje MANTENIENDO la pantalla despierta. La
 * navegacion programatica del tour NO cuenta como actividad del usuario, asi
 * que sin esto, pasados 60 s, saltarian el auto-return de Ajustes, el
 * idle-to-live o el salvapantallas (que cambiaria de vista) y arruinarian las
 * capturas. Reseteamos los tres relojes de inactividad en cada paso; si el
 * salvapantallas ya estuviera activo, ui_notify_user_activity lo despierta. */
static void tour_settle(void)
{
    if (lvgl_port_lock(500)) {
        lv_disp_trig_activity(NULL);   /* auto-return de Ajustes (inactive_time) */
        ui_notify_user_activity();     /* idle-to-live + screensaver_wake */
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(TOUR_SETTLE_MS));
}

/* ─── Carrusel de captura a demanda (switch en Settings→Display) ──────────────
 * Recorre SOLO las 8 pantallas de datos (6 device-views + grafico bateria +
 * grafico frigo), captura cada una a BMP en la SD (sobrescribe mismos nombres),
 * y al terminar apaga el switch y muestra el resultado. Reutiliza
 * tour_set_view/tour_settle/screenshot_save_bmp. A diferencia de
 * screenshot_tour_task: sin marcador, sin retardo de 60 s y sin las paginas de
 * Ajustes. Un unico disparo a la vez (s_capture_running). */
static volatile bool s_capture_running = false;

/* Guarda una pantalla contando aciertos y recordando el PRIMER error (para
 * diagnostico sin serie: distingue "sin PSRAM" de "SD ocupada/error"). */
static void cap_save(const char *path, int *ok, esp_err_t *first_err)
{
    esp_err_t e = screenshot_save_jpeg(path);
    if (e == ESP_OK) (*ok)++;
    else if (*first_err == ESP_OK) *first_err = e;
}

/* Apaga el switch y refleja el resultado (bajo lock LVGL; valida los objetos
 * por si la pagina Display se hubiera reconstruido durante la captura). */
static void capture_carousel_finish(ui_state_t *ui, int ok, esp_err_t first_err)
{
    if (lvgl_port_lock(1000)) {
        if (ui->capture_switch && lv_obj_is_valid(ui->capture_switch)) {
            lv_obj_clear_state(ui->capture_switch, LV_STATE_CHECKED);
        }
        if (ui->capture_status_lbl && lv_obj_is_valid(ui->capture_status_lbl)) {
            if (ok >= 8) {
                lv_label_set_text(ui->capture_status_lbl,
                                  "8/8 capturas guardadas en la SD");
            } else {
                const char *why = (first_err == ESP_ERR_NO_MEM) ? "sin PSRAM"
                                : (first_err == ESP_FAIL)        ? screenshot_last_error()
                                :                                  "error";
                lv_label_set_text_fmt(ui->capture_status_lbl,
                                      "%d/8 - fallo: %s", ok, why);
            }
        }
        lvgl_port_unlock();
    }
}

static void capture_carousel_task(void *arg)
{
    (void)arg;
    ui_state_t *ui = ui_get_state();
    const ui_view_mode_t saved_mode = ui->view_selection.mode;
    int ok = 0;
    esp_err_t first_err = ESP_OK;
    char path[96];

    const int total = ui_tour_screen_count();
    ESP_LOGI("CAPCAR", "Carrusel de captura: %d pantallas -> %s", total, TOUR_DIR);

    /* UN SOLO bucle sobre ui_tour_goto_screen: es la MISMA lista que usa la
     * pagina web /capturas, asi que una pantalla nueva en el tour entra aqui
     * sola. Antes habia una lista propia de 8 que se habia quedado corta: se
     * dejaba fuera el historico solar, la galeria, los 3 detalles de tarjeta y
     * TODAS las paginas de ajustes. El numero delante del nombre mantiene el
     * orden al listarlas. screenshot_save_jpeg ya crea el directorio padre bajo
     * camera_sd_bus_lock, no hace falta mkdir. 2026-07-26. */
    for (int i = 0; i < total; ++i) {
        const char *name = ui_tour_goto_screen(i);   /* ya hace tour_settle() */
        if (!name) continue;
        snprintf(path, sizeof(path), TOUR_DIR "/%02d_%s.jpg", i, name);
        cap_save(path, &ok, &first_err);
    }

    /* Restaurar: cerrar lo que quede abierto y volver a Live + la vista previa. */
    if (lvgl_port_lock(1000)) {
        ui_close_chart_screen();
        ui_close_battery_history_screen();
        ui_close_solar_history_screen();
        ui_gallery_close();
        ui_close_card_detail();
        ui->view_selection.mode = saved_mode;
        lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
        ensure_device_layout(ui, VICTRON_BLE_RECORD_TEST);
        lvgl_port_unlock();
    }

    ESP_LOGI("CAPCAR", "Carrusel terminado: %d/%d capturas (first_err=0x%x)",
             ok, total, (int)first_err);
    capture_carousel_finish(ui, ok, first_err);
    s_capture_running = false;
    vTaskDelete(NULL);
}

bool ui_capture_carousel_running(void)
{
    return s_capture_running;
}

void ui_start_capture_carousel(void)
{
    if (s_capture_running) {
        ESP_LOGW("CAPCAR", "Carrusel ya en curso, ignoro");
        return;
    }
    s_capture_running = true;
    if (xTaskCreate(capture_carousel_task, "cap_carousel", 12288, NULL, 3, NULL) != pdPASS) {
        s_capture_running = false;
        ESP_LOGE("CAPCAR", "No pude crear la tarea del carrusel");
    }
}

/* --- Navegacion por indice para las capturas por WiFi ---
 * Mapea un indice 0..(N-1) a una pantalla concreta, navega hasta ella (cerrando
 * antes cualquier overlay para partir de un estado limpio) y espera a que se
 * dibuje con tour_settle(). Reutiliza la misma logica que el auto-tour. Se
 * llama desde el handler HTTP /captura?n=<i> (config_server.c), que luego hace
 * screenshot_take_bmp() y devuelve el BMP. Devuelve el nombre corto de la
 * pantalla (para el nombre de fichero) o NULL si el indice esta fuera de rango. */
static const struct { ui_view_mode_t mode; const char *name; } TOUR_LIVE[] = {
    { UI_VIEW_MODE_OVERVIEW,        "overview"        },
    { UI_VIEW_MODE_DEFAULT_BATTERY, "bateria"         },
    { UI_VIEW_MODE_SOLAR_CHARGER,   "solar"           },
    { UI_VIEW_MODE_BATTERY_MONITOR, "monitor_bateria" },
    { UI_VIEW_MODE_INVERTER,        "inversor"        },
    { UI_VIEW_MODE_DCDC_CONVERTER,  "dcdc"            },
};
/* Detalle de tarjeta: solo estas 3 categorias pintan algo (ver el switch de
 * ui_show_card_detail); el resto cae al default y saldria vacio. */
static const struct { victron_record_type_t cat; const char *name; } TOUR_CARD[] = {
    { VICTRON_BLE_RECORD_SOLAR_CHARGER,   "detalle_solar"   },
    { VICTRON_BLE_RECORD_BATTERY_MONITOR, "detalle_bateria" },
    { VICTRON_BLE_RECORD_DCDC_CONVERTER,  "detalle_dcdc"    },
};
/* MISMO ORDEN que las llamadas a settings_menu_add_entry() en settings_panel.c:
 * el indice del tour es el orden de registro en s_page_ctxs, no el visual.
 * Faltaban "Tarjeta SD" y "Autocaravana", y eso no dejaba dos capturas sin
 * nombre: DESPLAZABA todas las de detras, asi que la pagina de Tarjeta SD se
 * guardaba como "sonido", la de Sonido como "victron_keys", etc. Si se anade una
 * entrada al menu, hay que anadirla AQUI en su sitio. 2026-07-26. */
static const char *TOUR_SET_NAMES[] = {
    "frigo",         /* Opciones Frigo        */
    "logs",          /* Historial en graficos */
    "wifi",          /* Wi-Fi                 */
    "display",       /* Pantalla              */
    "tarjeta_sd",    /* Tarjeta SD            */
    "sonido",        /* Sonido y alertas      */
    "autocaravana",  /* Autocaravana          */
    "victron_keys",  /* Victron Keys          */
    "about",         /* Acerca de             */
};
#define TOUR_N_LIVE   ((int)(sizeof(TOUR_LIVE) / sizeof(TOUR_LIVE[0])))
#define TOUR_N_CARD   ((int)(sizeof(TOUR_CARD) / sizeof(TOUR_CARD[0])))
#define TOUR_I_BATLOG  (TOUR_N_LIVE)              /* 6  */
#define TOUR_I_FRIGLOG (TOUR_N_LIVE + 1)          /* 7  */
#define TOUR_I_SOLLOG  (TOUR_N_LIVE + 2)          /* 8  historico solar */
#define TOUR_I_GALLERY (TOUR_N_LIVE + 3)          /* 9  galeria */
#define TOUR_I_CARD0   (TOUR_N_LIVE + 4)          /* 10..12 detalles de tarjeta */
#define TOUR_I_SETMAIN (TOUR_I_CARD0 + TOUR_N_CARD)  /* 13 */
#define TOUR_I_SETSUB0 (TOUR_I_SETMAIN + 1)          /* 14 */

int ui_tour_screen_count(void)
{
    return TOUR_I_SETSUB0 + ui_settings_panel_page_count();
}

const char *ui_tour_goto_screen(int idx)
{
    ui_state_t *ui = ui_get_state();
    if (idx < 0 || idx >= ui_tour_screen_count()) return NULL;

    /* Partir siempre de estado limpio: cerrar overlays abiertos. */
    if (lvgl_port_lock(1000)) {
        ui_close_chart_screen();
        ui_close_battery_history_screen();
        ui_close_solar_history_screen();
        ui_gallery_close();
        ui_close_card_detail();
        lvgl_port_unlock();
    }

    const char *name = "pantalla";
    if (idx < TOUR_N_LIVE) {
        tour_set_view(ui, TOUR_LIVE[idx].mode);
        name = TOUR_LIVE[idx].name;
    } else if (idx == TOUR_I_BATLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_battery_history_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_bateria";
    } else if (idx == TOUR_I_FRIGLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_chart_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_frigo";
    } else if (idx == TOUR_I_SOLLOG) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_solar_history_screen(ui);
            lvgl_port_unlock();
        }
        name = "log_solar";
    } else if (idx == TOUR_I_GALLERY) {
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_gallery_open();
            lvgl_port_unlock();
        }
        name = "galeria";
    } else if (idx >= TOUR_I_CARD0 && idx < TOUR_I_CARD0 + TOUR_N_CARD) {
        int c = idx - TOUR_I_CARD0;
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, 0, LV_ANIM_OFF);
            ui_show_card_detail(ui, TOUR_CARD[c].cat);
            lvgl_port_unlock();
        }
        name = TOUR_CARD[c].name;
    } else if (idx == TOUR_I_SETMAIN) {
        if (lvgl_port_lock(1000)) {
            ui_settings_panel_go_to_main();
            lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
            lvgl_port_unlock();
        }
        name = "ajustes";
    } else {
        int s = idx - TOUR_I_SETSUB0;
        if (lvgl_port_lock(1000)) {
            lv_tabview_set_act(ui->tabview, ui->tab_settings_index, LV_ANIM_OFF);
            ui_settings_panel_show_page(s);
            lvgl_port_unlock();
        }
        name = (s < (int)(sizeof(TOUR_SET_NAMES) / sizeof(TOUR_SET_NAMES[0])))
                   ? TOUR_SET_NAMES[s] : "ajustes";
    }

    tour_settle();
    return name;
}
