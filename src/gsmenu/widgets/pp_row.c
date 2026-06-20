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
    lv_obj_set_height(row, PP_SCALE(36));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    (void)icon_text;   /* OSD reskin: rows are label + value only (no leading icon) */

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
    lv_obj_set_style_text_font(d->value_label, pp_font_xb_md(), 0);
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

/* Per-row UX-state storage attached as a side struct via lv_obj_set_user_data
 * on a hidden child. We don't want to disturb existing user_data on the row
 * which other widgets already use. Instead we look up a dedicated label
 * child by name. */

#define PP_ROW_BUSY_ICON LV_SYMBOL_LOOP
#define PP_ROW_LOCK_ICON_DYNAMIC LV_SYMBOL_SETTINGS
#define PP_ROW_LOCK_ICON_OFFLINE LV_SYMBOL_WARNING
#define PP_ROW_LOCK_ICON_UNAVAILABLE LV_SYMBOL_MINUS

typedef struct {
    lv_obj_t      *spinner;       /* small label, hidden when not busy */
    lv_obj_t      *lock_label;    /* small label, hidden when not locked */
    pp_row_lock_t  lock_state;
    bool           busy;
} pp_row_state_t;

static void row_state_delete_cb(lv_event_t *e) {
    pp_row_state_t *s = (pp_row_state_t *)lv_event_get_user_data(e);
    if (s) lv_free(s);
}

static pp_row_state_t *row_state(lv_obj_t *row) {
    /* Stored in a child object marked with LV_OBJ_FLAG_USER_2; if no
     * state child exists we create one. */
    uint32_t n = lv_obj_get_child_cnt(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            return (pp_row_state_t *)lv_obj_get_user_data(c);
        }
    }
    /* Create the state holder: an empty 0-size object marked with USER_2. */
    lv_obj_t *holder = lv_obj_create(row);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, 0, 0);
    lv_obj_add_flag(holder, LV_OBJ_FLAG_USER_2);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);
    pp_row_state_t *s = lv_malloc(sizeof(*s));
    s->spinner = NULL;
    s->lock_label = NULL;
    s->lock_state = PP_ROW_UNLOCKED;
    s->busy = false;
    lv_obj_set_user_data(holder, s);
    lv_obj_add_event_cb(holder, row_state_delete_cb, LV_EVENT_DELETE, s);
    return s;
}

static lv_obj_t *ensure_spinner(lv_obj_t *row, pp_row_state_t *s) {
    if (s->spinner) return s->spinner;
    s->spinner = lv_label_create(row);
    lv_label_set_text(s->spinner, PP_ROW_BUSY_ICON);
    lv_obj_set_style_text_color(s->spinner, lv_color_hex(PP_C_ACCENT), 0);
    lv_obj_set_style_pad_left(s->spinner, PP_SCALE(6), 0);
    lv_obj_add_flag(s->spinner, LV_OBJ_FLAG_HIDDEN);
    return s->spinner;
}

static lv_obj_t *ensure_lock_label(lv_obj_t *row, pp_row_state_t *s) {
    if (s->lock_label) return s->lock_label;
    s->lock_label = lv_label_create(row);
    lv_label_set_text(s->lock_label, PP_ROW_LOCK_ICON_DYNAMIC);
    lv_obj_set_style_text_color(s->lock_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_pad_left(s->lock_label, PP_SCALE(6), 0);
    lv_obj_add_flag(s->lock_label, LV_OBJ_FLAG_HIDDEN);
    return s->lock_label;
}

void pp_row_set_busy(lv_obj_t *row, bool busy) {
    pp_row_state_t *s = row_state(row);
    if (s->busy == busy) return;
    s->busy = busy;
    lv_obj_t *spin = ensure_spinner(row, s);
    if (busy) {
        lv_obj_clear_flag(spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(row, LV_STATE_DISABLED);
    } else {
        lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(row, LV_STATE_DISABLED);
    }
}

void pp_row_set_locked(lv_obj_t *row, pp_row_lock_t state) {
    pp_row_state_t *s = row_state(row);
    if (s->lock_state == state) return;
    s->lock_state = state;
    lv_obj_t *lbl = ensure_lock_label(row, s);
    if (state == PP_ROW_UNLOCKED) {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(row, LV_STATE_DISABLED);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
    } else {
        lv_label_set_text(lbl,
            state == PP_ROW_LOCKED_OFFLINE     ? PP_ROW_LOCK_ICON_OFFLINE :
            state == PP_ROW_LOCKED_UNAVAILABLE ? PP_ROW_LOCK_ICON_UNAVAILABLE :
                                                 PP_ROW_LOCK_ICON_DYNAMIC);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(row, LV_STATE_DISABLED);
        lv_obj_set_style_opa(row, LV_OPA_60, 0);
    }
}

pp_row_lock_t pp_row_get_locked(lv_obj_t *row) {
    return row_state(row)->lock_state;
}
