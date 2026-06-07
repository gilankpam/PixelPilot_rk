#include "pp_toast.h"
#include "lvgl/lvgl.h"
#include "../styles.h"

/* Singleton state — at most one toast visible at a time. */
static lv_obj_t  *g_toast = NULL;
static lv_timer_t *g_timer = NULL;

static void dismiss_cb(lv_timer_t *t) {
    (void)t;
    if (g_toast) {
        lv_obj_delete(g_toast);
        g_toast = NULL;
    }
    /* Timer is one-shot (repeat_count=1) — LVGL auto-deletes it after
     * it fires, so we only need to null out our reference. */
    g_timer = NULL;
}

static void clear_existing(void) {
    if (g_timer) {
        lv_timer_delete(g_timer);
        g_timer = NULL;
    }
    if (g_toast) {
        lv_obj_delete(g_toast);
        g_toast = NULL;
    }
}

void pp_toast_error(const char *msg) {
    if (!msg || !*msg) return;

    clear_existing();

    /* Parent on the top layer so the toast floats above screens, panels,
     * and the pp_drilldown overlay. lv_layer_top() returns the default
     * display's top layer (v9.5 API). */
    lv_obj_t *top = lv_layer_top();

    lv_obj_t *toast = lv_obj_create(top);
    lv_obj_remove_style_all(toast);

    /* Geometry: 70% width, content height, bottom-center. */
    lv_obj_set_width(toast, LV_PCT(70));
    lv_obj_set_height(toast, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* Background: dark panel, crit border, rounded (design toast). */
    lv_obj_set_style_bg_color(toast, lv_color_hex(PP_C_MODAL), 0);
    lv_obj_set_style_bg_opa(toast, 240, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(PP_C_CRIT), 0);

    /* Padding: 12px horizontal, 8px vertical. */
    lv_obj_set_style_pad_hor(toast, 12, 0);
    lv_obj_set_style_pad_ver(toast, 8, 0);

    /* Drop shadow (v9.5 Gaussian drop_shadow API — same as styles.c). */
    lv_obj_set_style_drop_shadow_radius(toast, 16, 0);
    lv_obj_set_style_drop_shadow_opa(toast, LV_OPA_50, 0);
    lv_obj_set_style_drop_shadow_color(toast, lv_color_black(), 0);
    lv_obj_set_style_drop_shadow_offset_y(toast, 4, 0);

    /* Don't intercept touch/click events. */
    lv_obj_add_flag(toast, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = lv_obj_create(toast);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, PP_SCALE(8), PP_SCALE(8));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(PP_C_CRIT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(dot, PP_SCALE(8), 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_font(label, pp_font_xb_md(), 0);
    lv_obj_set_style_text_letter_space(label, PP_SCALE(1), 0);

    g_toast = toast;

    /* Auto-dismiss after 2 seconds. repeat_count=1 means fire once. */
    g_timer = lv_timer_create(dismiss_cb, 2000, NULL);
    lv_timer_set_repeat_count(g_timer, 1);
}
