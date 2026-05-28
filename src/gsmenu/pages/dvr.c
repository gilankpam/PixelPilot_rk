#include "dvr.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"

/* Keys mirror the originals in src/gsmenu/gs_system.c (DVR section) and
 * src/gsmenu/gs_dvrplayer.c. */

static void build_playback_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Recordings");
    pp_row_text(body, LV_SYMBOL_VIDEO,
                "(no recordings — stub backend)", NULL);
}

static void on_open_playback(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "Playback", build_playback_drilldown, NULL);
}

lv_obj_t *build_dvr_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "dvr");

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "gs", "dvr", "rec_enabled");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Mode",
                "gs", "dvr", "dvr_mode",
                "raw\nreencode");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Raw FPS",
                "gs", "dvr", "rec_fps",
                "30\n60\n90\n120");
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max file size (MB)",
              "gs", "dvr", "dvr_max_size", 100, 16000);

    pp_section_header(page, "Re-encode");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "gs", "dvr", "dvr_reenc_codec",
                "h264\nh265");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "gs", "dvr", "dvr_reenc_resolution",
                "1920x1080\n1280x720\n854x480");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Re-encode FPS",
                "gs", "dvr", "dvr_reenc_fps",
                "30\n60");
    pp_dropdown(page, LV_SYMBOL_AUDIO, "Bitrate (kbps)",
                "gs", "dvr", "dvr_reenc_bitrate",
                "4000\n8000\n12000\n16000\n25000");

    pp_section_header(page, "Overlay");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Burn OSD into DVR",
              "gs", "dvr", "dvr_osd");

    pp_section_header(page, "Playback");
    lv_obj_t *pb_row = pp_row_text(page, LV_SYMBOL_PLAY,
                                   "Browse recordings…", NULL);
    lv_obj_add_event_cb(pb_row, on_open_playback, LV_EVENT_KEY, NULL);

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
