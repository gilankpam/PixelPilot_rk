#include "display.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"

lv_obj_t *build_display_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "display");

    pp_section_header(page, "Output");
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 200);

    pp_section_header(page, "Color");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Color correction",
              "gs", "display", "color_correction");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
              "gs", "display", "cc_offset", -50, 50);

    /* Add focusable rows to the page's group. Section headers are lv_label_t
     * and have a different class, so the check filters them out. */
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }
    return page;
}
