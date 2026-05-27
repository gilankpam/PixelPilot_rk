/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"
#include "settings_fpvd_internal.h"

#include <string.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>

static const fpvd_keymap_entry_t KEYMAP[] = {
    /* Camera — Video */
    { "air", "camera", "size",       "video.resolution",  FPVD_T_STRING },
    { "air", "camera", "fps",        "video.fps",         FPVD_T_INT    },
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_BITRATE_KBPS },
    { "air", "camera", "codec",      "video.codec",       FPVD_T_ENUM   },
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT  },
    { "air", "camera", "rc_mode",    "video.rcMode",      FPVD_T_ENUM   },
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT    },

    /* Camera — ROI */
    { "air", "camera", "roi_enabled","video.roi.enabled", FPVD_T_BOOL   },
    { "air", "camera", "roi_qp",     "video.roi.qp",      FPVD_T_INT    },
    { "air", "camera", "roi_center", "video.roi.center",  FPVD_T_PERCENT_TO_FRAC },
    { "air", "camera", "roi_steps",  "video.roi.steps",   FPVD_T_INT    },

    /* Camera — Image */
    { "air", "camera", "mirror",     "image.mirror",      FPVD_T_BOOL   },
    { "air", "camera", "flip",       "image.flip",        FPVD_T_BOOL   },
    { "air", "camera", "rotate",     "image.rotate",      FPVD_T_INT    },

    /* Camera — Recording */
    { "air", "camera", "rec_enable", "recording.enabled",    FPVD_T_BOOL },
    { "air", "camera", "rec_split",  "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN },
    { "air", "camera", "rec_maxmb",  "recording.maxMB",      FPVD_T_INT  },

    /* Link — WFB-NG */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT },
    { "gs",  "wfbng", "txpower",    "link.txpower",  FPVD_T_INT },
    { "air", "wfbng", "mcs_index",  "link.mcs",      FPVD_T_INT },
    { "air", "wfbng", "stbc",       "link.stbc",     FPVD_T_BOOL },
    { "air", "wfbng", "ldpc",       "link.ldpc",     FPVD_T_BOOL },
    { "air", "wfbng", "fec_k",      "link.fec.k",    FPVD_T_INT },
    { "air", "wfbng", "fec_n",      "link.fec.n",    FPVD_T_INT },

    /* Dynamic Link */
    { "air", "dlink", "enabled",              "dynamicLink.enabled",              FPVD_T_BOOL },
    { "air", "dlink", "interleaving",         "dynamicLink.interleavingSupported",FPVD_T_BOOL },
    { "air", "dlink", "mavlink_enable",       "dynamicLink.mavlinkEnable",        FPVD_T_BOOL },
    { "air", "dlink", "osd_enabled",          "dynamicLink.osd.enabled",          FPVD_T_BOOL },
    { "air", "dlink", "osd_debug_latency",    "dynamicLink.osd.debugLatency",     FPVD_T_BOOL },
    { "air", "dlink", "health_timeout_ms",    "dynamicLink.healthTimeoutMs",      FPVD_T_INT },
    { "air", "dlink", "min_idr_interval_ms",  "dynamicLink.minIdrIntervalMs",     FPVD_T_INT },
    { "air", "dlink", "apply_stagger_ms",     "dynamicLink.applyStaggerMs",       FPVD_T_INT },
    { "air", "dlink", "apply_subpace_ms",     "dynamicLink.applySubPaceMs",       FPVD_T_INT },
    { "air", "dlink", "roiqp_threshold_kbps", "dynamicLink.roiQp.thresholdKbps",  FPVD_T_INT },
    { "air", "dlink", "roiqp_low_anchor_kbps","dynamicLink.roiQp.lowAnchorKbps",  FPVD_T_INT },
    { "air", "dlink", "roiqp_floor",          "dynamicLink.roiQp.floor",          FPVD_T_INT },
    { "air", "dlink", "roiqp_step",           "dynamicLink.roiQp.step",           FPVD_T_INT },
    { "air", "dlink", "safe_mcs",             "dynamicLink.safe.mcs",             FPVD_T_INT },
    { "air", "dlink", "safe_k",               "dynamicLink.safe.k",               FPVD_T_INT },
    { "air", "dlink", "safe_n",               "dynamicLink.safe.n",               FPVD_T_INT },
    { "air", "dlink", "safe_depth",           "dynamicLink.safe.depth",           FPVD_T_INT },
    { "air", "dlink", "safe_bandwidth",       "dynamicLink.safe.bandwidth",       FPVD_T_INT },
    { "air", "dlink", "safe_txpower_dbm",     "dynamicLink.safe.txPowerDbm",      FPVD_T_INT },
    { "air", "dlink", "safe_bitrate_kbps",    "dynamicLink.safe.bitrateKbps",     FPVD_T_INT },
};

static const size_t KEYMAP_N = sizeof(KEYMAP) / sizeof(KEYMAP[0]);

