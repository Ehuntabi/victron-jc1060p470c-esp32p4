#ifndef UI_UI_CARD_H
#define UI_UI_CARD_H

#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Paleta semántica (estilo Venus OS) ─────────────────────────── */
#define UI_COLOR_BG           lv_color_hex(0x06080C)  /* casi negro con sutil azulado */
#define UI_COLOR_CARD         lv_color_hex(0x141821)  /* card oscuro frio */
#define UI_COLOR_CARD_BORDER  lv_color_hex(0x2D3340)
#define UI_COLOR_TEXT         lv_color_hex(0xFFFFFF)
#define UI_COLOR_TEXT_DIM     lv_color_hex(0x8A93A6)
#define UI_COLOR_CYAN         lv_color_hex(0x4FC3F7)
#define UI_COLOR_GREEN        lv_color_hex(0x00C851)
#define UI_COLOR_ORANGE       lv_color_hex(0xFF9800)
#define UI_COLOR_RED          lv_color_hex(0xFF4444)
#define UI_COLOR_RED_DARK     lv_color_hex(0xCC3333)
#define UI_COLOR_YELLOW       lv_color_hex(0xFFD54F)
#define UI_COLOR_BLUE         lv_color_hex(0x4FC3F7)

#define UI_RADIUS_CARD        16
#define UI_PAD_CARD           20
#define UI_GAP_CARD           16

/* ── Card contenedor con borde de color por rol ─────────────────── */
/* Devuelve un objeto LVGL configurado como card vertical (flex column,
 * pad UI_PAD_CARD, gap UI_GAP_CARD, bg UI_COLOR_CARD, border de 2 px del
 * color indicado, radius UI_RADIUS_CARD). El caller añade hijos. */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_color_t border_color);

/* Dispara un pulso visual breve (~600 ms) sobre la sombra del card para
 * indicar actividad (p. ej. recepcion de un nuevo BLE record). Cancela
 * cualquier animacion previa para evitar acumulacion. */
void ui_card_pulse(lv_obj_t *card);

/* Cabecera de card: icono UTF-8 (puede ser LV_SYMBOL_*), título y color de
 * acento. Devuelve el contenedor del header — el caller puede añadir un
 * pill u otros widgets a la derecha (alineación SPACE_BETWEEN). */
lv_obj_t *ui_card_set_title(lv_obj_t *card, const char *icon_utf8,
                            const char *title, lv_color_t accent);

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
 * label font_14, valor font_24, unidad font_14. Mismo layout y API que
 * ui_metric_set. */
lv_obj_t *ui_metric_create_compact(lv_obj_t *parent, const char *label_text);

/* Variante grande para cards que ocupan todo el ancho de pantalla:
 * label font_24, valor font_46 (sin acentos, glifos solo ASCII), unidad
 * font_24. Para los valores numéricos típicos (12.84, +1.2, etc.). */
lv_obj_t *ui_metric_create_large(lv_obj_t *parent, const char *label_text);

/* Actualiza valor y unidad de una métrica creada con ui_metric_create.
 * Si value_color es lv_color_hex(0) se usa UI_COLOR_TEXT. */
void ui_metric_set(lv_obj_t *metric, const char *value_str,
                   const char *unit_str, lv_color_t value_color);

/* Cambia dinámicamente el texto y color del label superior de una métrica
 * (útil para rótulos que cambian según estado: "Alternador"/"Bat. motor"). */
void ui_metric_set_label(lv_obj_t *metric, const char *label_text,
                         lv_color_t label_color);

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
    UI_TANK_CLEAN  = 0,   /* alerta cuando vacio (level == 0) */
    UI_TANK_GREY   = 1,   /* alerta cuando lleno (level == 3) */
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
