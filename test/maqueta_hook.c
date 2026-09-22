/* ── COMPROBADOR DE MAQUETACION (auditoria 21-sep-2026) ──────────────────────
 * Recorre el arbol de objetos de CADA pantalla y mide si algo se sale del panel
 * (1024x600). Distingue lo que se sale DENTRO de algo con scroll (que es
 * legitimo: para eso esta el scroll) de lo que se sale sin remedio. */
#include "display.h"

static int fuera_total = 0;
static const char *nombre_clase(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class))  return "etiqueta";
    if (lv_obj_check_type(o, &lv_btn_class))    return "boton";
    if (lv_obj_check_type(o, &lv_img_class))    return "imagen";
    if (lv_obj_check_type(o, &lv_slider_class)) return "slider";
    if (lv_obj_check_type(o, &lv_chart_class))  return "grafica";
    if (lv_obj_check_type(o, &lv_table_class))  return "tabla";
    if (lv_obj_check_type(o, &lv_tabview_class))return "pestanas";
    return "contenedor";
}

static bool tiene_scroll_arriba(lv_obj_t *o)
{
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) return true;
        if (lv_obj_check_type(p, &lv_tabview_class)) return true;   /* las pestanas scrollean solas */
    }
    return false;
}

static void medir(lv_obj_t *o, const char *pantalla, int *n, int *fuera, int *scrol)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    (*n)++;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    bool se_sale = (a.x1 < 0 || a.y1 < 0 || a.x2 > BSP_LCD_H_RES - 1 || a.y2 > BSP_LCD_V_RES - 1);
    /* los objetos de tamaño "content" sin contenido aun no cuentan */
    if (se_sale && lv_obj_get_width(o) > 0 && lv_obj_get_height(o) > 0) {
        if (tiene_scroll_arriba(o)) {
            (*scrol)++;
        } else {
            (*fuera)++;
            if (*fuera <= 6) {
                ESP_LOGW("MAQUETA", "  %s: %s en (%d,%d)-(%d,%d) SE SALE sin scroll",
                         pantalla, nombre_clase(o), (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
            }
        }
    }
    uint32_t hijos = lv_obj_get_child_cnt(o);
    for (uint32_t i = 0; i < hijos; i++) medir(lv_obj_get_child(o, i), pantalla, n, fuera, scrol);
}

static void revisar(const char *nombre)
{
    lv_obj_t *scr = lv_scr_act();
    int n = 0, fuera = 0, scrol = 0;
    medir(scr, nombre, &n, &fuera, &scrol);
    fuera_total += fuera;
    ESP_LOGI("MAQUETA", "%-22s objetos=%-4d se salen=%-3d (de esos, con scroll: %d)%s",
             nombre, n, fuera, scrol, fuera ? "   <-- REVISAR" : "");
}

static void maqueta_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(35000));      /* que la UI este montada */
    ESP_LOGI("MAQUETA", "=== midiendo la maquetacion de cada pantalla (panel %dx%d) ===",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    ui_state_t *ui = ui_get_state();
    if (!ui) { ESP_LOGE("MAQUETA", "sin estado de UI"); vTaskDelete(NULL); return; }

    /* Las pestanas principales, una a una */
    if (ui->tabview) {
        const char *nombres[] = { "pestana 1", "pestana 2", "pestana 3", "pestana 4", "pestana 5" };
        for (int i = 0; i < 5; i++) {
            if (lvgl_port_lock(3000)) {
                lv_tabview_set_act(ui->tabview, i, LV_ANIM_OFF);
                lvgl_port_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(900));   /* que se redibuje */
            if (lvgl_port_lock(3000)) { revisar(nombres[i]); lvgl_port_unlock(); }
        }
    }
    /* Y las pantallas que se abren por codigo */
    struct { const char *nombre; void (*abrir)(ui_state_t *); void (*cerrar)(void); } extras[] = {
        { "grafica",             ui_show_chart_screen,           ui_close_chart_screen },
        { "historico bateria",   ui_show_battery_history_screen, ui_close_battery_history_screen },
        { "historico solar",     ui_show_solar_history_screen,   ui_close_solar_history_screen },
    };
    for (size_t i = 0; i < sizeof(extras)/sizeof(extras[0]); i++) {
        if (lvgl_port_lock(3000)) { extras[i].abrir(ui); lvgl_port_unlock(); }
        vTaskDelay(pdMS_TO_TICKS(1200));
        if (lvgl_port_lock(3000)) { revisar(extras[i].nombre); lvgl_port_unlock(); }
        if (lvgl_port_lock(3000)) { extras[i].cerrar(); lvgl_port_unlock(); }
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    ESP_LOGI("MAQUETA", "=== fin: %d objetos fuera de pantalla sin scroll ===", fuera_total);
    vTaskDelete(NULL);
}
