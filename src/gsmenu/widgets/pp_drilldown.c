#include "pp_drilldown.h"
#include "../styles.h"
#include <stdlib.h>

extern const lv_font_t *pp_font_geist_22(void);

static lv_obj_t   *g_overlay = NULL;
static lv_obj_t   *g_body    = NULL;
static lv_group_t *g_group   = NULL;
static lv_group_t *g_prev_group = NULL;
static lv_obj_t   *g_dimmed_page = NULL;

extern lv_indev_t *indev_drv;

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_HOME) pp_drilldown_close();
}

static void anim_x_cb(void *obj, int32_t v) {
    lv_obj_set_x((lv_obj_t *)obj, v);
}

lv_obj_t *pp_drilldown_open(lv_obj_t *anchor_page, const char *title,
                            pp_drilldown_build_fn build, void *user) {
    if (g_overlay) pp_drilldown_close();

    lv_obj_t *parent = lv_obj_get_parent(anchor_page);
    g_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_add_style(g_overlay, &pp_style_panel, 0);
    lv_obj_set_size(g_overlay, LV_PCT(78), LV_PCT(100));
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);

    /* Slide-in from the right edge of the panel. */
    int32_t panel_w = lv_obj_get_width(parent);
    if (panel_w <= 0) panel_w = lv_display_get_horizontal_resolution(NULL);
    lv_obj_set_pos(g_overlay, panel_w, 0);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, g_overlay);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, panel_w, 0);
    lv_anim_set_duration(&a, 180);
    lv_anim_start(&a);

    /* Dim the underlying anchor page so it's still visible. */
    g_dimmed_page = anchor_page;
    lv_obj_set_style_bg_opa(anchor_page, LV_OPA_60, 0);

    lv_obj_t *header = lv_label_create(g_overlay);
    lv_label_set_text(header, title);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(header, pp_font_geist_22(), 0);
    lv_obj_set_style_pad_all(header, 12, 0);

    g_body = lv_obj_create(g_overlay);
    lv_obj_remove_style_all(g_body);
    lv_obj_set_width(g_body, LV_PCT(100));
    lv_obj_set_flex_grow(g_body, 1);
    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_body, LV_DIR_VER);

    g_group = lv_group_create();
    g_prev_group = lv_indev_get_group(indev_drv);
    lv_indev_set_group(indev_drv, g_group);

    lv_obj_add_event_cb(g_overlay, on_key, LV_EVENT_KEY, NULL);

    if (build) build(g_body, user);
    /* Auto-add focusable body children to the drilldown group. */
    uint32_t n = lv_obj_get_child_cnt(g_body);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(g_body, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(g_group, c);
        }
    }
    return g_overlay;
}

void pp_drilldown_close(void) {
    if (!g_overlay) return;
    if (g_dimmed_page) {
        lv_obj_set_style_bg_opa(g_dimmed_page, LV_OPA_COVER, 0);
        g_dimmed_page = NULL;
    }
    if (g_prev_group) lv_indev_set_group(indev_drv, g_prev_group);
    if (g_group) lv_group_del(g_group);
    lv_obj_del(g_overlay);
    g_overlay = NULL;
    g_body = NULL;
    g_group = NULL;
}
