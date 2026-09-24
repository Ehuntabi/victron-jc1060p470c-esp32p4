#ifndef UI_UI_CARD_H
#define UI_UI_CARD_H

#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Paleta semántica (estilo Venus OS) ─────────────────────────── */
#define UI_COLOR_BG           lv_color_hex(0x0A0D13)  /* fondo de pagina (22-sep-2026: antes
                                                        * 0x06080C, se confundia con la tarjeta) */
#define UI_COLOR_CARD         lv_color_hex(0x1B2230)  /* card, mas clara que el fondo (antes
                                                        * 0x141821: se veian casi iguales) */
#define UI_COLOR_CARD_BORDER  lv_color_hex(0x39424F)
#define UI_COLOR_TEXT         lv_color_hex(0xFFFFFF)
#define UI_COLOR_TEXT_DIM     lv_color_hex(0x8A93A6)  /* SOLO para "no hay dato":
                                                        * el "--" de un metrico vacio y
                                                        * los estados apagados */
#define UI_COLOR_TEXT_SOFT    lv_color_hex(0xE4E9F0)  /* texto descriptivo (rotulos,
                                                        * subtitulos, pistas): casi
                                                        * blanco y legible en el panel
                                                        * (23-sep-2026; antes iban en
                                                        * TEXT_DIM y no se leian) */
#define UI_COLOR_CYAN         lv_color_hex(0x4FC3F7)
#define UI_COLOR_GREEN        lv_color_hex(0x00C851)
#define UI_COLOR_ORANGE       lv_color_hex(0xFF9800)
#define UI_COLOR_RED          lv_color_hex(0xFF4444)
#define UI_COLOR_RED_DARK     lv_color_hex(0xCC3333)
#define UI_COLOR_YELLOW       lv_color_hex(0xFFD54F)
#define UI_COLOR_BLUE         lv_color_hex(0x4FC3F7)
#define UI_COLOR_VIOLET       lv_color_hex(0xB388FF)  /* card camper (zona habitable) */
#define UI_COLOR_ICE          lv_color_hex(0x64B5F6)  /* card frigo (congelador, frio) */

#define UI_RADIUS_CARD        16
#define UI_PAD_CARD           20
#define UI_GAP_CARD           16

/* ── Card contenedor con borde de color por rol ─────────────────── */
/* Devuelve un objeto LVGL configurado como card vertical (flex column,
 * pad UI_PAD_CARD, gap UI_GAP_CARD, bg UI_COLOR_CARD, border de 3 px del
 * color indicado, radius UI_RADIUS_CARD). El caller añade hijos. */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_color_t border_color);

/* Dispara un pulso visual breve (~600 ms) sobre la sombra del card para
 * indicar actividad (p. ej. recepcion de un nuevo BLE record). Cancela
 * cualquier animacion previa para evitar acumulacion. */

/* Cabecera de card: icono UTF-8 (puede ser LV_SYMBOL_*), título y color de
 * acento. Devuelve el contenedor del header — el caller puede añadir un
 * pill u otros widgets a la derecha (alineación SPACE_BETWEEN). */
lv_obj_t *ui_card_set_title(lv_obj_t *card, const char *icon_utf8,
                            const char *title, lv_color_t accent);

/* v3.10: pasa el titulito de una tarjeta al estilo unico (cabecera a lo ancho,
 * titulo CENTRADO y linea fina del acento debajo, el de las vistas). Se le pasa
 * la etiqueta que la pagina ya tiene: se reparenta a la cabecera y esta queda
 * como primer hijo de la tarjeta, con el resto del contenido debajo.
 * Devuelve la cabecera, por si la pagina quiere anadir algo al lado del titulo. */
lv_obj_t *ui_card_wrap_title(lv_obj_t *card, lv_obj_t *title, lv_color_t accent);

/* Igual, pero dejando un CONTROL en la misma fila de la cabecera (interruptor,
 * boton pequeno). Para que el titulo siga centrado en la tarjeta se mete delante
 * un espaciador del mismo ancho que el control: asi no hay que bajar el control a
 * una fila aparte y la tarjeta NO crece. El control debe tener ancho estable
 * (un interruptor o un boton); si cambia de ancho con el dato (un valor que pasa
 * de "9 %" a "100 %"), mejor dejarlo en el cuerpo. */
lv_obj_t *ui_card_wrap_title_with(lv_obj_t *card, lv_obj_t *title,
                                  lv_color_t accent, lv_obj_t *control);

/* Variante con icono raster (lv_img_dsc_t embebido) — mismas reglas que
 * ui_card_set_title pero usa una imagen 64x64 (o lo que indique el dsc) en
 * lugar de un glifo de fuente. */
lv_obj_t *ui_card_set_title_img(lv_obj_t *card, const lv_img_dsc_t *img_src,
                                const char *title, lv_color_t accent);

