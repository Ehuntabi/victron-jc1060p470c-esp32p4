/* ── DETECTOR DE SOLAPAMIENTOS EN AJUSTES (auditoria 22-sep-2026) ────────────
 * Abre CADA pagina de Ajustes por codigo y busca objetos que se pisen entre si
 * (dos hermanos visibles cuyos rectangulos se cruzan). El usuario veia el texto
 * "Portal web" encima del desplegable en Wi-Fi; esto lo mide en vez de mirarlo. */
#include "ui/settings/settings_panel.h"

static int solapes_total = 0;

static bool rectas_se_cruzan(lv_obj_t *a, lv_obj_t *b)
{
    lv_area_t ra, rb;
    lv_obj_get_coords(a, &ra);
    lv_obj_get_coords(b, &rb);
    /* margen de 2 px: tocarse no es pisarse */
    if (ra.x2 <= rb.x1 + 2 || rb.x2 <= ra.x1 + 2) return false;
    if (ra.y2 <= rb.y1 + 2 || rb.y2 <= ra.y1 + 2) return false;
    if (lv_obj_get_width(a) < 4 || lv_obj_get_height(a) < 4) return false;
    if (lv_obj_get_width(b) < 4 || lv_obj_get_height(b) < 4) return false;
    return true;
}

static const char *clase_de(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class))   return "etiqueta";
    if (lv_obj_check_type(o, &lv_dropdown_class))return "desplegable";
    if (lv_obj_check_type(o, &lv_btn_class))     return "boton";
    if (lv_obj_check_type(o, &lv_textarea_class))return "campo";
    if (lv_obj_check_type(o, &lv_switch_class))  return "interruptor";
    if (lv_obj_check_type(o, &lv_slider_class))  return "slider";
    return "contenedor";
}

static int s_max_y2 = 0;   /* hasta donde llega el contenido por abajo */
static char s_titulo[40] = "";

/* El primer texto con contenido de la pagina suele ser su titulo. */
static void buscar_titulo(lv_obj_t *padre)
{
    if (!padre || s_titulo[0] || lv_obj_has_flag(padre, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(padre, &lv_label_class)) {
        const char *s = lv_label_get_text(padre);
        if (s && s[0] && s[0] != '\n') {
            size_t k = 0;
            while (s[k] && k < sizeof(s_titulo) - 1 && s[k] != '\n') { s_titulo[k] = s[k]; k++; }
            s_titulo[k] = 0;
            return;
        }
    }
    uint32_t nh = lv_obj_get_child_cnt(padre);
    for (uint32_t i = 0; i < nh && !s_titulo[0]; i++) buscar_titulo(lv_obj_get_child(padre, i));
}

static void buscar_solapes(lv_obj_t *padre, const char *pagina, int *n)
{
    if (!padre || lv_obj_has_flag(padre, LV_OBJ_FLAG_HIDDEN)) return;
    {
        lv_area_t a; lv_obj_get_coords(padre, &a);
        if (a.y2 > s_max_y2 && lv_obj_get_height(padre) > 4) s_max_y2 = a.y2;
    }
    uint32_t nh = lv_obj_get_child_cnt(padre);
    for (uint32_t i = 0; i < nh; i++) {
        lv_obj_t *a = lv_obj_get_child(padre, i);
        if (!a || lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) continue;
        (*n)++;
        for (uint32_t j = i + 1; j < nh; j++) {
            lv_obj_t *b = lv_obj_get_child(padre, j);
            if (!b || lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) continue;
            if (rectas_se_cruzan(a, b)) {
                lv_area_t ra, rb;
                lv_obj_get_coords(a, &ra); lv_obj_get_coords(b, &rb);
                solapes_total++;
                ESP_LOGW("SOLAPE", "  %s: %s(%d,%d-%d,%d) PISA a %s(%d,%d-%d,%d)",
                         pagina, clase_de(a), (int)ra.x1, (int)ra.y1, (int)ra.x2, (int)ra.y2,
                         clase_de(b), (int)rb.x1, (int)rb.y1, (int)rb.x2, (int)rb.y2);
            }
        }
        buscar_solapes(a, pagina, n);
    }
}

static void solapes_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(40000));
    int paginas = ui_settings_panel_page_count();
    ESP_LOGI("SOLAPE", "=== revisando %d paginas de Ajustes ===", paginas);
    for (int i = 0; i < paginas; i++) {
        ui_settings_panel_show_page(i);
        vTaskDelay(pdMS_TO_TICKS(1600));
        char nombre[32]; snprintf(nombre, sizeof nombre, "pagina %d", i);
        int n = 0; s_max_y2 = 0; s_titulo[0] = 0;
        if (lvgl_port_lock(3000)) {
            buscar_titulo(lv_scr_act());
            buscar_solapes(lv_scr_act(), nombre, &n);
            lvgl_port_unlock();
        }
        ESP_LOGI("SOLAPE", "  pagina %d [%.28s]: %d objetos · y=%d%s",
                 i, s_titulo[0] ? s_titulo : "?", n, s_max_y2,
                 s_max_y2 > 599 ? "  <-- HAY QUE DESPLAZAR" : "  (entra)");
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ui_settings_panel_go_to_main();
    ESP_LOGI("SOLAPE", "=== fin: %d solapamientos en total ===", solapes_total);
    vTaskDelete(NULL);
}
