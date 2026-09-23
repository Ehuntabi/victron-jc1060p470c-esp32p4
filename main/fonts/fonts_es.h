#ifndef MAIN_FONTS_FONTS_ES_H
#define MAIN_FONTS_FONTS_ES_H

/* Fuentes Inter-Regular con rango ASCII + acentos españoles + ñ + ¿¡ + °.
 * Generadas con lv_font_conv (range 0x20-0x7F + glifos Latin-1 selectos).
 * Aliasamos los nombres antiguos `lv_font_montserrat_*_es` a las nuevas
 * (los ficheros lv_font_montserrat_*_es.c se BORRARON el 23-sep-2026: nadie
 *  referenciaba sus simbolos, solo los nombres, que van por estos alias)
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
extern const lv_font_t lv_font_inter_semibold_24;

/* Aliases para no tocar todos los `lv_font_montserrat_*_es` esparcidos */
#define lv_font_montserrat_14_es lv_font_inter_14_es
#define lv_font_montserrat_20_es lv_font_inter_20_es
#define lv_font_montserrat_24_es lv_font_inter_24_es
#define lv_font_montserrat_28_es lv_font_inter_28_es
#define lv_font_montserrat_46    lv_font_inter_46

/* v3.8: y tambien los nombres SIN sufijo. Hasta hoy, quien escribia
 * `lv_font_montserrat_24` (sin _es) obtenia la Montserrat DE LVGL, que solo trae
 * ASCII: el texto salia sin acentos y en OTRA tipografia que el resto de la
 * pantalla. Paso en la pagina de GPS (por eso estaba toda escrita sin acentos:
 * "POSICION", "Sin senal del modulo"), en el panel del frigo, en la galeria y en
 * el modo ausente. Los ficheros lv_font_inter_* ya llevan la Montserrat como
 * fallback, asi que los simbolos LV_SYMBOL_* se siguen viendo.
 * El 32 no tiene Inter equivalente: se queda en el 28 del resto de valores
 * grandes de la misma tarjeta (el SOC de la pila y su tension). */
#define lv_font_montserrat_14 lv_font_inter_14_es
#define lv_font_montserrat_20 lv_font_inter_20_es
#define lv_font_montserrat_24 lv_font_inter_24_es
#define lv_font_montserrat_28 lv_font_inter_28_es
#define lv_font_montserrat_32 lv_font_inter_28_es

/* v3.9: la SemiBold de Inter. Estaba compilada desde el principio y no la usaba
 * nadie; entra en los TITULOS (entradas del menu de Ajustes y migas de pan): el
 * mismo tamano con mas presencia, sin subir la letra ni cambiar de familia. */
#define lv_font_semibold_20 lv_font_inter_semibold_20
#define lv_font_semibold_24 lv_font_inter_semibold_24
/* OJO con la SemiBold: se genero solo con los rangos de texto y SIN fallback, asi
 * que NO dibuja los LV_SYMBOL_* (salen como una caja). Para un simbolo, usar la
 * fuente normal, que si lleva Montserrat de fallback. */

#ifdef __cplusplus
}
#endif

#endif /* MAIN_FONTS_FONTS_ES_H */
