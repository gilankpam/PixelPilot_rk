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
    { "bitrate",      "12000" },
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

    /* Camera — Resilience + OSD */
    { "resilience",   "off" },
    { "osd_enabled",  "off" },

    /* Link — WFB-NG */
    { "gs_channel",   "149" },
    { "bandwidth",    "40" },
    { "txpower",      "20" },
    { "rx_power",     "20" },
    { "beamforming",  "off" },
    { "mcs_index",    "2" },
    { "stbc",         "off" },
    { "ldpc",         "on" },
    { "fec_k",        "8" },
    { "fec_n",        "12" },
    { "fec_mode",          "swfec" },
    { "fec_deadline_ms",   "30" },
    { "fec_overhead_pct",  "50" },

    /* Dynamic Link — General */
    { "enabled",            "off" },
    /* Dynamic Link — Compute */
    { "compute_base_redundancy",  "0.5" },
    { "compute_blocks_per_frame", "2.0" },
    { "compute_min_bitrate_kbps", "1000" },
    { "compute_max_bitrate_kbps", "24000" },
    { "max_mcs",                  "5" },

    /* Display */
    { "video_scale",      "100" },
    { "screen_mode",      "1920x1080@60" },
    { "color_correction", "off" },
    { "cc_gain",          "25" },
    { "cc_offset",        "0" },

    /* DVR (kept) */
    { "dvr_mode",             "reencode" },
    { "dvr_max_size",         "4000" },
    { "dvr_reenc_bitrate",    "8000" },

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

/* Single-slot listener — the dispatcher's fanout function plugs in here
 * and re-broadcasts to its internal subscriber list. */
static pp_settings_snapshot_cb g_dummy_listener_cb = NULL;
static void                   *g_dummy_listener_ud = NULL;

static void dummy_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    g_dummy_listener_cb = cb;
    g_dummy_listener_ud = ud;
}

static void dummy_fire_listener(void) {
    if (g_dummy_listener_cb) g_dummy_listener_cb(g_dummy_listener_ud);
}

/* UI keys that fpvd would mark read-only when dynamicLink.enabled=true.
 * Mirrors the LOCKED_PATHS list in settings_fpvd.c at the UI-key level. */
static const char *g_dummy_locked_keys[] = {
    "mcs_index", "txpower", "fec_k", "fec_n",
    "fec_deadline_ms", "fec_overhead_pct",
    "bandwidth", /* link.width */
    "bitrate",
    "qp_delta",
    "roi_enabled", "roi_qp", "roi_center", "roi_steps",
};

static bool g_drone_offline = false;   /* set in pp_settings_register_dummy */

/* Mirrors the fpvd provider: drone-backed rows are the air domain plus the
 * two gs-domain drone rows (drone TX power, beamforming handshake). */
static bool dummy_is_reachable(const char *d, const char *p, const char *k) {
    if (!g_drone_offline || !d || !p || !k) return true;
    if (strcmp(d, "air") == 0) return false;
    if (strcmp(d, "gs") == 0 && strcmp(p, "wfbng") == 0 && strcmp(k, "txpower") == 0)
        return false;
    if (strcmp(d, "gs") == 0 && strcmp(p, "link") == 0 && strcmp(k, "beamforming") == 0)
        return false;
    return true;
}

static bool dummy_is_locked(const char *d, const char *p, const char *k) {
    (void)d; (void)p;
    const char *enabled = find_value("enabled");
    bool dlink_on = enabled && strcmp(enabled, "on") == 0;
    if (!dlink_on) return false;

    const char *mode = find_value("fec_mode");
    bool swfec = mode && strcmp(mode, "swfec") == 0;

    /* swfec: deadline/overhead become editable; the compute redundancy/blocks
     * knobs become locked. Mirrors dl_locks_field() in settings_fpvd.c. */
    if (swfec && (strcmp(k, "fec_deadline_ms") == 0 ||
                  strcmp(k, "fec_overhead_pct") == 0))
        return false;
    if (swfec && (strcmp(k, "compute_base_redundancy") == 0 ||
                  strcmp(k, "compute_blocks_per_frame") == 0))
        return true;

    for (size_t i = 0; i < sizeof(g_dummy_locked_keys)/sizeof(g_dummy_locked_keys[0]); i++) {
        if (strcmp(g_dummy_locked_keys[i], k) == 0) return true;
    }
    return false;
}

static void dummy_set(const char *d, const char *p, const char *k, const char *v) {
    LV_LOG_USER("dummy.set %s/%s/%s = %s", d, p, k, v ? v : "(null)");
    overlay_set(k, v);
    dummy_fire_listener();
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

static bool dummy_is_available(const char *d, const char *p, const char *k) {
    (void)d; (void)p; (void)k;
    /* sim: every row is live — the dummy has no keymap to gate on. Intentionally
     * diverges from the real prov_is_available (keymap-gated) so new UI rows stay
     * exercisable in the simulator before their fpvd route exists. */
    return true;
}
static void dummy_apply(pp_settings_done_cb cb, void *ud) {
    if (cb) cb(0, NULL, ud);                   /* sim: no-op success */
}
static bool dummy_has_pending(void) { return false; }

static const pp_settings_provider_t g_dummy = {
    .set       = dummy_set,
    .get       = dummy_get,
    .set_async = dummy_set_async,
    .is_locked = dummy_is_locked,
    .is_reachable = dummy_is_reachable,
    .set_snapshot_listener = dummy_set_snapshot_listener,
    .is_available = dummy_is_available,
    .apply        = dummy_apply,
    .has_pending  = dummy_has_pending,
};

void pp_settings_register_dummy(void) {
    g_drone_offline = getenv("PP_SIM_DRONE_OFFLINE") != NULL;
    pp_settings_register(&g_dummy);
}
