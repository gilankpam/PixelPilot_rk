#include <stdio.h>
#include <stdbool.h>
#include "../../lvgl/lvgl.h"
#include "helper.h"
#include "settings.h"
#include "widgets/pp_row.h"
#include "widgets/pp_page.h"

/* Minimal helper retained after the GSMenu redesign. The simulator uses
 * find_resource_file() to locate icon/background assets via LVGL's FS API.
 * All the legacy create / reload / generic helpers were removed along
 * with the old air and gs page builders. */

static bool file_exists(const char *path) {
    lv_fs_file_t f;
    lv_fs_res_t res = lv_fs_open(&f, path, LV_FS_MODE_RD);
    if (res == LV_FS_RES_OK) {
        lv_fs_close(&f);
        return true;
    }
    return false;
}

/* All three widget data structs (pp_toggle_data_t in pp_toggle.c,
 * pp_slider_data_t in pp_slider.c, pp_dd_data_t in pp_dropdown.c) start
 * with the same three fields: char *domain, *page, *key. We reinterpret
 * lv_obj_get_user_data() as this common head to look up lock state without
 * needing to know which specific widget the row is. Rows whose user_data
 * is null or has null d/p/k pointers are silently skipped (covers section
 * headers and any future non-key rows). */
struct dpk_head { char *d; char *p; char *k; };

void pp_page_reapply_lock_state(lv_obj_t *page) {
    bool connected = pp_settings_is_connected();
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        struct dpk_head *h = (struct dpk_head *)lv_obj_get_user_data(c);
        if (!h || !h->d || !h->p || !h->k) continue;
        if (!pp_settings_is_available(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_UNAVAILABLE);
        } else if (!connected || !pp_settings_is_reachable(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_OFFLINE);
        } else if (pp_settings_is_locked(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_DYNAMIC);
        } else {
            pp_row_set_locked(c, PP_ROW_UNLOCKED);
        }
    }
    /* Locking may have disabled the row the user is sitting on (or the
     * whole page); don't leave the indev on a group that can't take keys. */
    pp_page_rescue_focus(page);
}

const char *find_resource_file(const char *relative_path) {
    static char path[256];

    const char *prefixes[] = {
        "/usr/local/share/pixelpilot",
        "/usr/share/pixelpilot",
        "./src/icons",
    };

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "A:%s/%s", prefixes[i], relative_path);
        if (file_exists(path)) return path;
    }
    return NULL;
}
