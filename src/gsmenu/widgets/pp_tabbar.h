#ifndef PP_TABBAR_H
#define PP_TABBAR_H
#include <lvgl.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_tabbar pp_tabbar_t;

typedef struct {
    const char *label;      /* "Camera" */
    const char *icon_text;  /* LV_SYMBOL_* */
    lv_obj_t   *page;       /* page to show when this tab is active */
} pp_tabbar_item_t;

pp_tabbar_t *pp_tabbar_create(lv_obj_t *parent,
                              const pp_tabbar_item_t *items, size_t n);

void         pp_tabbar_set_active(pp_tabbar_t *t, size_t index);
lv_group_t  *pp_tabbar_group(pp_tabbar_t *t);
lv_obj_t    *pp_tabbar_root(pp_tabbar_t *t);

#ifdef __cplusplus
}
#endif
#endif
