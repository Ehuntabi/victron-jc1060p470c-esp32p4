#include "chart_common.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void today_ymd(char out[11])
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(out, 11, "%04d-%02d-%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

void fmt_date_ddmmaaaa(const char *iso, char *out, size_t out_len)
{
    if (!iso || strlen(iso) != 10) { snprintf(out, out_len, "%s", iso ? iso : ""); return; }
    snprintf(out, out_len, "%.2s-%.2s-%.4s", iso + 8, iso + 5, iso);
}
