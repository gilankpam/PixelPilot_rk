#include "pp_page.h"
#include "../styles.h"
#include <stdlib.h>
#include <string.h>

extern lv_indev_t *indev_drv;

typedef struct {
    char *domain, *page;
    lv_group_t *group;
    lv_group_t *back_group;
} pp_page_data_t;

static void on_delete(lv_event_t *e) {
    pp_page_data_t *d = lv_event_get_user_data(e);
    if (!d) return;
    if (d->group) lv_group_del(d->group);
    free(d->domain); free(d->page); free(d);
}

/* When a child row receives HOME (A in NAV mode), hand focus back to
 * the registered back_group (the tabbar). Events from group-focused
 * children bubble up to the page object.
 *
 * lv_indev_set_group doesn't fire LV_EVENT_DEFOCUSED on the previously-
 * focused row, so its LV_STATE_FOCUS_KEY (the blue highlight) would
 * stick. Manually clear it before swapping. */
static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_HOME) return;
    pp_page_data_t *d = lv_event_get_user_data(e);
    if (!d || !d->back_group) return;

    lv_obj_t *focused = lv_group_get_focused(d->group);
    if (focused) lv_obj_remove_state(focused, LV_STATE_FOCUS_KEY);

    lv_indev_set_group(indev_drv, d->back_group);
}

lv_obj_t *pp_page_create(lv_obj_t *parent,
                         const char *domain, const char *page) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &pp_style_panel, 0);
    lv_obj_add_style(p, &pp_style_panel_alt, LV_PART_MAIN | LV_STATE_ALT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);

    pp_page_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->group  = lv_group_create();
    d->back_group = NULL;
    lv_obj_set_user_data(p, d);
    lv_obj_add_event_cb(p, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(p, on_key,    LV_EVENT_KEY,    d);
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
void pp_page_set_back_group(lv_obj_t *page, lv_group_t *back_group) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    if (!d) return;
    d->back_group = back_group;

    /* LV_EVENT_KEY is delivered to the focused object (a row), not to
     * the page. Enable event bubbling on every child so the page's
     * on_key handler can intercept LV_KEY_HOME. Call this AFTER all
     * children have been added — done from menu.c after page-build. */
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_add_flag(lv_obj_get_child(page, i), LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}
