/* ── SONDA LIGERA: ¿cabe la pagina del GPS? (22-sep-2026) ────────────────────
 * Abre cada pagina de Ajustes, busca la que lleva el cartel "TRAMAS EN CRUDO"
 * (esa es la del GPS) y mide hasta donde llega su contenido. Solo alturas: sin
 * comparar parejas de objetos, que era lo que ponia lenta la interfaz. */
#include "ui/settings/settings_panel.h"

static int  s_max_y2 = 0;
static bool s_es_gps  = false;

static void medir_alto(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    if (lv_obj_get_height(o) > 4 && a.y2 > s_max_y2) s_max_y2 = a.y2;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *s = lv_label_get_text(o);
        if (s && strstr(s, "TRAMAS EN CRUDO")) s_es_gps = true;
    }
    uint32_t c = lv_obj_get_child_cnt(o);
    for (uint32_t k = 0; k < c; k++) medir_alto(lv_obj_get_child(o, k));
}

static void gps_alto_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(40000));
    int paginas = ui_settings_panel_page_count();
    ESP_LOGI("GPSALTO", "=== buscando la pagina del GPS entre %d paginas ===", paginas);
    for (int i = 0; i < paginas; i++) {
        ui_settings_panel_show_page(i);
        vTaskDelay(pdMS_TO_TICKS(1200));
        s_max_y2 = 0; s_es_gps = false;
        if (lvgl_port_lock(3000)) { medir_alto(lv_scr_act()); lvgl_port_unlock(); }
        if (s_es_gps) {
            ESP_LOGI("GPSALTO", "pagina %d ES LA DEL GPS: el contenido llega a y=%d de 599 -> %s (%d px)",
                     i, s_max_y2, s_max_y2 > 599 ? "SOBRA" : "ENTRA", s_max_y2 - 599);
        }
    }
    ui_settings_panel_go_to_main();
    ESP_LOGI("GPSALTO", "fin");
    vTaskDelete(NULL);
}
