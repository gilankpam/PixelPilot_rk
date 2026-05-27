#include "pp_dropdown.h"
#include "pp_toast.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>

static void dropdown_done_cb(int rc, const char *err) {
    if (rc != 0) pp_toast_error(err ? err : "Failed to apply dropdown");
}

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *dd, *value_label, *row;
    lv_obj_t *popup;            /* floating options list while in EDIT */
    uint16_t saved_sel;
} pp_dd_data_t;

static void popup_close(pp_dd_data_t *d);

static void on_delete(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    if (d) {
        popup_close(d);
        free(d->domain); free(d->page); free(d->key); free(d);
    }
}

static void refresh_label(pp_dd_data_t *d) {
    char buf[64];
    lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
    lv_label_set_text(d->value_label, buf);
}

/* Build a small floating list of every option, with the currently-selected
 * one highlighted. Anchored to the row's value column on the right side. */
static void popup_open(pp_dd_data_t *d) {
    if (d->popup) return;

    lv_obj_t *top = lv_layer_top();
    lv_obj_t *p = lv_obj_create(top);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &pp_style_panel, 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_set_style_shadow_width(p, 16, 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    /* Width: comfortable fixed column. Height: clamp to visible page. */
    lv_obj_set_width(p, 180);
    lv_obj_set_style_max_height(p, lv_display_get_vertical_resolution(NULL) - 80, 0);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);

    /* Anchor: right-align next to the row. Compute absolute screen coords
     * of the row's value column, place popup just to the left of that. */
    lv_area_t coords;
    lv_obj_get_coords(d->value_label, &coords);
    int32_t x = coords.x2 + 8 - 180;          /* right-align popup to value end */
    if (x < 8) x = 8;
    int32_t y = coords.y1;
    int32_t bottom_limit = lv_display_get_vertical_resolution(NULL) - 16;
    int32_t pop_h_estimate = (lv_dropdown_get_option_count(d->dd) * 28) + 8;
    if (y + pop_h_estimate > bottom_limit) y = bottom_limit - pop_h_estimate;
    if (y < 8) y = 8;
    lv_obj_set_pos(p, x, y);

    uint16_t cur = lv_dropdown_get_selected(d->dd);
    uint16_t n   = lv_dropdown_get_option_count(d->dd);
    for (uint16_t i = 0; i < n; i++) {
        char buf[64];
        /* lv_dropdown_get_selected_str only returns the currently selected
         * string; iterate by temporarily setting selection on each. */
        lv_dropdown_set_selected(d->dd, i);
        lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);

        lv_obj_t *item = lv_obj_create(p);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 26);
        lv_obj_set_style_pad_hor(item, 10, 0);
        lv_obj_set_style_pad_ver(item, 4, 0);
        lv_obj_set_style_radius(item, 4, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        if (i == cur) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x4C60D8), 0);
            lv_obj_set_style_bg_opa(item, 79, 0);   /* same accent_bg as row focus */
        }

        lv_obj_t *l = lv_label_create(item);
        lv_label_set_text(l, buf);
        lv_obj_center(l);
        if (i == cur) {
            lv_obj_set_style_text_color(l, lv_color_hex(0x6B7FFF), 0);
        } else {
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_opa(l, 200, 0);
        }
    }
    /* Restore selection (above loop trashed it). */
    lv_dropdown_set_selected(d->dd, cur);

    /* Scroll the highlighted item into view if popup is taller than max. */
    if (cur < lv_obj_get_child_cnt(p)) {
        lv_obj_t *cur_item = lv_obj_get_child(p, cur);
        if (cur_item) lv_obj_scroll_to_view(cur_item, LV_ANIM_OFF);
    }

    d->popup = p;
}

static void popup_refresh(pp_dd_data_t *d) {
    if (!d->popup) return;
    uint16_t cur = lv_dropdown_get_selected(d->dd);
    uint32_t n = lv_obj_get_child_cnt(d->popup);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *item = lv_obj_get_child(d->popup, i);
        lv_obj_t *lbl  = lv_obj_get_child(item, 0);
        if (i == cur) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x4C60D8), 0);
            lv_obj_set_style_bg_opa(item, 79, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(0x6B7FFF), 0);
                lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
            }
        } else {
            lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_text_opa(lbl, 200, 0);
            }
        }
        if (i == cur) lv_obj_scroll_to_view(item, LV_ANIM_ON);
    }
}

static void popup_close(pp_dd_data_t *d) {
    if (!d->popup) return;
    lv_obj_del(d->popup);
    d->popup = NULL;
}

static void on_key(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    bool consumed = false;
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            d->saved_sel = lv_dropdown_get_selected(d->dd);
            control_mode = GSMENU_CONTROL_MODE_EDIT;
            popup_open(d);
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[64];
            lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
            pp_settings_set_async(d->domain, d->page, d->key, buf, dropdown_done_cb);
            popup_close(d);
        }
        consumed = true;
    } else if (k == LV_KEY_UP) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint16_t s = lv_dropdown_get_selected(d->dd);
            if (s > 0) lv_dropdown_set_selected(d->dd, s - 1);
            refresh_label(d);
            popup_refresh(d);
            consumed = true;
        }
    } else if (k == LV_KEY_DOWN) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint16_t s = lv_dropdown_get_selected(d->dd);
            if (s + 1 < lv_dropdown_get_option_count(d->dd))
                lv_dropdown_set_selected(d->dd, s + 1);
            refresh_label(d);
            popup_refresh(d);
            consumed = true;
        }
    } else if (k == LV_KEY_ESC) {
        lv_dropdown_set_selected(d->dd, d->saved_sel);
        refresh_label(d);
        popup_close(d);
        control_mode = GSMENU_CONTROL_MODE_NAV;
        consumed = true;
    }
    /* Same scroll-bubbling guard as pp_slider — only stop bubbling for
     * keys we consumed in EDIT mode. In NAV mode, UP/DOWN go to the
     * page group's focus traversal (which is what we want for row nav). */
    if (consumed) lv_event_stop_bubbling(e);
}

lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                     const char *icon_text, const char *label,
                     const char *domain, const char *page, const char *key,
                     const char *options) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
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

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "—");
    lv_obj_set_style_pad_right(value_label, 8, 0);

    /* Hidden dropdown — we drive it with keys, not its own popup.
     * Our own popup (popup_open) renders all options visibly. */
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);

    pp_dd_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->dd     = dd;
    d->value_label = value_label;
    d->row    = row;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        uint16_t n = lv_dropdown_get_option_count(dd);
        char buf[64];
        bool matched = false;
        for (uint16_t i = 0; i < n; i++) {
            lv_dropdown_set_selected(dd, i);
            lv_dropdown_get_selected_str(dd, buf, sizeof buf);
            if (strcmp(buf, v) == 0) { matched = true; break; }
        }
        if (matched) {
            refresh_label(d);
        } else {
            lv_dropdown_set_selected(dd, 0);
            /* keep placeholder em-dash in the label */
        }
    }
    free(v);

    return row;
}
