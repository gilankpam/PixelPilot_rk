#include "pp_slider.h"
#include "pp_slider_bounds.h"
#include "pp_slider_accel.h"
#include "pp_toast.h"
#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Spinbox layout in the value column:
 *     ▲      <- up chevron
 *    50      <- big number (accent when focused)
 *     ▼      <- down chevron
 *
 * Chevrons start dim and brighten when the row enters EDIT mode so the
 * user has a clear cue that W/S now adjusts the number. */

struct pp_slider_data {
    char *domain, *page, *key;
    int32_t min, max;
    int32_t value;
    int32_t saved_val;
    lv_obj_t *num, *up_chev, *down_chev;
    lv_obj_t *row;
    bool      in_flight;

    /* Optional relation: effective bound is rel_val + rel_offset, where
     * rel_val is read live from pp_settings_get(rel_domain, rel_page,
     * rel_key). NULL rel_key means no relation. */
    char *rel_domain, *rel_page, *rel_key;
    int32_t rel_offset;
    bool    rel_is_max;

    /* Hold-to-accelerate state: tracks consecutive same-key events
     * within PP_SLIDER_HOLD_GAP_MS to scale the step size. */
    uint32_t last_key_ms;
    int32_t  last_key;
    int32_t  hold_count;
};
typedef struct pp_slider_data pp_slider_data_t;

static int32_t effective_max(pp_slider_data_t *d) {
    if (d->rel_key && d->rel_is_max) {
        return pp_slider_bound_max(d->max, d->rel_domain, d->rel_page,
                                   d->rel_key, d->rel_offset);
    }
    return d->max;
}

static int32_t effective_min(pp_slider_data_t *d) {
    if (d->rel_key && !d->rel_is_max) {
        return pp_slider_bound_min(d->min, d->rel_domain, d->rel_page,
                                   d->rel_key, d->rel_offset);
    }
    return d->min;
}

/* Forward declaration — defined after struct pp_slider_data below. */
static void refresh_num(pp_slider_data_t *d);

struct slider_ctx {
    struct pp_slider_data *d;
    int32_t target_val;
};

static void slider_done_cb(int rc, const char *err, void *user_data) {
    struct slider_ctx *ctx = (struct slider_ctx *)user_data;
    struct pp_slider_data *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        d->value = ctx->target_val;
        refresh_num(d);
    } else {
        pp_toast_error(err ? err : "Failed to apply slider");
        /* d->value is already at saved_val; nothing to revert. */
    }
    lv_free(ctx);
}

static void on_delete(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    if (d) {
        free(d->domain); free(d->page); free(d->key);
        free(d->rel_domain); free(d->rel_page); free(d->rel_key);
        free(d);
    }
}

static void refresh_num(pp_slider_data_t *d) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int)d->value);
    lv_label_set_text(d->num, buf);
}

static int32_t step_for(int32_t min, int32_t max) {
    int32_t range = max - min;
    if (range < 0) range = -range;
    int32_t s = range / 20;
    if (s < 1) s = 1;
    return s;
}

/* Visually mark the spinbox as "currently editing" — chevrons brighten
 * to accent, and the number turns accent too. NAV-mode focus on the row
 * intentionally does not highlight the number; the row's own focus
 * background is enough to show which row is selected. */
static void set_edit_state(pp_slider_data_t *d, bool active) {
    lv_color_t c = active ? lv_color_hex(0x6B7FFF) : lv_color_hex(0xFFFFFF);
    lv_opa_t   o = active ? LV_OPA_COVER : 90;
    lv_obj_set_style_text_color(d->up_chev,   c, 0);
    lv_obj_set_style_text_color(d->down_chev, c, 0);
    lv_obj_set_style_text_opa(d->up_chev,   o, 0);
    lv_obj_set_style_text_opa(d->down_chev, o, 0);
    lv_obj_set_style_text_color(d->num,
        active ? lv_color_hex(0x6B7FFF) : lv_color_hex(0xFFFFFF), 0);
}

