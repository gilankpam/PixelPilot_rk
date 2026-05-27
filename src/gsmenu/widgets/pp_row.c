#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *domain;
    const char *page;
    char       *key;
    lv_obj_t   *value_label;
} pp_row_data_t;

static void row_delete_cb(lv_event_t *e) {
    pp_row_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->key); free(d); }
}

static void row_focus_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    pp_row_data_t *d = lv_event_get_user_data(e);
    if (d && d->value_label) {
        lv_obj_add_style(d->value_label, &pp_style_value_focus,
                         LV_STATE_FOCUS_KEY);
    }
}

lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_flex_grow(label_obj, 1);

    pp_row_data_t *d = calloc(1, sizeof(*d));
    /* Inherited from parent page via user_data; pp_page sets it in a
     * future task. Until then, domain/page are NULL — get/set become no-ops. */
    d->domain = NULL;
    d->page = NULL;
    d->key = key ? strdup(key) : NULL;

    d->value_label = lv_label_create(row);
    lv_label_set_text(d->value_label, "—");

    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, d);

    /* Initial value read — safe even with NULL domain (returns NULL). */
    if (d->key) {
        char *v = pp_settings_get(d->domain, d->page, d->key);
        if (v && *v) lv_label_set_text(d->value_label, v);
        free(v);
    }

    return row;
}

void pp_row_set_value(lv_obj_t *row, const char *value) {
    pp_row_data_t *d = lv_obj_get_user_data(row);
    if (d && d->value_label) lv_label_set_text(d->value_label, value);
}
