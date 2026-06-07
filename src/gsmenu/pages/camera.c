#include "camera.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_slider_scale.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"

/* Keys mirror those in the old src/gsmenu/air_camera.c. Dropdown option
 * strings are sensible defaults — a real backend will query the air unit
 * for valid choices and substitute them. */

lv_obj_t *build_camera_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "camera");

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Size",
                "air", "camera", "size",
                "1920x1080\n1280x720\n960x540");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "FPS",
                "air", "camera", "fps",
                "30\n60\n90\n120");
    static const pp_slider_cfg_t bitrate_cfg = {
        .raw_min = 500, .raw_max = 26000, .step = 500,
        .fine_step = 0, .fine_threshold = 0,
        .disp_div = 1000, .decimals = 1, .unit = "Mbps", .serialize = PP_SER_INT,
    };
    pp_slider_ex(page, LV_SYMBOL_AUDIO, "Bitrate",
                 "air", "camera", "bitrate", &bitrate_cfg);
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "air", "camera", "codec",
                "h264\nh265");
    static const pp_slider_cfg_t gop_cfg = {
        .raw_min = 0, .raw_max = 300, .step = 10,
        .fine_step = 1, .fine_threshold = 10,
        .disp_div = 10, .decimals = 1, .unit = NULL, .serialize = PP_SER_FLOAT_DIV,
    };
    pp_slider_ex(page, LV_SYMBOL_SETTINGS, "GOP size",
                 "air", "camera", "gopsize", &gop_cfg);
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "RC Mode",
                "air", "camera", "rc_mode",
                "cbr\nvbr");
    pp_slider(page, LV_SYMBOL_SETTINGS, "QP Delta",
              "air", "camera", "qp_delta", -32, 0);

    pp_section_header(page, "ROI");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Enabled",
              "air", "camera", "roi_enabled");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "QP",
              "air", "camera", "roi_qp", -24, 0);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Center",
              "air", "camera", "roi_center", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Steps",
              "air", "camera", "roi_steps", 1, 8);

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Rotate",
                "air", "camera", "rotate",
                "0\n90\n180\n270");

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "air", "camera", "rec_enable");
    pp_slider(page, LV_SYMBOL_VIDEO, "Split (min)",
              "air", "camera", "rec_split", 1, 60);
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max size (MB)",
              "air", "camera", "rec_maxmb", 100, 10000);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }

    extern void pp_page_reapply_lock_state(lv_obj_t *);
    /* The dispatcher supports multiple listeners via fanout, so this
     * registration coexists with the Dynamic Link tab's own listener. */
    pp_settings_set_snapshot_listener(
        (pp_settings_snapshot_cb)pp_page_reapply_lock_state, page);
    pp_page_reapply_lock_state(page);
    return page;
}
