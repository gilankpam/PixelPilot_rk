#ifndef PP_DRILLDOWN_H
#define PP_DRILLDOWN_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pp_drilldown_build_fn)(lv_obj_t *body, void *user);

lv_obj_t *pp_drilldown_open(lv_obj_t *anchor_page, const char *title,
                            pp_drilldown_build_fn build, void *user);
void      pp_drilldown_close(void);

#ifdef __cplusplus
}
#endif
#endif
