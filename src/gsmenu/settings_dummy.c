/* Dummy settings provider for the simulator.
 *
 * Populates each row with a plausible initial value so the menu shows
 * realistic content during development. Writes are not persisted —
 * they only update an in-memory table for the session so toggling /
 * sliding feels responsive (read back returns the new value). */

#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *key;
    const char *value;
} dummy_entry_t;

/* Read-only seed table. Keyed by the "key" component only — sufficient
 * because every page builder uses a unique key. */
static const dummy_entry_t g_seed[] = {
    /* System tab info rows (kept) */
    { "Version",      "1.3.0-sim" },
    { "Disk",         "12.4 / 64 GB" },
    { "Channel",      "149" },
    { "HDMI-OUT",     "1920x1080@60" },
    { "WFB_NICS",     "wlan0" },

    /* Camera — Video */
    { "size",         "1920x1080" },
    { "fps",          "60" },
    { "bitrate",      "15M" },
    { "codec",        "h265" },
    { "gopsize",      "1" },
    { "rc_mode",      "cbr" },
    { "qp_delta",     "-4" },

    /* Camera — ROI */
    { "roi_enabled",  "on" },
    { "roi_qp",       "0" },
    { "roi_center",   "40" },
    { "roi_steps",    "2" },

    /* Camera — Image */
    { "mirror",       "off" },
    { "flip",         "off" },
    { "rotate",       "0" },

    /* Camera — Recording */
    { "rec_enable",   "off" },
    { "rec_split",    "5" },
    { "rec_maxmb",    "500" },

    /* Link — WFB-NG */
    { "gs_channel",   "149" },
    { "bandwidth",    "40" },
    { "txpower",      "20" },
    { "mcs_index",    "2" },
    { "stbc",         "off" },
    { "ldpc",         "on" },
    { "fec_k",        "8" },
    { "fec_n",        "12" },

    /* Dynamic Link — General */
    { "enabled",            "off" },
    { "interleaving",       "on" },
    { "mavlink_enable",     "on" },
    /* Dynamic Link — OSD */
    { "osd_enabled",        "on" },
    { "osd_debug_latency",  "off" },
    /* Dynamic Link — Timing */
    { "health_timeout_ms",  "10000" },
    { "min_idr_interval_ms","500" },
    { "apply_stagger_ms",   "50" },
    { "apply_subpace_ms",   "5" },
    /* Dynamic Link — ROI QP */
    { "roiqp_threshold_kbps", "6000" },
    { "roiqp_low_anchor_kbps","2000" },
    { "roiqp_floor",          "-24" },
    { "roiqp_step",           "3" },
    /* Dynamic Link — Safe Ceilings */
    { "safe_mcs",         "1" },
    { "safe_k",           "8" },
    { "safe_n",           "12" },
    { "safe_depth",       "1" },
    { "safe_bandwidth",   "20" },
    { "safe_txpower_dbm", "20" },
    { "safe_bitrate_kbps","2000" },

    /* Display (kept for the unmodified Display tab) */
    { "hdmi_mode",        "1920x1080@60" },
    { "video_scale",      "100" },
    { "color_correction", "off" },
    { "cc_gain",          "25" },
    { "cc_offset",        "0" },

    /* DVR (kept) */
    { "rec_enabled",          "on" },
    { "dvr_mode",             "reencode" },
    { "rec_fps",              "60" },
    { "dvr_max_size",         "4000" },
    { "dvr_reenc_codec",      "h265" },
    { "dvr_reenc_resolution", "1920x1080" },
    { "dvr_reenc_fps",        "60" },
    { "dvr_reenc_bitrate",    "8000" },
    { "dvr_osd",              "on" },

    /* System — Receiver / Network / Telemetry (kept) */
    { "rx_codec",     "h265" },
    { "rx_mode",      "wfb" },
    { "hotspot",      "off" },
    { "restream",     "off" },
    { "serial",       "ttyS2" },
    { "router",       "mavfwd" },
    { "osd_fps",      "30" },
    { "gs_rendering", "on" },
};

