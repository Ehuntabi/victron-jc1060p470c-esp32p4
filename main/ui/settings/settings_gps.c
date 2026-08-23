/* Pagina "GPS" de Ajustes: estado, posicion, hora del modulo y tramas en crudo.
 *
 * Repartida en TARJETAS y no en una lista de etiquetas. La primera version era
 * una columna de rotulos uno debajo de otro y en 1024x600 se veia vacia: habia
 * que leerla entera para encontrar el dato. Aqui el estado se lleva la franja
 * de arriba con su color y el numero de satelites en grande, que es lo que uno
 * viene a mirar, y el resto se reparte el ancho.
 *
 * No hay ajustes de puerto a proposito (decision del usuario, 23-ago-2026): el
 * modulo es un NEO-M9N fijo en UART2 a 38400, y una casilla de velocidad solo
 * serviria para dejarlo mudo por un dedazo.
 *
 * Las tramas en crudo estan aqui y no escondidas porque son lo PRIMERO que hace
 * falta cuando el GPS no da posicion: si no llega nada es el cable, si llegan
 * ilegibles es la velocidad, y si llegan bien pero sin datos es que todavia
 * esta buscando. Sin verlas, los tres casos se parecen.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "gps/gps.h"

#include <stdio.h>
#include <lvgl.h>

#define COL_CARD     0x191D25
#define COL_BORDE    0x262C37
#define COL_TEXTO    0xECEFF3
#define COL_APAGADO  0x8B94A3
#define COL_VERDE    0x4CD964
#define COL_AMBAR    0xFF9800
#define COL_ROJO     0xFF5A5A
#define COL_AZUL     0x4FC3F7

static lv_obj_t *s_punto;      /* circulo de color del estado */
static lv_obj_t *s_estado;     /* "Posicion fijada" / ... */
static lv_obj_t *s_nota;       /* la linea de debajo: que hacer */
static lv_obj_t *s_sats;       /* el numero grande */
static lv_obj_t *s_sats_lbl;   /* "satelites" / "a la vista" */
static lv_obj_t *s_pos;
static lv_obj_t *s_hora;
static lv_obj_t *s_crudo;

