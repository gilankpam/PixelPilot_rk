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
#include <string.h>

/* ---- Page-state watch (500 ms) ----
 * The PP page reacts to changes the settings snapshot doesn't surface:
 *   1. DVR recording start/stop (SIGUSR1) -> re-lock the DVR config rows.
 *   2. DVR mode -> the Re-encode bitrate row only applies when re-encoding
 *      (hidden in raw mode).
 *   3. Color-correction Enabled -> Gain/Offset only apply when enabled
 *      (hidden when off).
 * A single 500 ms timer polls these and acts only on change. The page
 * user_data is owned by pp_page_data_t (pp_page.c), so the context lives in
 * the timer user_data and is freed via LV_EVENT_DELETE on the page. */
typedef struct {
    lv_obj_t   *page;
    lv_timer_t *timer;
    lv_obj_t   *bitrate_row;     /* hidden when dvr_mode == raw */
    lv_obj_t   *cc_gain_row;     /* hidden when color correction off */
    lv_obj_t   *cc_offset_row;
    int         last_recording;
    int         last_reenc;
    int         last_cc;
} pp_watch_ctx_t;

static void set_hidden(lv_obj_t *row, int hidden) {
    if (!row) return;
    if (hidden) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
}

/* Read settings via the generic provider so these work on device
 * (runtime-cfg) and in the sim (dummy). */
static int dvr_reenc_active(void) {
    char *m = pp_settings_get("gs", "dvr", "dvr_mode");
    int active = (m && strcmp(m, "raw") != 0);
    free(m);
    return active;
}
static int cc_enabled(void) {
    char *v = pp_settings_get("gs", "display", "color_correction");
    int on = (v && strcmp(v, "on") == 0);
    free(v);
    return on;
}

static void pp_watch_apply(pp_watch_ctx_t *ctx) {
    set_hidden(ctx->bitrate_row,   !ctx->last_reenc);
    set_hidden(ctx->cc_gain_row,   !ctx->last_cc);
    set_hidden(ctx->cc_offset_row, !ctx->last_cc);
}

static void pp_watch_timer_cb(lv_timer_t *t) {
    pp_watch_ctx_t *ctx = (pp_watch_ctx_t *)lv_timer_get_user_data(t);
    int rec = pp_runtime_cfg_is_recording() ? 1 : 0;
    if (ctx->last_recording != rec) {
        ctx->last_recording = rec;
        pp_page_reapply_lock_state(ctx->page);
    }
    int reenc = dvr_reenc_active();
    if (ctx->last_reenc != reenc) {
        ctx->last_reenc = reenc;
        set_hidden(ctx->bitrate_row, !reenc);
    }
    int cc = cc_enabled();
    if (ctx->last_cc != cc) {
        ctx->last_cc = cc;
        set_hidden(ctx->cc_gain_row,   !cc);
        set_hidden(ctx->cc_offset_row, !cc);
    }
}

static void pp_watch_page_delete_cb(lv_event_t *e) {
    pp_watch_ctx_t *ctx = (pp_watch_ctx_t *)lv_event_get_user_data(e);
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

    pp_section_header(page, "Color Correction");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Enabled",
              "gs", "display", "color_correction");
    lv_obj_t *cc_gain_row = pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    lv_obj_t *cc_offset_row = pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
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

    lv_obj_t *bitrate_row =
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
    pp_watch_ctx_t *ctx = (pp_watch_ctx_t *)malloc(sizeof(*ctx));
    if (ctx) {
        ctx->page          = page;
        ctx->bitrate_row   = bitrate_row;
        ctx->cc_gain_row   = cc_gain_row;
        ctx->cc_offset_row = cc_offset_row;
        ctx->last_recording = pp_runtime_cfg_is_recording() ? 1 : 0;
        ctx->last_reenc    = dvr_reenc_active();
        ctx->last_cc       = cc_enabled();
        ctx->timer = lv_timer_create(pp_watch_timer_cb, 500, ctx);
        lv_obj_add_event_cb(page, pp_watch_page_delete_cb, LV_EVENT_DELETE, ctx);
        pp_watch_apply(ctx);               /* initial row visibility */
    }
    pp_page_reapply_lock_state(page);      /* initial lock state */

    return page;
}
