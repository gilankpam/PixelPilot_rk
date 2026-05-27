#ifndef PP_SLIDER_H
#define PP_SLIDER_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max);
#ifdef __cplusplus
}
#endif
#endif
