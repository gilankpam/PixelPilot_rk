#include "pixelpilot.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"
#include "../widgets/pp_toast.h"
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

/* ---- DVR playback drilldown (read-only stub, unchanged from dvr.c) ---- */
static void build_playback_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Recordings");
    pp_row_text(body, LV_SYMBOL_VIDEO, "(no recordings — stub backend)", NULL);
}
static void on_open_playback(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "Playback", build_playback_drilldown, NULL);
}

/* ---- Apply button: POST /apply (one pixelpilot restart for the batch) ----
 * (A live "dirty" marker is deferred: the snapshot-listener API has no
 * per-listener removal, so a page-scoped listener can't be torn down safely.
 * The backend exposes pp_settings_has_pending() for a future authoritative
 * indicator.) */
static void apply_done_cb(int rc, const char *err, void *user_data) {
    lv_obj_t *row = (lv_obj_t *)user_data;
    pp_row_set_busy(row, false);
    if (rc != 0) pp_toast_error(err ? err : "Apply failed");
}
static void on_apply(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row = lv_event_get_target(e);
    pp_row_set_busy(row, true);
    pp_settings_apply(apply_done_cb, row);
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
    pp_slider(page, LV_SYMBOL_SETTINGS, "RTP Jitter (ms)",
              "gs", "display", "rtp_jitter_ms", 0, 50);
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Color correction",
              "gs", "display", "color_correction");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
              "gs", "display", "cc_offset", -50, 50);

    pp_section_header(page, "DVR · Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "gs", "dvr", "rec_enabled");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Mode",
                "gs", "dvr", "dvr_mode", "raw\nreencode");
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max file size (MB)",
              "gs", "dvr", "dvr_max_size", 100, 16000);

    pp_dropdown(page, LV_SYMBOL_AUDIO, "Re-encode bitrate (kbps)",
                "gs", "dvr", "dvr_reenc_bitrate",
                "4000\n8000\n12000\n16000\n25000");

    pp_section_header(page, "Playback");
    lv_obj_t *pb_row = pp_row_text(page, LV_SYMBOL_PLAY, "Browse recordings…", NULL);
    lv_obj_add_event_cb(pb_row, on_open_playback, LV_EVENT_KEY, NULL);

    pp_section_header(page, "Apply");
    lv_obj_t *apply_row = pp_row_text(page, LV_SYMBOL_OK, "Apply changes", NULL);
    lv_obj_add_event_cb(apply_row, on_apply, LV_EVENT_KEY, NULL);

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
