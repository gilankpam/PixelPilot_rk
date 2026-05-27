#include "camera.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"

/* Keys mirror those in the old src/gsmenu/air_camera.c. Dropdown option
 * strings are sensible defaults — a real backend will query the air unit
 * for valid choices and substitute them. */

lv_obj_t *build_camera_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "camera");

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Size",
                "air", "camera", "size",
                "1920x1080\n1280x720\n960x540");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Video Mode",
                "air", "camera", "video_mode",
                "normal\nfpv\nhdr");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "FPS",
                "air", "camera", "fps",
                "30\n60\n90\n120");
    pp_dropdown(page, LV_SYMBOL_AUDIO, "Bitrate",
                "air", "camera", "bitrate",
                "5M\n10M\n15M\n20M\n25M");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "air", "camera", "codec",
                "h264\nh265");
    pp_slider(page, LV_SYMBOL_SETTINGS, "GOP size",
              "air", "camera", "gopsize", 0, 60);
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "RC Mode",
                "air", "camera", "rc_mode",
                "cbr\nvbr");

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Contrast",
              "air", "camera", "contrast", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Hue",
              "air", "camera", "hue", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Saturation",
              "air", "camera", "saturation", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Luminance",
              "air", "camera", "luminace", 0, 100);

    pp_section_header(page, "ISP");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Exposure",
              "air", "camera", "exposure", 0, 100);
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "Antiflicker",
                "air", "camera", "antiflicker",
                "off\n50hz\n60hz");
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "Sensor File",
                "air", "camera", "sensor_file",
                "default\nimx415_4k\nimx335");

    pp_section_header(page, "FPV");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Enabled",
              "air", "camera", "fpv_enable");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Noise level",
              "air", "camera", "noiselevel", 0, 100);

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "air", "camera", "rec_enable");
    pp_slider(page, LV_SYMBOL_VIDEO, "Split (min)",
              "air", "camera", "rec_split", 0, 60);
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max usage (%)",
              "air", "camera", "rec_maxusage", 50, 100);

    /* Add focusable rows to the page's group. */
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