const fpvd_keymap_entry_t *fpvd_keymap_lookup(const char *d, const char *p, const char *k) {
    for (size_t i = 0; i < KEYMAP_N; i++) {
        if (strcmp(KEYMAP[i].domain, d) == 0 &&
            strcmp(KEYMAP[i].page,   p) == 0 &&
            strcmp(KEYMAP[i].key,    k) == 0) {
            return &KEYMAP[i];
        }
    }
    return NULL;
}

const fpvd_keymap_entry_t *fpvd_keymap_at(size_t i) {
    if (i >= KEYMAP_N) return NULL;
    return &KEYMAP[i];
}

static cJSON *walk_path(cJSON *root, const char *path) {
    cJSON *node = root;
    const char *p = path;
    char seg[64];
    while (*p && node) {
        size_t i = 0;
        while (*p && *p != '.' && i + 1 < sizeof seg) { seg[i++] = *p++; }
        seg[i] = '\0';
        if (*p == '.') p++;
        node = cJSON_GetObjectItemCaseSensitive(node, seg);
    }
    return node;
}

char *fpvd_snapshot_read_string(cJSON *root, const char *path, fpvd_type_t type) {
    cJSON *node = walk_path(root, path);
    if (!node) return strdup("");
    char buf[64];
    switch (type) {
    case FPVD_T_INT:
        if (cJSON_IsNumber(node)) {
            snprintf(buf, sizeof buf, "%d", (int)node->valuedouble);
            return strdup(buf);
        }
        break;
    case FPVD_T_FLOAT:
        if (cJSON_IsNumber(node)) {
            double v = node->valuedouble;
            if (fabs(v - (int)v) < 1e-6) snprintf(buf, sizeof buf, "%d", (int)v);
            else snprintf(buf, sizeof buf, "%.3g", v);
            return strdup(buf);
        }
        break;
    case FPVD_T_BOOL:
        if (cJSON_IsBool(node))
            return strdup(cJSON_IsTrue(node) ? "on" : "off");
        break;
    case FPVD_T_STRING:
    case FPVD_T_ENUM:
        if (cJSON_IsString(node) && node->valuestring)
            return strdup(node->valuestring);
        break;
    case FPVD_T_BITRATE_KBPS:
        if (cJSON_IsNumber(node)) {
            int kbps = (int)node->valuedouble;
            snprintf(buf, sizeof buf, "%dM", kbps / 1000);
            return strdup(buf);
        }
        break;
    case FPVD_T_SECONDS_FROM_MIN:
        if (cJSON_IsNumber(node)) {
            int secs = (int)node->valuedouble;
            snprintf(buf, sizeof buf, "%d", secs / 60);
            return strdup(buf);
        }
        break;
    case FPVD_T_PERCENT_TO_FRAC:
        if (cJSON_IsNumber(node)) {
            int pct = (int)(node->valuedouble * 100.0 + 0.5);
            snprintf(buf, sizeof buf, "%d", pct);
            return strdup(buf);
        }
        break;
    }
    return strdup("");
}

static cJSON *value_to_cjson(const char *value, fpvd_type_t type) {
    switch (type) {
    case FPVD_T_INT: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)v);
    }
    case FPVD_T_FLOAT: {
        char *end;
        double v = strtod(value, &end);
        if (end == value) return NULL;
        return cJSON_CreateNumber(v);
    }
    case FPVD_T_BOOL:
        if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0)
            return cJSON_CreateBool(1);
        return cJSON_CreateBool(0);
    case FPVD_T_STRING:
    case FPVD_T_ENUM:
        return cJSON_CreateString(value);
    case FPVD_T_BITRATE_KBPS: {
        /* "15M" → 15000; bare "15000" → 15000. */
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        if (*end == 'M' || *end == 'm') v *= 1000;
        return cJSON_CreateNumber((double)v);
    }
    case FPVD_T_SECONDS_FROM_MIN: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)(v * 60));
    }
    case FPVD_T_PERCENT_TO_FRAC: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)v / 100.0);
    }
    }
    return NULL;
}

cJSON *fpvd_build_patch_body(const char *path, const char *value, fpvd_type_t type) {
    cJSON *leaf = value_to_cjson(value, type);
    if (!leaf) return NULL;

    char buf[256];
    strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = '\0';

    /* Split into segments (in-place via strchr/null-terminate). */
    const char *segs[16];
    size_t nsegs = 0;
    char *tok = buf;
    while (tok && *tok && nsegs < 16) {
        segs[nsegs++] = tok;
        char *dot = strchr(tok, '.');
        if (!dot) break;
        *dot = '\0';
        tok = dot + 1;
    }

    cJSON *cur = leaf;
    for (ssize_t i = (ssize_t)nsegs - 1; i >= 0; i--) {
        cJSON *parent = cJSON_CreateObject();
        cJSON_AddItemToObject(parent, segs[i], cur);
        cur = parent;
    }
    return cur;
}

void pp_settings_register_fpvd(void) {
    /* Filled in by Task 3.6. */
}
