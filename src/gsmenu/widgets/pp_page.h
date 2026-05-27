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

#ifdef __cplusplus
}
#endif
#endif
