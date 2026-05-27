#include "dynamiclink.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"

#include <string.h>
#include <stdlib.h>

/* Hide all rows except the Enabled toggle when dynamicLink is off.
 * Visibility is recomputed on every snapshot mutation. */
static void apply_visibility(lv_obj_t *page) {
    char *v = pp_settings_get("air", "dlink", "enabled");
    bool on = v && strcmp(v, "on") == 0;
    free(v);

    uint32_t n = lv_obj_get_child_cnt(page);
    bool past_enabled = false;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        /* The Enabled toggle is marked with LV_OBJ_FLAG_USER_3 by the
         * builder below. Any row past the marker is conditional. */
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_3)) { past_enabled = true; continue; }
        if (!past_enabled) continue;
        if (on) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
}

static void snapshot_listener_cb(void *user_data) {
    lv_obj_t *page = (lv_obj_t *)user_data;
    apply_visibility(page);
}

lv_obj_t *build_dynamiclink_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "dlink");

    pp_section_header(page, "General");
    lv_obj_t *enabled = pp_toggle(page, LV_SYMBOL_POWER, "Enabled",
                                  "air", "dlink", "enabled");
    lv_obj_add_flag(enabled, LV_OBJ_FLAG_USER_3);   /* visibility anchor */
    pp_toggle(page, LV_SYMBOL_SETTINGS, "Interleaving Supported",
              "air", "dlink", "interleaving");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "MAVLink Enable",
              "air", "dlink", "mavlink_enable");

    pp_section_header(page, "OSD");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "OSD Enabled",
              "air", "dlink", "osd_enabled");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Debug Latency",
              "air", "dlink", "osd_debug_latency");

    pp_section_header(page, "Timing");
    pp_slider(page, LV_SYMBOL_REFRESH, "Health Timeout (ms)",
              "air", "dlink", "health_timeout_ms", 1000, 30000);
    pp_slider(page, LV_SYMBOL_REFRESH, "Min IDR Interval (ms)",
              "air", "dlink", "min_idr_interval_ms", 16, 2000);
    pp_slider(page, LV_SYMBOL_REFRESH, "Apply Stagger (ms)",
              "air", "dlink", "apply_stagger_ms", 0, 500);
    pp_slider(page, LV_SYMBOL_REFRESH, "Apply Sub-pace (ms)",
              "air", "dlink", "apply_subpace_ms", 0, 50);

    pp_section_header(page, "ROI QP");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Threshold (kbps)",
              "air", "dlink", "roiqp_threshold_kbps", 1000, 20000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Low Anchor (kbps)",
              "air", "dlink", "roiqp_low_anchor_kbps", 500, 10000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Floor",
              "air", "dlink", "roiqp_floor", -48, 0);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Step",
              "air", "dlink", "roiqp_step", 1, 10);

    pp_section_header(page, "Failsafe");
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS",
              "air", "dlink", "safe_mcs", 0, 7);
    lv_obj_t *safe_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC K",
                                 "air", "dlink", "safe_k", 1, 31);
    lv_obj_t *safe_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC N",
                                 "air", "dlink", "safe_n", 2, 32);
    pp_slider_set_relation(safe_k, "air", "dlink", "safe_n", -2, /*is_max*/ true);
    pp_slider_set_relation(safe_n, "air", "dlink", "safe_k",  2, /*is_max*/ false);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Block Depth",
              "air", "dlink", "safe_depth", 1, 8);
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "air", "dlink", "safe_bandwidth", "10\n20\n40");
    pp_slider(page, LV_SYMBOL_UP, "TX Power (dBm)",
              "air", "dlink", "safe_txpower_dbm", -10, 30);
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate (kbps)",
              "air", "dlink", "safe_bitrate_kbps", 500, 30000);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }

    apply_visibility(page);
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
}
