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
    /* read-only info rows (System tab) */
    { "Version",      "1.3.0-sim" },
    { "Disk",         "12.4 / 64 GB" },
    { "Channel",      "149" },
    { "HDMI-OUT",     "1920x1080@60" },
    { "WFB_NICS",     "wlan0" },

    /* Camera — Video */
    { "size",         "1920x1080" },
    { "video_mode",   "fpv" },
    { "fps",          "60" },
    { "bitrate",      "15M" },
    { "codec",        "h265" },
    { "gopsize",      "30" },
    { "rc_mode",      "cbr" },

    /* Camera — Image */
    { "mirror",       "off" },
    { "flip",         "off" },
    { "contrast",     "50" },
    { "hue",          "50" },
    { "saturation",   "50" },
    { "luminace",     "50" },

    /* Camera — ISP */
    { "exposure",     "50" },
    { "antiflicker",  "50hz" },
    { "sensor_file",  "imx415_4k" },

    /* Camera — FPV / Recording */
    { "fpv_enable",   "on" },
    { "noiselevel",   "20" },
    { "rec_enable",   "off" },
    { "rec_split",    "10" },
    { "rec_maxusage", "80" },

    /* Link — WFB-NG (gs) */
    { "gs_channel",   "149" },
    { "bandwidth",    "40" },
    { "txpower",      "50" },
    /* Link — WFB-NG (air) */
    { "power",        "3" },
    { "air_channel",  "149" },
    { "width",        "40" },
    { "mcs_index",    "5" },
    { "stbc",         "off" },
    { "ldpc",         "on" },
    { "fec_k",        "8" },
    { "fec_n",        "12" },
    { "mlink",        "on" },
    /* Link — Adaptive */
    { "adaptivelink", "on" },
    /* Link — AALink */
    { "power_level_0_to_4",    "3" },
    { "fallback_ms",           "1500" },
    { "hold_fallback_mode_s",  "5" },
    { "min_between_changes_ms","800" },
    { "hold_modes_down_s",     "10" },
    { "hysteresis_percent",    "20" },
    { "allow_request_keyframe","on" },
    { "idr_every_change",      "off" },
    /* Link — AP-FPV */
    { "channel",      "6" },

    /* Display */
    { "hdmi_mode",        "1920x1080@60" },
    { "video_scale",      "100" },
    { "color_correction", "off" },
    { "cc_gain",          "25" },
    { "cc_offset",        "0" },

    /* DVR */
    { "rec_enabled",          "on" },
    { "dvr_mode",             "reencode" },
    { "rec_fps",              "60" },
    { "dvr_max_size",         "4000" },
    { "dvr_reenc_codec",      "h265" },
    { "dvr_reenc_resolution", "1920x1080" },
    { "dvr_reenc_fps",        "60" },
    { "dvr_reenc_bitrate",    "8000" },
    { "dvr_osd",              "on" },

    /* System — Receiver */
    { "rx_codec",     "h265" },
    { "rx_mode",      "wfb" },

    /* System — Network */
    { "hotspot",      "off" },
    { "restream",     "off" },

    /* System — Telemetry */
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

static void dummy_set_async(const char *d, const char *p, const char *k,
                            const char *v, pp_settings_done_cb on_done,
                            void *user_data) {
    const char *fail = getenv("PP_SIM_FAIL");
    if (fail && *fail) {
        /* Don't apply the value — simulate a backend failure. */
        if (on_done) on_done(1, "simulated failure (PP_SIM_FAIL set)", user_data);
        return;
    }
    dummy_set(d, p, k, v);
    if (on_done) on_done(0, NULL, user_data);
}

static const pp_settings_provider_t g_dummy = {
    .set       = dummy_set,
    .get       = dummy_get,
    .set_async = dummy_set_async,
};

void pp_settings_register_dummy(void) {
    pp_settings_register(&g_dummy);
}
