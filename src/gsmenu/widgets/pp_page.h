#ifndef PP_PAGE_H
#define PP_PAGE_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A scrollable container that owns a focus group of its child rows.
 * (domain, page) is stored on the object and inherited via getters used
 * by future widgets that don't take the triple explicitly. */
lv_obj_t   *pp_page_create(lv_obj_t *parent,
                           const char *domain, const char *page);
lv_group_t *pp_page_group(lv_obj_t *page);
const char *pp_page_domain(lv_obj_t *page);
const char *pp_page_name(lv_obj_t *page);

/* When the user presses LV_KEY_HOME while focus is in this page, the
 * indev focus group is switched to `back_group`. Typically the tabbar's
 * group, wired by menu.c after all pages are built. */
void pp_page_set_back_group(lv_obj_t *page, lv_group_t *back_group);

/* Called after lock state changes. If the indev currently drives this
 * page's group and its focused row was disabled, refocus an enabled row,
 * or — when none is left (all rows locked) — return focus to back_group
 * so the user is never stuck on a page that swallows every key. */
void pp_page_rescue_focus(lv_obj_t *page);

#ifdef __cplusplus
}
#endif
#endif
