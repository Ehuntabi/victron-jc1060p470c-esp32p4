/* ui/chart_common.h — utilidades de fecha compartidas entre las pantallas de
 * historico (frigo_history_screen.c y battery_history_screen.c). */
#ifndef UI_CHART_COMMON_H
#define UI_CHART_COMMON_H

#include <stddef.h>

/* "YYYY-MM-DD" de hoy. Usado para no listar el CSV de hoy como fecha
 * navegable ademas de "HOY" (idx -1): mismo dia, dos pestanas identicas. */
void today_ymd(char out[11]);

/* "YYYY-MM-DD" -> "DD-MM-YYYY", solo para mostrar. El string de origen (nombre
 * de fichero, orden alfabetico ascendente) no se toca. */
void fmt_date_ddmmaaaa(const char *iso, char *out, size_t out_len);

#endif /* UI_CHART_COMMON_H */
