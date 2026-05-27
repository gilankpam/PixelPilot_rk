/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"
#include "settings_fpvd_internal.h"

#include <string.h>

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

void pp_settings_register_fpvd(void) {
    /* Filled in by Task 3.6. */
}
