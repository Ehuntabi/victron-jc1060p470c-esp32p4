#include "ui_format.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static size_t append_text(char *dst, size_t max, const char *text)
{
    if (max == 0) {
        return 0;
    }

    size_t i = 0;
    while (i < max - 1 && text[i] != '\0') {
        dst[i] = text[i];
        ++i;
    }
    dst[i] = '\0';
    return i;
}

static size_t write_uint(char *dst, size_t max, unsigned value)
{
    if (max == 0) {
        return 0;
    }

    if (value == 0) {
        dst[0] = '0';
        if (max > 1) {
            dst[1] = '\0';
        }
        return 1;
    }

    char tmp[16];
    size_t pos = 0;
    while (value > 0 && pos < sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    size_t i = 0;
    while (pos > 0 && i < max - 1) {
        dst[i++] = tmp[--pos];
    }
    dst[i] = '\0';
    return i;
}

static size_t build_unsigned_fixed(char *buf, size_t max,
                                   unsigned value, unsigned scale,
                                   uint8_t frac_digits)
{
    if (max == 0) {
        return 0;
    }

    size_t idx = write_uint(buf, max, value / scale);
    if (idx >= max - 1 || frac_digits == 0) {
        return idx;
    }

    buf[idx++] = '.';
    buf[idx] = '\0';

    unsigned frac = value % scale;
    unsigned divisor = scale;
    for (uint8_t i = 0; i < frac_digits && idx < max - 1; ++i) {
        divisor /= 10U;
        unsigned digit = divisor ? frac / divisor : 0U;
        buf[idx++] = (char)('0' + digit);
        buf[idx] = '\0';
        if (divisor) {
            frac %= divisor;
        }
    }

    return idx;
}

static size_t build_signed_fixed(char *buf, size_t max, int value,
                                 unsigned scale, uint8_t frac_digits)
{
    if (max == 0) {
        return 0;
    }

    size_t idx = 0;
    bool negative = value < 0;
    unsigned abs_val = negative ? (unsigned)(-value) : (unsigned)value;
    if (negative && abs_val != 0 && idx < max - 1) {
        buf[idx++] = '-';
    }

    size_t written = build_unsigned_fixed(buf + idx, max - idx,
                                          abs_val, scale, frac_digits);
    return idx + written;
}

void ui_label_set_unsigned_fixed(lv_obj_t *label, unsigned value,
                                 unsigned scale, uint8_t frac_digits,
                                 const char *unit)
{
    char number[24] = {0};
    build_unsigned_fixed(number, sizeof(number), value, scale, frac_digits);

    char text[32] = {0};
    size_t idx = append_text(text, sizeof(text), number);
    if (unit != NULL && idx < sizeof(text) - 1) {
        append_text(text + idx, sizeof(text) - idx, unit);
    }

    lv_label_set_text(label, text);
}

void ui_label_set_signed_fixed(lv_obj_t *label, int value,
                               unsigned scale, uint8_t frac_digits,
                               const char *unit)
{
    char number[24] = {0};
    build_signed_fixed(number, sizeof(number), value, scale, frac_digits);

    char text[32] = {0};
    size_t idx = append_text(text, sizeof(text), number);
    if (unit != NULL && idx < sizeof(text) - 1) {
        append_text(text + idx, sizeof(text) - idx, unit);
    }

    lv_label_set_text(label, text);
}

int ui_round_div_signed(int value, unsigned divisor)
{
    if (divisor == 0) {
        return value;
    }

    if (value >= 0) {
        return (value + (int)(divisor / 2)) / (int)divisor;
    }
    return -(((-value) + (int)(divisor / 2)) / (int)divisor);
}

void ui_format_aux_value(uint8_t aux_input, uint16_t aux_value,
                         char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }

    const uint16_t AUX_NA = 0xFFFF;
    size_t idx = 0;

    switch (aux_input & 0x03u) {
    case 0:
        idx += append_text(out + idx, out_len - idx, "A: ");
        if (aux_value == AUX_NA) {
            idx += append_text(out + idx, out_len - idx, "N/A");
        } else {
            char number[16] = {0};
            build_unsigned_fixed(number, sizeof(number), aux_value, 100, 2);
            idx += append_text(out + idx, out_len - idx, number);
            idx += append_text(out + idx, out_len - idx, " V");
        }
        break;
    case 1:
        idx += append_text(out + idx, out_len - idx, "M: ");
        if (aux_value == AUX_NA) {
            idx += append_text(out + idx, out_len - idx, "N/A");
        } else {
            char number[16] = {0};
            build_unsigned_fixed(number, sizeof(number), aux_value, 100, 2);
            idx += append_text(out + idx, out_len - idx, number);
            idx += append_text(out + idx, out_len - idx, " V");
        }
        break;
    case 2:
        idx += append_text(out + idx, out_len - idx, "T:");
        if (aux_value == AUX_NA) {
            idx += append_text(out + idx, out_len - idx, "N/A");
        } else {
            int temp_centi = (int)aux_value - 27315;
            int temp_tenths = ui_round_div_signed(temp_centi, 10);
            char number[16] = {0};
            build_signed_fixed(number, sizeof(number), temp_tenths, 10, 1);
            idx += append_text(out + idx, out_len - idx, number);
            idx += append_text(out + idx, out_len - idx, "C");
        }
        break;
    default:
        idx = append_text(out, out_len, "-");
        break;
    }

    if (idx < out_len) {
        out[idx] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
}
