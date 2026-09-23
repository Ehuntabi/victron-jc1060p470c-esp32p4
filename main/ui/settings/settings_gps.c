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
#include "ui/widgets/ui_card.h"     /* paleta compartida (UI_COLOR_CARD / _BORDER) */
#include "fonts/fonts_es.h"     /* tipografia unica de la app (Inter con acentos) */
#include "gps/gps.h"
#include "data/trip_computer.h"

#include <stdio.h>
#include <lvgl.h>

/* El fondo y el borde de las tarjetas salen de la paleta compartida (v3.7).
 * Antes eran dos colores propios de antes del cambio de contraste de la v3.6
 * (0x191D25 y 0x262C37): el borde quedo mas apagado que el fondo nuevo de la
 * pagina y las tarjetas parecian no tener marco. Los de texto y estado se
 * quedan como estaban. */
#define COL_CARD     UI_COLOR_CARD
#define COL_TEXTO    0xECEFF3
#define COL_APAGADO  0x8B94A3
#define COL_VERDE    0x4CD964
#define COL_AMBAR    0xFF9800
#define COL_ROJO     0xFF5A5A
#define COL_AZUL     0x4FC3F7
#define COL_MORADO   0xB388FF

static lv_obj_t *s_punto;      /* circulo de color del estado */
static lv_obj_t *s_estado;     /* "Posición fijada" / ... */
static lv_obj_t *s_nota;       /* la linea de debajo: que hacer */
static lv_obj_t *s_sats;       /* el numero grande */
static lv_obj_t *s_sats_lbl;   /* "satelites" / "a la vista" */
static lv_obj_t *s_pos;
static lv_obj_t *s_hora;
static lv_obj_t *s_crudo;
static lv_obj_t *s_tarjeta_estado;   /* la franja de arriba: su marco va del color del estado */

/* Tarjeta con su titulito en versalitas. Devuelve el cuerpo, donde va todo.
 * El marco es de 2 px y del color que se le pasa (v3.7): con el gris neutro de
 * la paleta y 1 px no se leia como marco en el panel. Cada tarjeta lleva el
 * suyo, como en el resto de paginas de Ajustes. */