static void on_key(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    bool consumed = false;
    int32_t step = step_for(d->min, d->max);

    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
                pp_toast_error("Locked by Dynamic Link");
                consumed = true;
            } else {
                d->saved_val = d->value;
                d->hold_count = 0;
                d->last_key = 0;
                d->last_key_ms = 0;
                control_mode = GSMENU_CONTROL_MODE_EDIT;
                set_edit_state(d, true);
                consumed = true;
            }
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            set_edit_state(d, false);
            if (d->value == d->saved_val) {
                consumed = true;          /* no change — skip the round-trip */
            } else {
                char buf[32];
                snprintf(buf, sizeof buf, "%d", (int)d->value);
                /* Revert the visible value to saved_val until apply confirms;
                 * we keep the *attempted* value in ctx->target_val. */
                int32_t attempted = d->value;
                d->value = d->saved_val;
                refresh_num(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct slider_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d = d;
                ctx->target_val = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf,
                                      slider_done_cb, ctx);
                consumed = true;
            }
        }
    } else if (k == LV_KEY_UP) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint32_t now = lv_tick_get();
            d->hold_count = pp_slider_accel_update(now, d->last_key_ms,
                                                   d->last_key, k, d->hold_count);
            d->last_key = k;
            d->last_key_ms = now;
            int32_t scaled = pp_slider_accel_step(step, d->hold_count);
            int32_t emax = effective_max(d);
            d->value += scaled;
            if (d->value > emax) d->value = emax;
            refresh_num(d);
            consumed = true;
        }
    } else if (k == LV_KEY_DOWN) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint32_t now = lv_tick_get();
            d->hold_count = pp_slider_accel_update(now, d->last_key_ms,
                                                   d->last_key, k, d->hold_count);
            d->last_key = k;
            d->last_key_ms = now;
            int32_t scaled = pp_slider_accel_step(step, d->hold_count);
            int32_t emin = effective_min(d);
            d->value -= scaled;
            if (d->value < emin) d->value = emin;
            refresh_num(d);
            consumed = true;
        }
    } else if (k == LV_KEY_ESC) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            d->value = d->saved_val;
            refresh_num(d);
            set_edit_state(d, false);
        }
        control_mode = GSMENU_CONTROL_MODE_NAV;
        consumed = true;
    }
    if (consumed) lv_event_stop_bubbling(e);
}

lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max) {
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

    (void)icon_text;   /* OSD reskin: rows are label + value only (no leading icon) */

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_style_min_width(label_obj, 200, 0);
    lv_obj_set_style_pad_right(label_obj, 16, 0);
    lv_obj_set_flex_grow(label_obj, 1);

    /* Horizontal spinbox on the right side: ▲ 50 ▼ — keeps the row at the
     * same 36px height as toggle/dropdown. UP/DOWN chevrons still convey
     * the UP/DOWN key affordance even when laid out horizontally. */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(col, 6, 0);

    lv_obj_t *up = lv_label_create(col);
    lv_label_set_text(up, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(up, &lv_font_montserrat_14, 0);

    lv_obj_t *num = lv_label_create(col);
    lv_label_set_text(num, "—");

    lv_obj_t *dn = lv_label_create(col);
    lv_label_set_text(dn, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(dn, &lv_font_montserrat_14, 0);

    pp_slider_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->min    = min;
    d->max    = max;
    d->value  = min;
    d->num    = num;
    d->up_chev   = up;
    d->down_chev = dn;

    lv_obj_set_user_data(row, d);
    d->row = row;
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    set_edit_state(d, false);

    /* Read initial value via settings provider. */
    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        d->value = atoi(v);
        if (d->value < min) d->value = min;
        if (d->value > max) d->value = max;
        refresh_num(d);
    }
    free(v);

    if (!pp_settings_is_available(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_UNAVAILABLE);
    } else if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }

    return row;
}

void pp_slider_set_relation(lv_obj_t *row,
                            const char *rel_domain,
                            const char *rel_page,
                            const char *rel_key,
                            int32_t offset,
                            bool is_max) {
    pp_slider_data_t *d = (pp_slider_data_t *)lv_obj_get_user_data(row);
    if (!d) return;
    free(d->rel_domain); free(d->rel_page); free(d->rel_key);
    d->rel_domain = rel_domain ? strdup(rel_domain) : NULL;
    d->rel_page   = rel_page   ? strdup(rel_page)   : NULL;
    d->rel_key    = rel_key    ? strdup(rel_key)    : NULL;
    d->rel_offset = offset;
    d->rel_is_max = is_max;
}
