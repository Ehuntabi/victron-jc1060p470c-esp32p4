#ifndef UI_UI_FORMAT_H
#define UI_UI_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <lvgl.h>

int ui_round_div_signed(int value, unsigned divisor);
void ui_label_set_unsigned_fixed(lv_obj_t *label, unsigned value,
                                 unsigned scale, uint8_t frac_digits,
                                 const char *unit);
void ui_label_set_signed_fixed(lv_obj_t *label, int value,
                               unsigned scale, uint8_t frac_digits,
                               const char *unit);
void ui_format_aux_value(uint8_t aux_input, uint16_t aux_value,
                         char *out, size_t out_len);

#endif /* UI_UI_FORMAT_H */