/* Tarjeta con su titulito en versalitas. Devuelve el cuerpo, donde va todo. */
static lv_obj_t *tarjeta(lv_obj_t *padre, const char *titulo, lv_coord_t alto)
{
    lv_obj_t *c = lv_obj_create(padre);
    lv_obj_set_size(c, lv_pct(100), alto);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_BORDE), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    if (titulo) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, titulo);
        lv_obj_set_style_text_color(t, lv_color_hex(COL_APAGADO), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(t, 2, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

static void refresco_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_estado) return;

    gps_data_t g;
    gps_get(&g);

    /* Los tres estados. Cada uno dice QUE HACER, no solo que pasa: es a lo que
     * se viene a esta pantalla cuando el GPS no va. */
    uint32_t col;
    if (!g.hay_datos) {
        col = COL_ROJO;
        lv_label_set_text(s_estado, "Sin senal del modulo");
        lv_label_set_text(s_nota, "Revisa el cable: RX en GPIO 3, TX en GPIO 2.\n"
                                  "Si no llega ni una trama, no es cuestion de esperar.");
        lv_label_set_text(s_sats, "--");
        lv_label_set_text(s_sats_lbl, "SATELITES");
    } else if (!g.hay_fix) {
        col = COL_AMBAR;
        lv_label_set_text(s_estado, "Buscando satelites");
        lv_label_set_text(s_nota, "Al aire libre tarda uno o dos minutos la primera vez.\n"
                                  "Bajo techo o entre edificios puede no llegar a fijar.");
        lv_label_set_text_fmt(s_sats, "%u", (unsigned)g.satelites);
        lv_label_set_text(s_sats_lbl, "A LA VISTA");
    } else {
        col = COL_VERDE;
        lv_label_set_text(s_estado, "Posicion fijada");
        uint32_t n = gps_sincronizaciones();
        if (n == 0) lv_label_set_text(s_nota, "El reloj todavia no se ha puesto en hora con el GPS.");
        else        lv_label_set_text_fmt(s_nota, "Reloj puesto en hora con el GPS %lu vez%s.\n"
                                                  "Se repasa cada 6 horas.",
                                          (unsigned long)n, n == 1 ? "" : "es");
        lv_label_set_text_fmt(s_sats, "%u", (unsigned)g.satelites);
        lv_label_set_text(s_sats_lbl, "SATELITES");
    }
    lv_obj_set_style_bg_color(s_punto, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_estado, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_sats, lv_color_hex(col), 0);

    if (g.hay_fix) {
        /* Seis decimales son ~11 cm. Con menos, dos plazas de aparcamiento
         * contiguas saldrian en el mismo sitio. */
        lv_label_set_text_fmt(s_pos, "Latitud    %.6f\nLongitud   %.6f\nAltitud    %.0f m",
                              g.lat, g.lon, (double)g.altitud_m);
    } else {
        lv_label_set_text(s_pos, "Latitud    --\nLongitud   --\nAltitud    --");
    }

    if (g.hora[0]) lv_label_set_text_fmt(s_hora, "%s  UTC\n%s", g.hora, g.fecha);
    else           lv_label_set_text(s_hora, "--\n--");

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
    lv_obj_set_style_pad_row(page, 12, 0);
    style_settings_scrollbar(page);

    /* ── Estado: la franja de arriba ─────────────────────────────── */
    lv_obj_t *est = tarjeta(page, NULL, 118);

    s_punto = lv_obj_create(est);
    lv_obj_set_size(s_punto, 18, 18);
    lv_obj_set_style_radius(s_punto, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_punto, 0, 0);
    lv_obj_clear_flag(s_punto, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_punto, LV_ALIGN_TOP_LEFT, 0, 8);

    s_estado = lv_label_create(est);
    lv_obj_set_style_text_font(s_estado, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_estado, "--");
    lv_obj_align(s_estado, LV_ALIGN_TOP_LEFT, 30, 0);

    s_nota = lv_label_create(est);
    lv_obj_set_style_text_font(s_nota, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_nota, lv_color_hex(COL_APAGADO), 0);
    lv_label_set_text(s_nota, "");
    lv_obj_align(s_nota, LV_ALIGN_TOP_LEFT, 30, 42);

    /* Los satelites, a la derecha y en grande: junto al color del estado, es lo
     * que resume la situacion sin leer nada. */
    s_sats = lv_label_create(est);
    lv_obj_set_style_text_font(s_sats, &lv_font_montserrat_46, 0);
    lv_label_set_text(s_sats, "--");
    lv_obj_align(s_sats, LV_ALIGN_TOP_RIGHT, 0, -2);

    s_sats_lbl = lv_label_create(est);
    lv_obj_set_style_text_font(s_sats_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sats_lbl, lv_color_hex(COL_APAGADO), 0);
    lv_obj_set_style_text_letter_space(s_sats_lbl, 2, 0);
    lv_label_set_text(s_sats_lbl, "SATELITES");
    lv_obj_align(s_sats_lbl, LV_ALIGN_TOP_RIGHT, 0, 56);

    /* ── Posicion y hora, repartiendose el ancho ─────────────────── */
    lv_obj_t *fila = lv_obj_create(page);
    lv_obj_remove_style_all(fila);
    lv_obj_set_size(fila, lv_pct(100), 150);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila, 12, 0);
    lv_obj_clear_flag(fila, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cpos = tarjeta(fila, "POSICION", lv_pct(100));
    lv_obj_set_flex_grow(cpos, 3);
    s_pos = lv_label_create(cpos);
    lv_obj_set_style_text_font(s_pos, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_pos, lv_color_hex(COL_AZUL), 0);
    lv_obj_set_style_text_line_space(s_pos, 8, 0);
    lv_label_set_text(s_pos, "--");
    lv_obj_align(s_pos, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *chora = tarjeta(fila, "HORA DEL GPS", lv_pct(100));
    lv_obj_set_flex_grow(chora, 2);
    s_hora = lv_label_create(chora);
    lv_obj_set_style_text_font(s_hora, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_hora, lv_color_hex(COL_TEXTO), 0);
    lv_obj_set_style_text_line_space(s_hora, 8, 0);
    lv_label_set_text(s_hora, "--");
    lv_obj_align(s_hora, LV_ALIGN_TOP_LEFT, 0, 30);

    /* ── Tramas en crudo ─────────────────────────────────────────── */
    lv_obj_t *ctr = tarjeta(page, "TRAMAS EN CRUDO", 210);
    s_crudo = lv_label_create(ctr);
    /* Letra 14 y no menos: es la mas pequena que hay compilada, y una trama
     * NMEA entera tiene que caber en una linea para poder leerla. No hay
     * tipografia de ancho fijo en el firmware, asi que no quedaran alineadas. */
    lv_obj_set_style_text_font(s_crudo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_crudo, lv_color_hex(COL_APAGADO), 0);
    lv_label_set_text(s_crudo, "(nada todavia)");
    lv_obj_align(s_crudo, LV_ALIGN_TOP_LEFT, 0, 28);

    static lv_timer_t *t;
    if (!t) t = lv_timer_create(refresco_cb, 1000, NULL);
    refresco_cb(NULL);
}
