#include "pp_slider.h"
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

typedef struct {
    char *domain, *page, *key;
    int32_t min, max;
    int32_t value;
    int32_t saved_val;
    lv_obj_t *num, *up_chev, *down_chev;
} pp_slider_data_t;

static void on_delete(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
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
            d->saved_val = d->value;
            control_mode = GSMENU_CONTROL_MODE_EDIT;
            set_edit_state(d, true);
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            set_edit_state(d, false);
            char buf[32];
            snprintf(buf, sizeof buf, "%d", (int)d->value);
            pp_settings_set_async(d->domain, d->page, d->key, buf, NULL);
        }
        consumed = true;
    } else if (k == LV_KEY_UP) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            d->value += step;
            if (d->value > d->max) d->value = d->max;
            refresh_num(d);
            consumed = true;
        }
    } else if (k == LV_KEY_DOWN) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            d->value -= step;
            if (d->value < d->min) d->value = d->min;
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
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 36, 0);
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
    lv_obj_set_flex_grow(label_obj, 1);

    /* Spinbox column on the right side. */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

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

    return row;
}
