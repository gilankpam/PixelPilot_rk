#include <stdio.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "input.h"
#include "menu.h"
#include "gsmenu/ui.h"
#include "gsmenu/styles.h"
#include "gsmenu/widgets/pp_tabbar.h"
#include "gsmenu/widgets/pp_page.h"
#include "gsmenu/pages/display.h"
#include "gsmenu/pages/camera.h"
#include "gsmenu/pages/link.h"
#include "gsmenu/pages/dvr.h"
#include "gsmenu/pages/system.h"
#include "lvosd.h"

lv_obj_t   *menu;                   /* Legacy; kept for ABI w/ old code paths. */
lv_indev_t *indev_drv;
lv_group_t *default_group;
lv_obj_t   *pp_menu_screen;
lv_obj_t   *pp_osd_screen;
lv_obj_t   *dvr_screen;             /* Legacy. */
lv_obj_t   *txprofiles_screen;      /* Legacy. */
lv_group_t *main_group;

void pp_menu_main(void)
{
    style_init();

    indev_drv = create_virtual_keyboard();

    default_group = lv_group_create();
    lv_group_set_default(default_group);
    lv_indev_set_group(indev_drv, default_group);

    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(pp_menu_screen);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(pp_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Root row inside the menu screen: tabbar (left) + tab pages (right).
     * Anchored to left edge; takes 78% width to keep video visible on the right. */
    lv_obj_t *root = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);

    /* Build pages — Camera + Display are real, others are placeholders. */
    lv_obj_t *cam = build_camera_tab(root);
    lv_obj_set_flex_grow(cam, 1);
    lv_obj_set_height(cam, LV_PCT(100));
    lv_obj_t *lnk = build_link_tab(root);
    lv_obj_set_flex_grow(lnk, 1);
    lv_obj_set_height(lnk, LV_PCT(100));
    lv_obj_t *dsp = build_display_tab(root);
    lv_obj_set_flex_grow(dsp, 1);
    lv_obj_set_height(dsp, LV_PCT(100));
    lv_obj_t *dvr = build_dvr_tab(root);
    lv_obj_set_flex_grow(dvr, 1);
    lv_obj_set_height(dvr, LV_PCT(100));
    lv_obj_t *sys = build_system_tab(root);
    lv_obj_set_flex_grow(sys, 1);
    lv_obj_set_height(sys, LV_PCT(100));

    pp_tabbar_item_t items[5] = {
        { "Camera",  LV_SYMBOL_IMAGE,     cam },
        { "Link",    LV_SYMBOL_WIFI,      lnk },
        { "Display", LV_SYMBOL_EYE_OPEN,  dsp },
        { "DVR",     LV_SYMBOL_VIDEO,     dvr },
        { "System",  LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(root, items, 5);
    lv_obj_move_to_index(pp_tabbar_root(tabbar), 0);

    /* Each page builder already adds its own rows to its page group.
     * Here we only wire each page's "back target" to the tabbar group
     * so A (HOME) from inside a page returns focus to the tab strip.
     * pp_page_set_back_group also enables LV_OBJ_FLAG_EVENT_BUBBLE on
     * each child so the page's HOME handler actually receives the key. */
    lv_obj_t *pages[5] = { cam, lnk, dsp, dvr, sys };
    main_group = pp_tabbar_group(tabbar);
    for (int i = 0; i < 5; i++) {
        pp_page_set_back_group(pages[i], main_group);
    }

    pp_osd_main();
    lv_screen_load(pp_osd_screen);
}
