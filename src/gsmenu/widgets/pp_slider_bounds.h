#ifndef PP_SLIDER_BOUNDS_H
#define PP_SLIDER_BOUNDS_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Pure bound-computation helpers. Live value of the related key is read
 * via pp_settings_get and folded into the static bound.
 *
 *   bound_max = min(static_max, get(rel_*) + offset)
 *   bound_min = max(static_min, get(rel_*) + offset)
 *
 * When rel_key is NULL or the live value is unset, the static bound is
 * returned unchanged. Extracted from pp_slider for unit testing. */

int32_t pp_slider_bound_max(int32_t static_max,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset);

int32_t pp_slider_bound_min(int32_t static_min,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset);

#ifdef __cplusplus
}
#endif
#endif
