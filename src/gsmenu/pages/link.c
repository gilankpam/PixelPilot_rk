#include "link.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"
#include <string.h>
#include <stdlib.h>

extern void pp_page_reapply_lock_state(lv_obj_t *);

/* Show exactly one FEC parameter group based on link.fec.mode.
 * rs    -> FEC_K / FEC_N      (rows tagged LV_OBJ_FLAG_USER_1)
 * swfec -> Deadline / Overhead (rows tagged LV_OBJ_FLAG_USER_2)
 * An unknown/empty mode hides both groups until the snapshot arrives. */
static void apply_fec_visibility(lv_obj_t *page) {
    char *v = pp_settings_get("air", "wfbng", "fec_mode");
    bool is_rs    = v && strcmp(v, "rs") == 0;
    bool is_swfec = v && strcmp(v, "swfec") == 0;
    free(v);

    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            if (is_rs) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            else       lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        } else if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            if (is_swfec) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void snapshot_listener_cb(void *user_data) {
    lv_obj_t *page = (lv_obj_t *)user_data;
    apply_fec_visibility(page);
    pp_page_reapply_lock_state(page);
}

lv_obj_t *build_link_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "link");

    pp_section_header(page, "WFB-NG");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "wfbng", "gs_channel",
                "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n"
                "52\n56\n60\n64\n100\n104\n108\n112\n116\n120\n124\n128\n132\n136\n140\n144\n"
                "149\n153\n157\n161\n165");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "gs", "wfbng", "bandwidth", "10\n20\n40");
    /* dBm sliders: API range -10..30, integer steps, dim unit label. */
    static const pp_slider_cfg_t DBM_CFG = {
        .raw_min = -10, .raw_max = 30, .step = 1, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 1, .decimals = 0,
        .unit = "dBm", .serialize = PP_SER_INT,
    };
    pp_slider_ex(page, LV_SYMBOL_UP, "TX Power",
                 "gs", "wfbng", "txpower", &DBM_CFG);
    pp_slider_ex(page, LV_SYMBOL_DOWN, "RX Power",
                 "gs", "link", "rx_power", &DBM_CFG);
    pp_toggle(page, LV_SYMBOL_WIFI, "Beamforming", "gs", "link", "beamforming");
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS Index",
              "air", "wfbng", "mcs_index", 0, 7);
    pp_toggle(page, LV_SYMBOL_SETTINGS, "STBC", "air", "wfbng", "stbc");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "LDPC", "air", "wfbng", "ldpc");
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "FEC Mode",
                "air", "wfbng", "fec_mode", "rs\nswfec");

    lv_obj_t *fec_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
                                "air", "wfbng", "fec_k", 1, 31);
    lv_obj_t *fec_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
                                "air", "wfbng", "fec_n", 2, 32);
    /* Enforce k <= n - 2 from both sides. */
    pp_slider_set_relation(fec_k, "air", "wfbng", "fec_n", -2, /*is_max*/ true);
    pp_slider_set_relation(fec_n, "air", "wfbng", "fec_k",  2, /*is_max*/ false);

    lv_obj_t *fec_deadline = pp_slider(page, LV_SYMBOL_SETTINGS, "Deadline (ms)",
                                       "air", "wfbng", "fec_deadline_ms", 10, 50);
    lv_obj_t *fec_overhead = pp_slider(page, LV_SYMBOL_SETTINGS, "Overhead (%)",
                                       "air", "wfbng", "fec_overhead_pct", 0, 100);

    /* Conditional groups: rs -> k/n, swfec -> deadline/overhead. */
    lv_obj_add_flag(fec_k,        LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(fec_n,        LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(fec_deadline, LV_OBJ_FLAG_USER_2);
    lv_obj_add_flag(fec_overhead, LV_OBJ_FLAG_USER_2);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }

    /* The dispatcher supports multiple listeners via fanout, so this
     * registration coexists with the Dynamic Link tab's own listener. */
    apply_fec_visibility(page);
    pp_page_reapply_lock_state(page);
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
}
