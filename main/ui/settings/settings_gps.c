/* Pagina "GPS" de Ajustes: estado y posicion, puesta en hora, y las tramas en
 * crudo para diagnosticar.
 *
 * No hay ajustes de puerto a proposito (decision del usuario, 23-ago-2026): el
 * modulo es un NEO-M9N fijo en UART2 a 38400, y una casilla de velocidad solo
 * serviria para dejarlo mudo por un dedazo.
 *
 * Las tramas en crudo estan aqui y no escondidas porque son lo PRIMERO que hace
 * falta cuando el GPS no da posicion: si no llega nada es el cable, si llegan
 * lineas ilegibles es la velocidad, y si llegan bien pero sin datos es que
 * todavia esta buscando satelites. Sin verlas, los tres casos se parecen.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "gps/gps.h"

#include <stdio.h>
#include <lvgl.h>

/* Etiquetas que se refrescan solas. Estaticas de fichero porque el timer las
 * necesita fuera de la funcion que las crea. */
static lv_obj_t *s_estado;
static lv_obj_t *s_pos;
static lv_obj_t *s_hora;
static lv_obj_t *s_crudo;

static lv_obj_t *fila(lv_obj_t *page, const char *titulo, uint32_t color)
{
    lv_obj_t *t = lv_label_create(page);
    lv_label_set_text(t, titulo);
    lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_top(t, 12, 0);

    lv_obj_t *v = lv_label_create(page);
    lv_obj_set_style_text_color(v, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_width(v, lv_pct(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v, "--");
    return v;
}

static void refresco_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_estado) return;

    gps_data_t g;
    gps_get(&g);

    /* Mismos tres estados que el icono de la barra, con el texto que explica
     * QUE HACER en cada uno -- que es lo que uno viene a buscar aqui. */
    if (!g.hay_datos) {
        lv_label_set_text(s_estado, "Sin senal del modulo.\n"
                                    "Revisa el cable: RX en GPIO 3, TX en GPIO 2.");
        lv_obj_set_style_text_color(s_estado, lv_color_hex(0xFF6666), 0);
    } else if (!g.hay_fix) {
        lv_label_set_text_fmt(s_estado, "Buscando satelites... (ve %u)\n"
                                        "Al aire libre tarda 1-2 min la primera vez.",
                              (unsigned)g.satelites);
        lv_obj_set_style_text_color(s_estado, lv_color_hex(0xFF9800), 0);
    } else {
        lv_label_set_text_fmt(s_estado, "Posicion fijada con %u satelites",
                              (unsigned)g.satelites);
        lv_obj_set_style_text_color(s_estado, lv_color_hex(0x4CD964), 0);
    }

    if (g.hay_fix) {
        /* Seis decimales: son ~11 cm, de sobra. Con menos, dos plazas de
         * aparcamiento contiguas saldrian en el mismo sitio. */
        lv_label_set_text_fmt(s_pos, "Latitud:   %.6f\nLongitud:  %.6f\nAltitud:   %.0f m",
                              g.lat, g.lon, (double)g.altitud_m);
    } else {
        lv_label_set_text(s_pos, "--");
    }

    uint32_t n = gps_sincronizaciones();
    if (n == 0) {
        lv_label_set_text(s_hora, "El reloj todavia NO se ha puesto en hora con\n"
                                  "el GPS. Hace falta tener posicion primero.");
    } else {
        lv_label_set_text_fmt(s_hora, "Reloj puesto en hora con el GPS %lu vez%s.\n"
                                      "Se repasa cada 6 horas.",
                              (unsigned long)n, n == 1 ? "" : "es");
    }

    char buf[GPS_CRUDO_N * 96];
    size_t u = 0;
    buf[0] = 0;
    for (int i = 0; i < GPS_CRUDO_N; i++) {
        char l[96];
        gps_crudo_get(i, l, sizeof(l));
        if (!l[0]) continue;
        int w = snprintf(buf + u, sizeof(buf) - u, "%s\n", l);
        if (w < 0 || (size_t)w >= sizeof(buf) - u) break;
        u += (size_t)w;
    }
    lv_label_set_text(s_crudo, u ? buf : "(nada todavia)");
}

void create_gps_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    style_settings_scrollbar(page);

    s_estado = fila(page, "Estado", 0x4CD964);
    s_pos    = fila(page, "Posicion", 0x4FC3F7);
    s_hora   = fila(page, "Reloj", 0xFFD54F);

    s_crudo  = fila(page, "Tramas en crudo", 0x90A4AE);
    /* Monoespaciada no hay, pero la 14 deja caber una trama NMEA entera por
     * linea, que es lo que importa para leerlas. */
    lv_obj_set_style_text_font(s_crudo, &lv_font_montserrat_14, 0);

    static lv_timer_t *t;
    if (!t) t = lv_timer_create(refresco_cb, 1000, NULL);
    refresco_cb(NULL);
}
