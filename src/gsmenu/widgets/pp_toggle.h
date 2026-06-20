#ifndef PP_TOGGLE_H
#define PP_TOGGLE_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Like pp_row_text, but the value is an lv_switch. Pressing ENTER on the
 * focused row toggles the switch and calls pp_settings_set_async. */
lv_obj_t *pp_toggle(lv_obj_t *parent_page,
                    const char *icon_text,
                    const char *label,
                    const char *domain, const char *page, const char *key);

#ifdef __cplusplus
}
#endif
#endif
