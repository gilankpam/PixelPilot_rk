#include "pp_page.h"
#include "../styles.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page;
    lv_group_t *group;
} pp_page_data_t;

static void on_delete(lv_event_t *e) {
    pp_page_data_t *d = lv_event_get_user_data(e);
    if (!d) return;
    if (d->group) lv_group_del(d->group);
    free(d->domain); free(d->page); free(d);
}

lv_obj_t *pp_page_create(lv_obj_t *parent,
                         const char *domain, const char *page) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &pp_style_panel, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);

    pp_page_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->group  = lv_group_create();
    lv_obj_set_user_data(p, d);
    lv_obj_add_event_cb(p, on_delete, LV_EVENT_DELETE, d);
    return p;
}

lv_group_t *pp_page_group(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->group : NULL;
}
const char *pp_page_domain(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->domain : NULL;
}
const char *pp_page_name(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->page : NULL;
}
