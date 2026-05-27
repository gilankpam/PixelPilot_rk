#include "pp_slider_bounds.h"
#include "../settings.h"
#include <stdlib.h>

int32_t pp_slider_bound_max(int32_t static_max,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset) {
    if (!rel_key) return static_max;
    int32_t m = static_max;
    char *v = pp_settings_get(rel_domain, rel_page, rel_key);
    if (v && *v) {
        int32_t bound = atoi(v) + offset;
        if (bound < m) m = bound;
    }
    free(v);
    return m;
}

int32_t pp_slider_bound_min(int32_t static_min,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset) {
    if (!rel_key) return static_min;
    int32_t m = static_min;
    char *v = pp_settings_get(rel_domain, rel_page, rel_key);
    if (v && *v) {
        int32_t bound = atoi(v) + offset;
        if (bound > m) m = bound;
    }
    free(v);
    return m;
}
