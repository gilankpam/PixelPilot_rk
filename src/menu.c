#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "input.h"
#include "menu.h"
#include "gsmenu/settings.h"
#include "gsmenu/ui.h"
#include "gsmenu/styles.h"
#include "gsmenu/widgets/pp_tabbar.h"
#include "gsmenu/widgets/pp_page.h"
#include "gsmenu/widgets/pp_footer.h"
#include "gsmenu/pages/pixelpilot.h"
#include "gsmenu/pages/camera.h"
#include "gsmenu/pages/link.h"
#include "gsmenu/pages/dynamiclink.h"
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

extern bool menu_active;   /* defined in src/input.cpp */

static void on_tabbar_cancel(lv_event_t *e) {
    (void)e;
    /* User pressed HOME while focus was on the tab strip — close the
     * menu and return to the OSD screen. */
    lv_screen_load(pp_osd_screen);
    menu_active = false;
    pp_settings_set_visibility(false);
}

void pp_menu_main(void)
{
    style_init();

    indev_drv = create_virtual_keyboard();

    /* Parking group for the indev until the first menu open switches it to
     * main_group. Deliberately NOT lv_group_set_default(): with a default
     * group set, LVGL auto-adds every group_def widget (lv_dropdown,
     * lv_switch, ...) created during page building, making the hidden
     * native widgets key-focusable — a stray ENTER before the first menu
     * open then acts on invisible UI (e.g. opens a dropdown's unstyled
     * native list, which floats over the menu forever). All gsmenu groups
     * are populated explicitly by the page/tabbar builders. */
    default_group = lv_group_create();
    lv_indev_set_group(indev_drv, default_group);

    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(pp_menu_screen);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, 0);  /* video shows through */
    lv_obj_clear_flag(pp_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Full-frame scrim: a semi-opaque dark child the video shows through.
     * On device the video is the layer below this transparent screen; on the
     * sim a placeholder frame is inserted behind it for screenshots. */
    lv_obj_t *scrim = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(PP_C_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, PP_OPA_SCRIM, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered floating panel: [ rail | content-col ]. Width is proportional
     * with a max cap so it adapts to non-1920 panels. */
    lv_obj_t *panel = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(72));
    lv_obj_set_style_max_width(panel, 1240, 0);
    lv_obj_set_height(panel, LV_PCT(86));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(PP_C_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(panel, 30, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Content column: pages-area (grows) + footer (fixed, added later). Rail is
     * added by pp_tabbar_create below and moved to index 0. */
    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(content, 0, 0);

    lv_obj_t *pages_area = lv_obj_create(content);
    lv_obj_remove_style_all(pages_area);
    lv_obj_set_width(pages_area, LV_PCT(100));
    lv_obj_set_flex_grow(pages_area, 1);
    lv_obj_clear_flag(pages_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(pages_area, 0, 0);

    /* Build the five tab pages into pages_area (was: into the old root). */
    lv_obj_t *cam = build_camera_tab(pages_area);
    lv_obj_set_size(cam, LV_PCT(100), LV_PCT(100));
    lv_obj_t *lnk = build_link_tab(pages_area);
    lv_obj_set_size(lnk, LV_PCT(100), LV_PCT(100));
    lv_obj_t *dl  = build_dynamiclink_tab(pages_area);
    lv_obj_set_size(dl, LV_PCT(100), LV_PCT(100));
    lv_obj_t *pp  = build_pixelpilot_tab(pages_area);
    lv_obj_set_size(pp, LV_PCT(100), LV_PCT(100));
    lv_obj_t *sys = build_system_tab(pages_area);
    lv_obj_set_size(sys, LV_PCT(100), LV_PCT(100));

    pp_footer_create(content);

    pp_tabbar_item_t items[5] = {
        { "Camera",     LV_SYMBOL_IMAGE,     cam },
        { "Link",       LV_SYMBOL_WIFI,      lnk },
        { "DLink",      LV_SYMBOL_LOOP,      dl  },
        { "PixelPilot", LV_SYMBOL_VIDEO,     pp  },
        { "System",     LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(panel, items, 5);
    lv_obj_move_to_index(pp_tabbar_root(tabbar), 0);   /* rail leftmost */
    lv_obj_add_event_cb(pp_tabbar_root(tabbar), on_tabbar_cancel,
                        LV_EVENT_CANCEL, NULL);

    /* Each page builder already adds its own rows to its page group.
     * Here we only wire each page's "back target" to the tabbar group
     * so A (HOME) from inside a page returns focus to the tab strip.
     * pp_page_set_back_group also enables LV_OBJ_FLAG_EVENT_BUBBLE on
     * each child so the page's HOME handler actually receives the key. */
    lv_obj_t *pages[5] = { cam, lnk, dl, pp, sys };
    main_group = pp_tabbar_group(tabbar);
    for (int i = 0; i < 5; i++) {
        pp_page_set_back_group(pages[i], main_group);
    }

    pp_osd_main();
    lv_screen_load(pp_osd_screen);
}
