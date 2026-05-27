#include <lvgl.h>
#include "styles.h"
#include "settings.h"
#include "widgets/pp_section_header.h"
#include "widgets/pp_row.h"
#include "widgets/pp_toggle.h"
#include "widgets/pp_slider.h"
#include "widgets/pp_dropdown.h"
#include "widgets/pp_page.h"

static lv_obj_t *demo_root = NULL;

lv_obj_t *demo_root_obj(void) { return demo_root; }

void pp_widget_demo_main(void) {
    style_init();
    pp_settings_register_stub();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    lv_obj_t *page = pp_page_create(scr, "air", "camera");
    lv_obj_set_size(page, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(page, 0, 0);
    demo_root = page;

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "resolution",
                "1080p60\n720p120\n540p60");
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate",
              "air", "camera", "bitrate", 1, 50);

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");

    pp_section_header(page, "Info");
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Version", NULL);

    /* Add every focusable row to the page group so W/S navigates them.
     * Section headers are lv_label_t and are not added (not focusable). */
    extern lv_indev_t *indev_drv;
    extern lv_group_t *default_group;
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            /* lv_obj_create-based rows; section headers are lv_label_t. */
            lv_group_add_obj(grp, c);
        }
    }
    default_group = grp;
    lv_indev_set_group(indev_drv, grp);
}