/* Per-session writable overlay so toggle/slider/dropdown changes
 * persist for the lifetime of the simulator process. */
#define OVERLAY_CAP 128
typedef struct {
    char *key;
    char *value;
} overlay_entry_t;

static overlay_entry_t g_overlay[OVERLAY_CAP];
static size_t          g_overlay_n = 0;

static const char *find_value(const char *key) {
    for (size_t i = 0; i < g_overlay_n; i++) {
        if (strcmp(g_overlay[i].key, key) == 0) return g_overlay[i].value;
    }
    for (size_t i = 0; i < sizeof(g_seed) / sizeof(g_seed[0]); i++) {
        if (strcmp(g_seed[i].key, key) == 0) return g_seed[i].value;
    }
    return NULL;
}

static void overlay_set(const char *key, const char *value) {
    for (size_t i = 0; i < g_overlay_n; i++) {
        if (strcmp(g_overlay[i].key, key) == 0) {
            free(g_overlay[i].value);
            g_overlay[i].value = strdup(value ? value : "");
            return;
        }
    }
    if (g_overlay_n < OVERLAY_CAP) {
        g_overlay[g_overlay_n].key   = strdup(key);
        g_overlay[g_overlay_n].value = strdup(value ? value : "");
        g_overlay_n++;
    }
}

static void dummy_set(const char *d, const char *p, const char *k, const char *v) {
    LV_LOG_USER("dummy.set %s/%s/%s = %s", d, p, k, v ? v : "(null)");
    overlay_set(k, v);
}

static char *dummy_get(const char *d, const char *p, const char *k) {
    (void)d; (void)p;
    const char *v = find_value(k);
    return strdup(v ? v : "");
}

typedef struct {
    char *domain, *page, *key, *value;
    pp_settings_done_cb on_done;
    void *user_data;
    bool will_fail;
} dummy_deferred_t;

static void deferred_timer_cb(lv_timer_t *t) {
    dummy_deferred_t *ctx = (dummy_deferred_t *)lv_timer_get_user_data(t);
    if (!ctx->will_fail) {
        dummy_set(ctx->domain, ctx->page, ctx->key, ctx->value);
    }
    if (ctx->on_done) {
        ctx->on_done(ctx->will_fail ? 1 : 0,
                     ctx->will_fail ? "simulated failure (PP_SIM_FAIL set)" : NULL,
                     ctx->user_data);
    }
    free(ctx->domain); free(ctx->page); free(ctx->key); free(ctx->value);
    free(ctx);
    lv_timer_delete(t);
}

static void dummy_set_async(const char *d, const char *p, const char *k,
                            const char *v, pp_settings_done_cb on_done,
                            void *user_data) {
    const char *fail   = getenv("PP_SIM_FAIL");
    const char *latstr = getenv("PP_SIM_LATENCY_MS");
    int latency_ms = latstr ? atoi(latstr) : 200;
    if (latency_ms < 0) latency_ms = 0;

    if (latency_ms == 0) {
        /* Preserve prior synchronous behavior. */
        if (fail && *fail) {
            if (on_done) on_done(1, "simulated failure (PP_SIM_FAIL set)", user_data);
            return;
        }
        dummy_set(d, p, k, v);
        if (on_done) on_done(0, NULL, user_data);
        return;
    }

    dummy_deferred_t *ctx = calloc(1, sizeof(*ctx));
    ctx->domain   = strdup(d);
    ctx->page     = strdup(p);
    ctx->key      = strdup(k);
    ctx->value    = strdup(v ? v : "");
    ctx->on_done  = on_done;
    ctx->user_data= user_data;
    ctx->will_fail= (fail && *fail);
    lv_timer_t *t = lv_timer_create(deferred_timer_cb, latency_ms, ctx);
    lv_timer_set_repeat_count(t, 1);
}

static const pp_settings_provider_t g_dummy = {
    .set       = dummy_set,
    .get       = dummy_get,
    .set_async = dummy_set_async,
};

void pp_settings_register_dummy(void) {
    pp_settings_register(&g_dummy);
}
