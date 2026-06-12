#include "pp_tabbar.h"
#include "pp_page.h"
#include "../styles.h"
#include <stdlib.h>
#include <string.h>

extern lv_indev_t *indev_drv;

#define PP_TAB_W 150
#define PP_TAB_H 84

struct pp_tabbar {
    lv_obj_t  *root;
    lv_group_t *group;
    pp_tabbar_item_t *items;
    size_t n;
    size_t active;
    lv_obj_t **tab_objs;
};

static void apply_active(pp_tabbar_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        lv_obj_t *page = t->items[i].page;
        if (i == t->active) {
            lv_obj_add_state(t->tab_objs[i], LV_STATE_CHECKED);
            /* Show instantly at full opacity. A fade (opa < COVER) buffers the
             * whole page into a layer and recomposites it every frame — very
             * expensive on the single-threaded SW renderer; not worth 120ms. */
            lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
            lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(t->tab_objs[i], LV_STATE_CHECKED);
            lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_focus(lv_event_t *e) {
    pp_tabbar_t *t = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    for (size_t i = 0; i < t->n; i++) {
        if (t->tab_objs[i] == target) {
            t->active = i;
            apply_active(t);
            lv_obj_send_event(t->root, LV_EVENT_VALUE_CHANGED, &t->active);
            return;
        }
    }
}

/* Pressing ENTER on a tab hands focus to that tab's page's group so W/S
 * starts moving through rows. Pressing A (HOME) in the page returns
 * focus to the tabbar — that path is handled in pp_page. */
static void on_tab_key(lv_event_t *e) {
    pp_tabbar_t *t = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);

    /* HOME at the root level: emit a cancel event on the tabbar root.
     * The owner (menu.c) handles it by closing the menu screen. */
    if (k == LV_KEY_HOME) {
        lv_obj_send_event(t->root, LV_EVENT_CANCEL, NULL);
        return;
    }
    if (k != LV_KEY_ENTER) return;

    lv_group_t *page_group = pp_page_group(t->items[t->active].page);
    if (!page_group || lv_group_get_obj_count(page_group) == 0) return;

    /* The keypad indev delivers keys only to the group's focused object,
     * and only while that object is enabled — a disabled focused row
     * swallows everything except NEXT/PREV, including the HOME needed to
     * leave the page. Lock state may have disabled the group's remembered
     * focus (or every row, e.g. the all-air Camera page with the drone
     * off) since the last visit, so never enter without an enabled
     * focused row. lv_group_focus_next skips disabled/hidden objects. */
    lv_obj_t *focused = lv_group_get_focused(page_group);
    if (!focused || lv_obj_has_state(focused, LV_STATE_DISABLED)) {
        lv_group_focus_next(page_group);
        focused = lv_group_get_focused(page_group);
        if (!focused || lv_obj_has_state(focused, LV_STATE_DISABLED)) return;
    }
    lv_indev_set_group(indev_drv, page_group);

    /* Restore the focus highlight. lv_indev_set_group doesn't fire
     * LV_EVENT_FOCUSED, and pp_page's HOME handler strips
     * LV_STATE_FOCUS_KEY on exit, so re-apply it here. */
    lv_obj_add_state(focused, LV_STATE_FOCUS_KEY);
}

pp_tabbar_t *pp_tabbar_create(lv_obj_t *parent,
                              const pp_tabbar_item_t *items, size_t n) {
    pp_tabbar_t *t = calloc(1, sizeof(*t));
    t->n = n;
    t->items = malloc(n * sizeof(*items));
    memcpy(t->items, items, n * sizeof(*items));
    t->tab_objs = calloc(n, sizeof(*t->tab_objs));

    t->root = lv_obj_create(parent);
    lv_obj_remove_style_all(t->root);
    lv_obj_add_style(t->root, &pp_style_tabbar, 0);
    lv_obj_set_size(t->root, PP_TAB_W, LV_PCT(100));
    lv_obj_set_flex_flow(t->root, LV_FLEX_FLOW_COLUMN);

    t->group = lv_group_create();

    for (size_t i = 0; i < n; i++) {
        lv_obj_t *tab = lv_obj_create(t->root);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &pp_style_tab, 0);
        lv_obj_add_style(tab, &pp_style_tab_active, LV_STATE_CHECKED);
        lv_obj_set_size(tab, LV_PCT(100), PP_TAB_H);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *icon = lv_label_create(tab);
        lv_label_set_text(icon, items[i].icon_text ? items[i].icon_text : "");

        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, items[i].label);

        lv_obj_add_event_cb(tab, on_focus,  LV_EVENT_FOCUSED, t);
        lv_obj_add_event_cb(tab, on_tab_key, LV_EVENT_KEY,    t);
        lv_group_add_obj(t->group, tab);
        t->tab_objs[i] = tab;
    }

    pp_tabbar_set_active(t, 0);
    return t;
}

void pp_tabbar_set_active(pp_tabbar_t *t, size_t i) {
    if (i >= t->n) return;
    t->active = i;
    apply_active(t);
}

lv_group_t *pp_tabbar_group(pp_tabbar_t *t) { return t->group; }
lv_obj_t   *pp_tabbar_root(pp_tabbar_t *t)  { return t->root; }
