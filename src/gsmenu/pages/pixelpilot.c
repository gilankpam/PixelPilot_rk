#include "pixelpilot.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"
#include "../settings_runtime_cfg.h"
#include "../helper.h"
#include <stdlib.h>

/* ---- Recording-watch timer: re-apply row lock when DVR starts/stops ----
 * Recording is toggled out-of-band (SIGUSR1), not through the settings
 * snapshot, so nothing else re-evaluates row lock state.  A 500 ms timer
 * polls the in-process flag and calls pp_page_reapply_lock_state only when
 * the flag changes.  The page user_data is owned by pp_page_data_t (in
 * pp_page.c), so we keep our own context struct in the timer user_data and
 * free it via LV_EVENT_DELETE on the page. */
typedef struct {
    lv_obj_t  *page;
    lv_timer_t *timer;
    int        last;
} rec_watch_ctx_t;

static void rec_watch_timer_cb(lv_timer_t *t) {
    rec_watch_ctx_t *ctx = (rec_watch_ctx_t *)lv_timer_get_user_data(t);
    int now = pp_runtime_cfg_is_recording() ? 1 : 0;
    if (ctx->last != now) {
        ctx->last = now;
        pp_page_reapply_lock_state(ctx->page);
    }
}

static void rec_watch_page_delete_cb(lv_event_t *e) {
    rec_watch_ctx_t *ctx = (rec_watch_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    lv_timer_delete(ctx->timer);
    free(ctx);
}

lv_obj_t *build_pixelpilot_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "pixelpilot");

    pp_section_header(page, "Display");
    pp_dropdown(page, LV_SYMBOL_EYE_OPEN, "Screen Mode",
                "gs", "display", "screen_mode",
                "1920x1080@60\n1920x1080@120\n1280x720@60\n1280x720@120\n"
                "2560x1440@60\n3840x2160@60");
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 100);
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Color correction",
              "gs", "display", "color_correction");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
              "gs", "display", "cc_offset", -50, 50);

    pp_section_header(page, "DVR · Recording");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Mode",
                "gs", "dvr", "dvr_mode", "raw\nreencode\nboth");
    static const pp_slider_cfg_t maxsize_cfg = {
        .raw_min = 500, .raw_max = 16000, .step = 500, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 1000, .decimals = 1,
        .unit = "GB", .serialize = PP_SER_INT,
    };
    pp_slider_ex(page, LV_SYMBOL_SD_CARD, "Max file size",
                 "gs", "dvr", "dvr_max_size", &maxsize_cfg);

    pp_dropdown_units(page, LV_SYMBOL_AUDIO, "Re-encode bitrate",
                      "gs", "dvr", "dvr_reenc_bitrate",
                      "4000\n8000\n12000\n16000\n25000", "Mbps", 1000);

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

    /* Drive recording-aware lock state for the DVR rows.  We can't use
     * lv_obj_set_user_data(page, ...) because pp_page.c already owns it
     * for pp_page_data_t, so the context lives in the timer user_data and
     * is freed by the LV_EVENT_DELETE handler below. */
    rec_watch_ctx_t *ctx = (rec_watch_ctx_t *)malloc(sizeof(*ctx));
    if (ctx) {
        ctx->page  = page;
        ctx->last  = pp_runtime_cfg_is_recording() ? 1 : 0;
        ctx->timer = lv_timer_create(rec_watch_timer_cb, 500, ctx);
        lv_obj_add_event_cb(page, rec_watch_page_delete_cb, LV_EVENT_DELETE, ctx);
    }
    pp_page_reapply_lock_state(page);   /* initial state */

    return page;
}
