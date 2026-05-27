#include "link.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"

/* Keys mirror the originals in src/gsmenu/air_wfbng.c, gs_wfbng.c,
 * air_alink.c, air_aalink.c, gs_apfpv.c. Option strings are reasonable
 * defaults — a real backend will populate them from the air/gs unit. */

static void build_txprofiles_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Profiles");
    pp_row_text(body, LV_SYMBOL_LIST,
                "(no profiles — stub backend)", NULL);
}

static void on_open_txprofiles(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "TX Profiles", build_txprofiles_drilldown, NULL);
}

lv_obj_t *build_link_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "link");

    pp_section_header(page, "WFB-NG");
    /* GS-side channel (shared with air via WFB association). */
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "wfbng", "gs_channel",
                "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n149\n153\n157\n161\n165");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "gs", "wfbng", "bandwidth", "20\n40\n80");
    pp_slider(page, LV_SYMBOL_UP, "TX Power (%)",
              "gs", "wfbng", "txpower", 0, 100);
    /* Air-side radio params. */
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS Index",
              "air", "wfbng", "mcs_index", 0, 11);
    pp_toggle(page, LV_SYMBOL_SETTINGS, "STBC", "air", "wfbng", "stbc");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "LDPC", "air", "wfbng", "ldpc");
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
              "air", "wfbng", "fec_k", 1, 32);
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
              "air", "wfbng", "fec_n", 1, 32);

    pp_section_header(page, "Adaptive Link");
    pp_toggle(page, LV_SYMBOL_REFRESH, "GS Enabled",
              "gs", "alink", "adaptivelink");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Air Enabled",
              "air", "alink", "adaptivelink");

    pp_section_header(page, "AALink");
    pp_dropdown(page, LV_SYMBOL_UP, "Power level (0–4)",
                "air", "aalink", "power_level_0_to_4",
                "0\n1\n2\n3\n4");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Fallback (ms)",
              "air", "aalink", "fallback_ms", 0, 5000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Hold fallback (s)",
              "air", "aalink", "hold_fallback_mode_s", 0, 60);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Min between changes (ms)",
              "air", "aalink", "min_between_changes_ms", 0, 5000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Hold modes down (s)",
              "air", "aalink", "hold_modes_down_s", 0, 60);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Hysteresis (%)",
              "air", "aalink", "hysteresis_percent", 0, 100);
    pp_toggle(page, LV_SYMBOL_REFRESH, "Allow keyframe request",
              "air", "aalink", "allow_request_keyframe");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Keyframe on link change",
              "air", "aalink", "idr_every_change");

    pp_section_header(page, "AP-FPV");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "apfpv", "channel",
                "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11");

    pp_section_header(page, "TX Profiles");
    lv_obj_t *tx_row = pp_row_text(page, LV_SYMBOL_LIST,
                                   "Manage profiles…", NULL);
    lv_obj_add_event_cb(tx_row, on_open_txprofiles, LV_EVENT_KEY, NULL);

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
