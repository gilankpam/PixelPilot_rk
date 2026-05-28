#ifndef PP_DROPDOWN_H
#define PP_DROPDOWN_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
/* options: newline-separated list, e.g. "1080p60\n720p120\n540p60" */
lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                      const char *icon_text, const char *label,
                      const char *domain, const char *page, const char *key,
                      const char *options);
#ifdef __cplusplus
}
#endif
#endif
