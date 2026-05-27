#include <lvgl.h>
#include "styles.h"
#include "widgets/pp_section_header.h"
#include "widgets/pp_row.h"
#include "widgets/pp_toggle.h"

/* Defined here; subsequent widget tasks append to demo_root via demo_root_obj(). */
static lv_obj_t *demo_root = NULL;

lv_obj_t *demo_root_obj(void) { return demo_root; }

void pp_widget_demo_main(void) {
    style_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    demo_root = lv_obj_create(scr);
    lv_obj_set_size(demo_root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(demo_root, 0, 0);
    lv_obj_add_style(demo_root, &pp_style_panel, 0);
    lv_obj_set_flex_flow(demo_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(demo_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(demo_root, LV_DIR_VER);

    pp_section_header(demo_root, "Video");
    pp_section_header(demo_root, "Image");
    pp_section_header(demo_root, "ISP");

    pp_row_text(demo_root, LV_SYMBOL_SETTINGS, "Version", NULL);
    pp_row_text(demo_root, LV_SYMBOL_SETTINGS, "Disk",    NULL);
    pp_row_text(demo_root, LV_SYMBOL_VIDEO,    "Codec",   "codec");

    pp_toggle(demo_root, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(demo_root, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");

    /* Subsequent widget tasks add demo rows here via demo_root_obj(). */
}
