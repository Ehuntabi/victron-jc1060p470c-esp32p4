#ifndef MAIN_FONTS_FONTS_ES_H
#define MAIN_FONTS_FONTS_ES_H

/* Fuentes Inter-Regular con rango ASCII + acentos españoles + ñ + ¿¡ + °.
 * Generadas con lv_font_conv (range 0x20-0x7F + glifos Latin-1 selectos).
 * Aliasamos los nombres antiguos `lv_font_montserrat_*_es` a las nuevas
 * `lv_font_inter_*_es` para minimizar cambios en el codigo. */

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t lv_font_inter_14_es;
extern const lv_font_t lv_font_inter_20_es;
extern const lv_font_t lv_font_inter_24_es;
extern const lv_font_t lv_font_inter_28_es;
extern const lv_font_t lv_font_inter_46;
extern const lv_font_t lv_font_inter_semibold_20;

/* Aliases para no tocar todos los `lv_font_montserrat_*_es` esparcidos */
#define lv_font_montserrat_14_es lv_font_inter_14_es
#define lv_font_montserrat_20_es lv_font_inter_20_es
#define lv_font_montserrat_24_es lv_font_inter_24_es
#define lv_font_montserrat_28_es lv_font_inter_28_es
#define lv_font_montserrat_46    lv_font_inter_46

#ifdef __cplusplus
}
#endif

#endif /* MAIN_FONTS_FONTS_ES_H */
