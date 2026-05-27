#include "link.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"

lv_obj_t *build_link_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "link");

    pp_section_header(page, "WFB-NG");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "wfbng", "gs_channel",
                "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n149\n153\n157\n161\n165");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "gs", "wfbng", "bandwidth", "20\n40");
    pp_slider(page, LV_SYMBOL_UP, "TX Power",
              "gs", "wfbng", "txpower", 1, 63);
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS Index",
              "air", "wfbng", "mcs_index", 0, 7);
    pp_toggle(page, LV_SYMBOL_SETTINGS, "STBC", "air", "wfbng", "stbc");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "LDPC", "air", "wfbng", "ldpc");
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
              "air", "wfbng", "fec_k", 1, 31);
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
              "air", "wfbng", "fec_n", 2, 32);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }

    extern void pp_page_reapply_lock_state(lv_obj_t *);
    /* v1 known limitation: only one snapshot listener slot in the provider
     * — the Dynamic Link tab installs its own, so this one is overwritten
     * when that tab builds. Static lock state at construction time still
     * works. */
    pp_settings_set_snapshot_listener(
        (pp_settings_snapshot_cb)pp_page_reapply_lock_state, page);
    pp_page_reapply_lock_state(page);
    return page;
}
