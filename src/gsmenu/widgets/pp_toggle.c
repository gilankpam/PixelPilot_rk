#include "pp_toggle.h"
#include "pp_toast.h"
#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *sw;
    lv_obj_t *row;
    bool      in_flight;
} pp_toggle_data_t;

struct toggle_ctx {
    pp_toggle_data_t *d;
    bool target_on;       /* the value we *attempted* to write */
};

static void on_delete(lv_event_t *e) {
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void toggle_done_cb(int rc, const char *err, void *user_data) {
    struct toggle_ctx *ctx = (struct toggle_ctx *)user_data;
    pp_toggle_data_t *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        if (ctx->target_on) lv_obj_add_state(d->sw, LV_STATE_CHECKED);
        else                lv_obj_remove_state(d->sw, LV_STATE_CHECKED);
    } else {
        pp_toast_error(err ? err : "Failed to apply toggle");
        /* Switch position unchanged — we never flipped it. */
    }
    lv_free(ctx);
}

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    if (d->in_flight) { lv_event_stop_bubbling(e); return; }
    if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
        pp_toast_error("Locked by Dynamic Link");
        lv_event_stop_bubbling(e);
        return;
    }
    bool prev_on = lv_obj_has_state(d->sw, LV_STATE_CHECKED);
    bool target  = !prev_on;
    d->in_flight = true;
    pp_row_set_busy(d->row, true);
    struct toggle_ctx *ctx = lv_malloc(sizeof(*ctx));
    ctx->d = d;
    ctx->target_on = target;
    pp_settings_set_async(d->domain, d->page, d->key, target ? "on" : "off",
                          toggle_done_cb, ctx);
    lv_event_stop_bubbling(e);
}

lv_obj_t *pp_toggle(lv_obj_t *parent_page,
                    const char *icon_text,
                    const char *label,
                    const char *domain, const char *page, const char *key) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, PP_SCALE(36));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_style_min_width(label_obj, 200, 0);
    lv_obj_set_style_pad_right(label_obj, 16, 0);
    lv_obj_set_flex_grow(label_obj, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_add_style(sw, &pp_style_switch_on,
                     LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_size(sw, PP_SCALE(40), PP_SCALE(22));

    pp_toggle_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->sw     = sw;
    d->row    = row;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && strcmp(v, "on") == 0) lv_obj_add_state(sw, LV_STATE_CHECKED);
    free(v);

    if (!pp_settings_is_available(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_UNAVAILABLE);
    } else if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }

    return row;
}
