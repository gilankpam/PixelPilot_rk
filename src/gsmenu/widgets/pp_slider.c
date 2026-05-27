#include "pp_slider.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *slider, *value_label;
    int32_t saved_val;
} pp_slider_data_t;

static void on_delete(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void update_label(pp_slider_data_t *d) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)lv_slider_get_value(d->slider));
    lv_label_set_text(d->value_label, buf);
}

static void on_key(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    bool consumed = false;
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            d->saved_val = lv_slider_get_value(d->slider);
            control_mode = GSMENU_CONTROL_MODE_SLIDER;
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[32];
            snprintf(buf, sizeof buf, "%d", (int)lv_slider_get_value(d->slider));
            pp_settings_set_async(d->domain, d->page, d->key, buf, NULL);
        }
        consumed = true;
    } else if (k == LV_KEY_RIGHT) {
        lv_slider_set_value(d->slider, lv_slider_get_value(d->slider) + 1, LV_ANIM_OFF);
        update_label(d);
        consumed = true;
    } else if (k == LV_KEY_LEFT) {
        lv_slider_set_value(d->slider, lv_slider_get_value(d->slider) - 1, LV_ANIM_OFF);
        update_label(d);
        consumed = true;
    } else if (k == LV_KEY_ESC) {
        if (control_mode == GSMENU_CONTROL_MODE_SLIDER) {
            lv_slider_set_value(d->slider, d->saved_val, LV_ANIM_OFF);
            update_label(d);
        }
        control_mode = GSMENU_CONTROL_MODE_NAV;
        consumed = true;
    }
    /* Stop bubbling for any key we handle: otherwise LVGL's base event
     * handler on the parent scrollable page would auto-scroll on
     * LEFT/RIGHT/UP/DOWN. HOME isn't consumed here so it still bubbles
     * to pp_page::on_key for the back-to-tabbar handling. */
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

    lv_obj_t *slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_width(slider, 80);
    lv_obj_add_state(slider, LV_STATE_DISABLED);    /* keys-only adjust */

    pp_slider_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->slider = slider;
    d->value_label = value_label;

    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        lv_slider_set_value(slider, atoi(v), LV_ANIM_OFF);
        update_label(d);
    }
    free(v);

    return row;
}
