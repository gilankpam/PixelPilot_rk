#ifndef PP_SLIDER_H
#define PP_SLIDER_H
#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "pp_slider_scale.h"
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max);

/* Config-driven slider (fractional / scaled / variable-step). `pp_slider`
 * above is a thin wrapper over this with a plain-integer default config. */
lv_obj_t *pp_slider_ex(lv_obj_t *parent_page,
                       const char *icon_text, const char *label,
                       const char *domain, const char *page, const char *key,
                       const pp_slider_cfg_t *cfg);

/* Optional: tie this slider's effective max (or min) to the live value
 * of another settings key. The effective bound becomes
 *   pp_settings_get(rel_*) + offset
 * and is enforced during EDIT mode (UP/DOWN clamping). If is_max is true
 * the relation defines the upper bound; otherwise the lower bound.
 * Used by the FEC K/N pair to enforce k <= n - 2. */
void pp_slider_set_relation(lv_obj_t *row,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset,
                            bool is_max);

#ifdef __cplusplus
}
#endif
#endif