static lv_obj_t *tarjeta(lv_obj_t *padre, const char *titulo, lv_coord_t alto, uint32_t acento)
{
    lv_obj_t *c = lv_obj_create(padre);
    lv_obj_set_size(c, lv_pct(100), alto);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(acento), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    if (titulo) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, titulo);
        lv_obj_set_style_text_color(t, UI_COLOR_TEXT_SOFT, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_letter_space(t, 2, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

static void refresco_cb(lv_timer_t *t)
{
    (void)t;
    /* s_estado no se pone a NULL al salir de la pagina (sigue viva, solo
     * reparentada al storage oculto del lv_menu -- ver lv_menu_set_page en
     * lv_menu.c). Sin comprobar visibilidad, este timer de 1 Hz seguia
     * corriendo PARA SIEMPRE tras la primera visita a Ajustes -> GPS, aunque
     * llevaras horas en otra pantalla: gps_get() y los 6 lv_label_set_text
     * cada segundo, sin que nadie los viera. Mismo patron ya usado (y
     * efectivo, comprobado contra lv_menu.c) en sd_trip_timer_cb
     * (settings_panel.c). Detectado auditando el 08-sep-2026. */
    if (!s_estado || !lv_obj_is_visible(s_estado)) return;

    gps_data_t g;
    gps_get(&g);

    /* Los tres estados. Cada uno dice QUE HACER, no solo que pasa: es a lo que
     * se viene a esta pantalla cuando el GPS no va. */
    uint32_t col;
    if (!g.hay_datos) {
        col = COL_ROJO;
        lv_label_set_text(s_estado, "Sin señal del módulo");
        lv_label_set_text(s_nota, "Revisa el cable: RX en GPIO 3, TX en GPIO 2.\n"
                                  "Si no llega ni una trama, no es cuestión de esperar.");
        lv_label_set_text(s_sats, "--");
        lv_label_set_text(s_sats_lbl, "SATÉLITES");
    } else if (!g.hay_fix) {
        col = COL_AMBAR;
        lv_label_set_text(s_estado, "Buscando satélites");
        lv_label_set_text(s_nota, "Al aire libre tarda uno o dos minutos la primera vez.\n"
                                  "Bajo techo o entre edificios puede no llegar a fijar.");
        lv_label_set_text_fmt(s_sats, "%u", (unsigned)g.satelites);
        lv_label_set_text(s_sats_lbl, "A LA VISTA");
    } else {
        col = COL_VERDE;
        lv_label_set_text(s_estado, "Posición fijada");
        uint32_t n = gps_sincronizaciones();
        if (n == 0) lv_label_set_text(s_nota, "El reloj todavía no se ha puesto en hora con el GPS.");
        else        lv_label_set_text_fmt(s_nota, "Reloj puesto en hora con el GPS %lu vez%s.\n"
                                                  "Se repasa cada 6 horas.",
                                          (unsigned long)n, n == 1 ? "" : "es");
        lv_label_set_text_fmt(s_sats, "%u", (unsigned)g.satelites);
        lv_label_set_text(s_sats_lbl, "SATÉLITES");
    }
    lv_obj_set_style_bg_color(s_punto, lv_color_hex(col), 0);
    /* El marco de la franja de arriba es el mismo aviso que el punto y el
     * titulo: de un vistazo, sin leer, ya se ve de que color esta el GPS. */
    if (s_tarjeta_estado) lv_obj_set_style_border_color(s_tarjeta_estado, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_estado, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_sats, lv_color_hex(col), 0);

    /* El recorrido del viaje se enseña AQUI y no en la tarjeta de energia: es un
     * dato del GPS, y este es el sitio al que se viene a comprobar que el GPS
     * hace lo suyo. Ver el contador subir mientras se conduce es lo unico que
     * confirma el odometro sin esperar al resumen del viaje. */
    trip_computer_t tc;      /* 't' ya es el lv_timer_t del callback */
    trip_computer_get(&tc);
    /* La POTENCIA es lo que se viene a mirar cuando se prueba un sitio nuevo
     * (dentro de un cajon, bajo una claraboya): por debajo de 30 dB-Hz no se
     * fija posicion, a 40 o mas va sobrado. Se da la del mejor satelite Y la
     * media a proposito: uno recien asomado por el horizonte da 20 y hunde la
     * media sin que pase nada malo, asi que la media sola engana.
     * Para medir cuanto tapa un sitio: se mira fuera, se mira dentro, y la
     * resta es la perdida. */
    char pot[72];
    if (g.snr_cuantos > 0) {
        snprintf(pot, sizeof(pot), "Señal      %u dB-Hz el mejor, %u de media (%u sat.)",
                 (unsigned)g.snr_mejor, (unsigned)g.snr_medio, (unsigned)g.snr_cuantos);
    } else {
        snprintf(pot, sizeof(pot), "Señal      -- (ningún satélite a la vista)");
    }

    if (g.hay_fix) {
        /* Seis decimales son ~11 cm. Con menos, dos plazas de aparcamiento
         * contiguas saldrian en el mismo sitio. */
        lv_label_set_text_fmt(s_pos,
                              "Latitud    %.6f\nLongitud   %.6f\nAltitud    %.0f m\n"
                              "%s\nRecorrido  %.1f km  (este viaje)",
                              g.lat, g.lon, (double)g.altitud_m, pot, tc.km);
    } else {
        lv_label_set_text_fmt(s_pos,
                              "Latitud    --\nLongitud   --\nAltitud    --\n"
                              "%s\nRecorrido  %.1f km  (este viaje)", pot, tc.km);
    }

    if (g.hora[0]) lv_label_set_text_fmt(s_hora, "%s  UTC\n%s", g.hora, g.fecha);
    else           lv_label_set_text(s_hora, "--\n--");

    char buf[GPS_CRUDO_N * 96];
    size_t u = 0;
    buf[0] = 0;
    /* Solo las ULTIMAS tramas: la tarjeta mide 210 px y antes se le metian todas
     * las del anillo (~40 lineas), que se salian de la tarjeta y estiraban la
     * pagina hasta y=1387 (medido con la sonda el 22-sep-2026). Cinco bastan para
     * ver si el modulo habla y son las que caben en el presupuesto de la pagina
     * (23-sep-2026). */
    const int VISIBLES = 5;
    for (int i = (GPS_CRUDO_N > VISIBLES ? GPS_CRUDO_N - VISIBLES : 0); i < GPS_CRUDO_N; i++) {
        char l[96];
        gps_crudo_get(i, l, sizeof(l));
        if (!l[0]) continue;
        int w = snprintf(buf + u, sizeof(buf) - u, "%s\n", l);
        if (w < 0 || (size_t)w >= sizeof(buf) - u) break;
        u += (size_t)w;
    }
    lv_label_set_text(s_crudo, u ? buf : "(nada todavía)");
}

void create_gps_settings_page(ui_state_t *ui, lv_obj_t *page)
{
    (void)ui;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    /* Presupuesto de altura MEDIDO el 23-sep sobre la captura: la zona util de
     * una pagina de Ajustes son 476 px (el panel llega a 538 y la barra de abajo
     * empieza en 550). Antes el contenido pedia 534 y la tarjeta de TRAMAS
     * EN CRUDO se metia debajo de la barra: se veia cortada. Con el reparto de
     * abajo queda en 468 y entra entera. */
    lv_obj_set_style_pad_all(page, 4, 0);
    lv_obj_set_style_pad_row(page, 2, 0);
    /* 16 por la derecha: la barra de scroll (8 px de ancho + 6 de pad_right, ver
     * style_settings_scrollbar) vive en x=1010..1018, y las tarjetas, con solo 4
     * de margen, llegaban a 1020: le pasaban por debajo y la barra se comia su
     * borde derecho. Se vio al medir la captura del 22-sep (borde en 1017 con la
     * barra empezando en 1010; en la figura anterior el borde quedaba en 1005, o
     * sea limpio). Arriba y abajo el margen sigue en 4. */
    lv_obj_set_style_pad_right(page, 16, 0);
    style_settings_scrollbar(page);

    /* ── Estado: la franja de arriba ─────────────────────────────── */
    /* 96 y no 118: el contenido de la franja (titulo 34 + nota de dos lineas 36,
     * o los satelites 46 + rotulo) cabe en 88. */
    lv_obj_t *est = tarjeta(page, NULL, 96, COL_VERDE);
    s_tarjeta_estado = est;

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
    lv_obj_set_style_text_color(s_nota, UI_COLOR_TEXT_SOFT, 0);
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
    lv_obj_set_style_text_color(s_sats_lbl, UI_COLOR_TEXT_SOFT, 0);
    lv_obj_set_style_text_letter_space(s_sats_lbl, 2, 0);
    lv_label_set_text(s_sats_lbl, "SATÉLITES");
    lv_obj_align(s_sats_lbl, LV_ALIGN_TOP_RIGHT, 0, 56);

    /* ── Posicion y hora, repartiendose el ancho ─────────────────── */
    lv_obj_t *fila = lv_obj_create(page);
    lv_obj_remove_style_all(fila);
    /* 190 y no 150: con la linea del recorrido son CUATRO renglones de font 24
     * con 8 de interlineado (140 px) mas el hueco del titulito. A 150 la ultima
     * linea quedaba cortada por abajo -- y la tarjeta no hace scroll, asi que no
     * se veia que faltaba nada. */
    /* CINCO renglones de font 24 con 8 de interlineado: medido sobre la captura,
     * el bloque de datos va de +30 a +216 y con el relleno pide 226. La tarjeta no
     * hace scroll, asi que si no cabe se corta por abajo sin avisar. */
    lv_obj_set_size(fila, lv_pct(100), 226);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila, 6, 0);
    lv_obj_clear_flag(fila, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cpos = tarjeta(fila, "POSICIÓN", lv_pct(100), COL_AZUL);
    lv_obj_set_flex_grow(cpos, 3);
    s_pos = lv_label_create(cpos);
    lv_obj_set_style_text_font(s_pos, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_pos, lv_color_hex(COL_AZUL), 0);
    lv_obj_set_style_text_line_space(s_pos, 8, 0);
    lv_label_set_text(s_pos, "--");
    lv_obj_align(s_pos, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *chora = tarjeta(fila, "HORA DEL GPS", lv_pct(100), COL_MORADO);
    lv_obj_set_flex_grow(chora, 2);
    s_hora = lv_label_create(chora);
    lv_obj_set_style_text_font(s_hora, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_hora, lv_color_hex(COL_TEXTO), 0);
    lv_obj_set_style_text_line_space(s_hora, 8, 0);
    lv_label_set_text(s_hora, "--");
    lv_obj_align(s_hora, LV_ALIGN_TOP_LEFT, 0, 30);

    /* ── Tramas en crudo ─────────────────────────────────────────── */
    /* 134: titulo (19) + 5 tramas de font 14 (95) + rellenos. Con 6 no cabe en el
     * presupuesto de la pagina. */
    lv_obj_t *ctr = tarjeta(page, "TRAMAS EN CRUDO", 134, COL_APAGADO);
    s_crudo = lv_label_create(ctr);
    /* Letra 14 y no menos: es la mas pequena que hay compilada, y una trama
     * NMEA entera tiene que caber en una linea para poder leerla. No hay
     * tipografia de ancho fijo en el firmware, asi que no quedaran alineadas. */
    lv_obj_set_style_text_font(s_crudo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_crudo, lv_color_hex(COL_APAGADO), 0);
    lv_label_set_text(s_crudo, "(nada todavía)");
    lv_obj_set_width(s_crudo, lv_pct(100));
    /* Recortar, no envolver: asi una trama larga no ocupa dos lineas ni descoloca */
    lv_label_set_long_mode(s_crudo, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_crudo, LV_ALIGN_TOP_LEFT, 0, 28);

    static lv_timer_t *t;
    if (!t) t = lv_timer_create(refresco_cb, 1000, NULL);
    refresco_cb(NULL);
}
