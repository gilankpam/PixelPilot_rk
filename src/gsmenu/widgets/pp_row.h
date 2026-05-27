#ifndef PP_ROW_H
#define PP_ROW_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Creates a focusable horizontal row.
 *   icon_text: NULL or an LV_SYMBOL_* constant
 *   label:     left-aligned text
 *   key:       settings key (used to read the value via pp_settings_get).
 *              May be NULL for static rows (no value reload).
 *
 * The row stores its (domain, page, key) triple in user_data so other
 * widgets (toggle/slider/dropdown) can build on top. */
lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key);

/* Update a row's value label to a new string. */
void pp_row_set_value(lv_obj_t *row, const char *value);

#ifdef __cplusplus
}
#endif
#endif