/* ── Métrica: label pequeño + valor grande + unidad ─────────────── */
/* Crea un contenedor flex column con: label_text (font_20, dim) arriba,
 * fila inferior con valor (font_28, blanco) y unidad (font_20, dim).
 * Acceso interno por el orden de hijos (no usar custom user_data). */
lv_obj_t *ui_metric_create(lv_obj_t *parent, const char *label_text);

/* Variante compacta para cards estrechas (default_battery 31% ancho, etc.):
 * rotulo font_24, valor font_46, unidad font_24 (aprovecha el alto que sobre
 * al no compartir fila). Mismo layout y API que ui_metric_set. */
lv_obj_t *ui_metric_create_compact(lv_obj_t *parent, const char *label_text);

/* Actualiza valor y unidad de una métrica creada con ui_metric_create.
 * Si value_color es lv_color_hex(0) se usa UI_COLOR_TEXT. */
void ui_metric_set(lv_obj_t *metric, const char *value_str,
                   const char *unit_str, lv_color_t value_color);

/* Cambia dinámicamente el texto y color del label superior de una métrica
 * (útil para rótulos que cambian según estado: "Alternador"/"Bat. motor"). */
void ui_metric_set_label(lv_obj_t *metric, const char *label_text,
                         lv_color_t label_color);

/* Cambia la fuente del valor de una métrica. Necesario porque la fuente
 * grande (montserrat_46) solo trae digitos: para textos con letras
 * (p.ej. "APAGADO") hay que pasar a una fuente con alfabeto. */
void ui_metric_set_value_font(lv_obj_t *metric, const lv_font_t *font);

/* ── Pill de estado (badge redondeado) ──────────────────────────── */
lv_obj_t *ui_pill_create(lv_obj_t *parent, const char *text, lv_color_t bg);
void ui_pill_set(lv_obj_t *pill, const char *text, lv_color_t bg);

/* ── Gauge SOC circular ─────────────────────────────────────────── */
/* Devuelve un contenedor cuadrado (size x size) con un arco de fondo
 * (gris) y otro indicador con color dinámico, más un label central
 * grande con el SOC% y un sublabel con el voltaje. */
lv_obj_t *ui_arc_soc_create(lv_obj_t *parent, lv_coord_t size);

/* Actualiza el arc SOC. soc_deci en décimas de % (0..1000), voltage_centi
 * en centivoltios. Aplica color por rango (verde/naranja/rojo). Si
 * soc_deci > 1000 se trata como "sin dato" y se muestra "--". */
void ui_arc_soc_set(lv_obj_t *arc_box, uint16_t soc_deci, uint16_t voltage_centi);

/* ── Gauge SOC tipo "pila vertical con relleno" ─────────────────── */
/* Widget con forma de pila: cuerpo rectangular + tapa superior. El
 * relleno crece de abajo arriba segun el SOC% y se colorea con los
 * mismos umbrales (verde/naranja/rojo). Encima del cuerpo aparece el
 * %; debajo del contenedor, el voltaje. Tamanos sugeridos: w=70 h=160.
 */
lv_obj_t *ui_battery_soc_create(lv_obj_t *parent,
                                lv_coord_t width, lv_coord_t height);
void      ui_battery_soc_set(lv_obj_t *bat_box,
                             uint16_t soc_deci, uint16_t voltage_centi);

/* ── Tanque visual estilo "depósito" para niveles de agua ──────── */
/* Widget rectangular grande con relleno que sube de abajo arriba.
 * El nivel se da en sondas 0..3 (sumatorio del bitmask del NE185).
 * accent_color es el color del relleno cuando está en estado normal;
 * cuando se acerca a alarma (lleno para grises o vacío para limpia)
 * se torna rojo automáticamente.
 *
 * label_text aparece arriba del widget (ej. "Agua limpia").
 * El % grande se muestra dentro del depósito.                        */
typedef enum {
    UI_TANK_CLEAN   = 0,  /* depósito vertical limpia (alerta vacío) */
    UI_TANK_GREY    = 1,  /* depósito vertical grises (alerta lleno) */
    UI_TANK_CLEAN_H = 2,  /* barra horizontal limpia (5 LEDs: 1R + 4G) */
    UI_TANK_GREY_H  = 3,  /* barra horizontal grises (1 LED rojo on/off) */
} ui_tank_kind_t;

lv_obj_t *ui_tank_create(lv_obj_t *parent, lv_coord_t width, lv_coord_t height,
                         const char *label_text, lv_color_t accent_color,
                         ui_tank_kind_t kind);
void      ui_tank_set(lv_obj_t *tank_box, uint8_t level_0_to_3);

/* ── Helpers de color por rango ─────────────────────────────────── */
lv_color_t ui_color_for_soc(uint16_t soc_deci);
lv_color_t ui_color_for_current(int32_t milli);

#ifdef __cplusplus
}
#endif

#endif /* UI_UI_CARD_H */
