#include <lvgl.h>
#include "styles.h"
#include "settings.h"
#include "widgets/pp_section_header.h"
#include "widgets/pp_row.h"
#include "widgets/pp_toggle.h"
#include "widgets/pp_slider.h"
#include "widgets/pp_dropdown.h"
#include "widgets/pp_page.h"
#include "widgets/pp_tabbar.h"

static lv_obj_t *demo_root = NULL;
lv_obj_t *demo_root_obj(void) { return demo_root; }

static void add_focusable_children(lv_obj_t *page, lv_group_t *grp) {
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }
}

static lv_obj_t *build_demo_tab(lv_obj_t *parent, const char *name) {
    lv_obj_t *page = pp_page_create(parent, "gs", name);
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_height(page, LV_PCT(100));
    pp_section_header(page, name);
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Placeholder", NULL);
    return page;
}

static lv_obj_t *build_camera_demo(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "camera");
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_height(page, LV_PCT(100));

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "resolution",
                "1080p60\n720p120\n540p60");
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate",
              "air", "camera", "bitrate", 1, 50);

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
    return page;
}

void pp_widget_demo_main(void) {
    style_init();
    pp_settings_register_stub();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    /* Root row: tabbar (left) + tab pages (right) */
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    demo_root = root;

    /* Build 5 pages */
    lv_obj_t *cam = build_camera_demo(root);
    lv_obj_t *lnk = build_demo_tab(root, "Link");
    lv_obj_t *dsp = build_demo_tab(root, "Display");
    lv_obj_t *dvr = build_demo_tab(root, "DVR");
    lv_obj_t *sys = build_demo_tab(root, "System");

    /* Tabbar — note items are added AFTER the pages, so move tabbar
     * root to index 0 to put it on the left. */
    pp_tabbar_item_t items[5] = {
        { "Camera",  LV_SYMBOL_IMAGE,     cam },
        { "Link",    LV_SYMBOL_WIFI,      lnk },
        { "Display", LV_SYMBOL_EYE_OPEN,  dsp },
        { "DVR",     LV_SYMBOL_VIDEO,     dvr },
        { "System",  LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(root, items, 5);
    lv_obj_move_to_index(pp_tabbar_root(tabbar), 0);

    /* Wire focus groups: each page gets its rows; default group is the tabbar. */
    lv_obj_t *pages[5] = { cam, lnk, dsp, dvr, sys };
    for (int i = 0; i < 5; i++) {
        add_focusable_children(pages[i], pp_page_group(pages[i]));
    }

    extern lv_indev_t *indev_drv;
    extern lv_group_t *default_group;
    default_group = pp_tabbar_group(tabbar);
    lv_indev_set_group(indev_drv, pp_tabbar_group(tabbar));
}
