#include "pp_dropdown.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *dd, *value_label;
    uint16_t saved_sel;
} pp_dd_data_t;

static void on_delete(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void refresh_label(pp_dd_data_t *d) {
    char buf[64];
    lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
    lv_label_set_text(d->value_label, buf);
}

static void on_key(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            d->saved_sel = lv_dropdown_get_selected(d->dd);
            control_mode = GSMENU_CONTROL_MODE_EDIT;
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[64];
            lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
            pp_settings_set_async(d->domain, d->page, d->key, buf, NULL);
        }
    } else if (k == LV_KEY_UP) {
        uint16_t s = lv_dropdown_get_selected(d->dd);
        if (s > 0) lv_dropdown_set_selected(d->dd, s - 1);
        refresh_label(d);
    } else if (k == LV_KEY_DOWN) {
        uint16_t s = lv_dropdown_get_selected(d->dd);
        if (s + 1 < lv_dropdown_get_option_count(d->dd))
            lv_dropdown_set_selected(d->dd, s + 1);
        refresh_label(d);
    } else if (k == LV_KEY_ESC) {
        lv_dropdown_set_selected(d->dd, d->saved_sel);
        refresh_label(d);
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
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

    /* hidden dropdown — we drive it with keys, not its own popup. */
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);

    pp_dd_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->dd     = dd;
    d->value_label = value_label;
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
