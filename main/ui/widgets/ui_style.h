#ifndef UI_UI_STYLE_H
#define UI_UI_STYLE_H

/* ── TOKENS DE LA GUIA DE ESTILO ─────────────────────────────────────────────
 * Fuente UNICA de verdad de colores, tipografia y medidas de la UI de la P4.
 * La guia esta en docs/GUIA_ESTILO.md y las reglas que la hacen cumplir en
 * test/auditar.sh (seccion 12).
 *
 * REGLAS (decididas el 4-oct-2026 midiendo las 24 pantallas con /captura):
 *   1. Un solo color por significado: aqui, y en ningun otro sitio.
 *      PROHIBIDO lv_color_hex() fuera de este fichero.
 *   2. Una sola familia (Inter) y cinco papeles: UI_FONT_*.
 *      PROHIBIDO lv_font_montserrat_* en el codigo de UI: el alias de
 *      fonts_es.h lo tapa, pero un fichero que no incluya esa cabecera se lleva
 *      la fuente del SDK, que solo tiene ASCII. Ademas el 36 no estaba aliasado
 *      y mantenia 56 KB de fuente compilada para dos estilos muertos.
 *   3. Medidas en la rejilla de 4: 4/8/12/16/20/24. Nada de 2, 3, 6, 10, 14.
 *
 * Los nombres viejos (UI_COLOR_*, UI_RADIUS_CARD, UI_PAD_CARD, UI_GAP_CARD) se
 * mantienen para no romper el codigo que ya los usa: ui_card.h incluye esto.
 * ------------------------------------------------------------------------- */

#include <lvgl.h>
#include "fonts/fonts_es.h"   /* Inter con acentos: la unica familia permitida */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 1. COLOR ────────────────────────────────────────────────────────────── */
#define UI_COLOR_BG           lv_color_hex(0x0A0D13)  /* fondo de pagina (22-sep-2026: antes
                                                        * 0x06080C, se confundia con la tarjeta) */
#define UI_COLOR_CARD         lv_color_hex(0x1B2230)  /* superficie de tarjeta (antes 0x141821) */
#define UI_COLOR_CARD_BORDER  lv_color_hex(0x39424F)
#define UI_COLOR_TEXT         lv_color_hex(0xFFFFFF)  /* dato principal */
#define UI_COLOR_TEXT_SOFT    lv_color_hex(0xE4E9F0)  /* texto descriptivo (rotulos, subtitulos) */
#define UI_COLOR_TEXT_DIM     lv_color_hex(0x8A93A6)  /* SOLO "no hay dato" (el "--" de un
                                                        * metrico vacio y los estados apagados) */

/* Colores de ESTADO (un tono por significado; los duplicados que habia en
 * pantalla estan listados en docs/GUIA_ESTILO.md §2 y se van colapsando). */
#define UI_COLOR_GREEN        lv_color_hex(0x00C851)  /* correcto   (antes tambien 00CA52/4CD964) */
#define UI_COLOR_YELLOW       lv_color_hex(0xFFD54F)  /* atencion   (antes tambien FFD74A) */
#define UI_COLOR_ORANGE       lv_color_hex(0xFF9800)  /* aviso      (antes FF9A00/FFAA00) */
#define UI_COLOR_RED          lv_color_hex(0xFF4444)  /* error      (antes FF595A) */
#define UI_COLOR_RED_DARK     lv_color_hex(0xCC3333)  /* error atenuado / fondo de aviso
                                                        * (antes B51C19/8C2021) */

/* Acentos de SECCION (uno por pantalla como maximo) */
#define UI_COLOR_CYAN         lv_color_hex(0x4FC3F7)  /* informacion / solar (antes 4AC2F7/008AD6) */
#define UI_COLOR_BLUE         lv_color_hex(0x4FC3F7)  /* alias historico de CYAN */
#define UI_COLOR_VIOLET       lv_color_hex(0xB388FF)  /* card camper (zona habitable) */
#define UI_COLOR_ICE          lv_color_hex(0x64B5F6)  /* card frigo (congelador, frio) */

/* ── 2. TIPOGRAFIA: cinco papeles, una familia ───────────────────────────── */
#define UI_FONT_DISPLAY       (&lv_font_inter_46)            /* cifra protagonista */
#define UI_FONT_TITLE         (&lv_font_inter_28_es)         /* titulo de pantalla */
#define UI_FONT_VALUE         (&lv_font_inter_24_es)         /* dato de tarjeta */
#define UI_FONT_VALUE_SB      (&lv_font_inter_semibold_24)   /* dato que manda en su tarjeta */
#define UI_FONT_TEXT          (&lv_font_inter_20_es)         /* etiqueta, fila de ajustes */
#define UI_FONT_TEXT_SB       (&lv_font_inter_semibold_20)   /* etiqueta destacada */
#define UI_FONT_SMALL         (&lv_font_inter_14_es)         /* unidades, notas, pie */

/* ── 3. MEDIDAS (rejilla de 4 px) ────────────────────────────────────────── */
#define UI_PAD_4              4
#define UI_PAD_8              8
#define UI_PAD_12             12
#define UI_PAD_16             16
#define UI_PAD_20             20
#define UI_PAD_24             24
#define UI_PAD_PAGE           12      /* margen lateral de pagina (vistas y ajustes IGUAL) */
#define UI_PAD_CARD           20      /* relleno interior de tarjeta */
#define UI_GAP_CARD           16      /* separacion entre tarjetas */
#define UI_RADIUS_CARD        16
#define UI_RADIUS_CTRL        8       /* boton, chip, control */
#define UI_RADIUS_TAG         4       /* etiqueta pequena, LED */
#define UI_ROW_H              44      /* alto de fila de ajuste */
#define UI_HEADER_H           48      /* alto de cabecera de pantalla */
#define UI_BAR_H              48      /* alto de la barra inferior: las DOS pestanas
                                       * reservan lo mismo (antes 50 en vistas y 62
                                       * en ajustes, y por eso el contenido de las
                                       * dos pestanas no empezaba/terminaba igual) */

#ifdef __cplusplus
}
#endif

#endif /* UI_UI_STYLE_H */
