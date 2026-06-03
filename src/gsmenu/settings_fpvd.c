/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"
#include "settings_fpvd_internal.h"
#include "settings_gs_rxpower.h"

#include <string.h>
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "lvgl/lvgl.h"   /* for lv_async_call, lv_malloc, lv_free */

#define FPVD_DEFAULT_URL "http://127.0.0.1:8080"
#define FPVD_QUEUE_CAP   32

typedef struct fpvd_job {
    char     path[128];           /* dotted json path */
    char     value[128];          /* UI value string */
    fpvd_type_t type;
    fpvd_endpoint_t endpoint;     /* AIR, LINK or CONFIG */
    char     apply_to[8];         /* "both"|"gs" for LINK; "" for AIR */
    bool     apply_only;          /* true → skip PATCH, just POST /apply */
    pp_settings_done_cb on_done;
    void    *user_data;
} fpvd_job_t;

typedef struct {
    char     base_url[128];
    pthread_t worker;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool     stop;
    bool     visible;
    bool     connected;
    bool     worker_started;

    cJSON   *air_snapshot;        /* GET /air/config (drone), protected by mu */
    cJSON   *gs_snapshot;         /* GET /link wrapped {"link":...}, by mu */
    cJSON   *config_snapshot;     /* GET /config?pending=true (GS), by mu */
    bool     config_dirty;        /* staged-but-unapplied /config changes */

    fpvd_job_t queue[FPVD_QUEUE_CAP];
    size_t     queue_n;

    pp_settings_snapshot_cb listener_cb;
    void                   *listener_ud;
} fpvd_state_t;

static fpvd_state_t G;

static const fpvd_keymap_entry_t KEYMAP[] = {
    /* Camera — Video */
    { "air", "camera", "size",       "video.resolution",  FPVD_T_STRING,         FPVD_EP_AIR, NULL },
    { "air", "camera", "fps",        "video.fps",         FPVD_T_INT,            FPVD_EP_AIR, NULL },
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_BITRATE_KBPS,   FPVD_EP_AIR, NULL },
    { "air", "camera", "codec",      "video.codec",       FPVD_T_ENUM,           FPVD_EP_AIR, NULL },
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT,          FPVD_EP_AIR, NULL },
    { "air", "camera", "rc_mode",    "video.rcMode",      FPVD_T_ENUM,           FPVD_EP_AIR, NULL },
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — ROI */
    { "air", "camera", "roi_enabled","video.roi.enabled", FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_qp",     "video.roi.qp",      FPVD_T_INT,            FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_center", "video.roi.center",  FPVD_T_PERCENT_TO_FRAC,FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_steps",  "video.roi.steps",   FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — Image */
    { "air", "camera", "mirror",     "image.mirror",      FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "flip",       "image.flip",        FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "rotate",     "image.rotate",      FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — Recording */
    { "air", "camera", "rec_enable", "recording.enabled",    FPVD_T_BOOL,            FPVD_EP_AIR, NULL },
    { "air", "camera", "rec_split",  "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN,FPVD_EP_AIR, NULL },
    { "air", "camera", "rec_maxmb",  "recording.maxMB",      FPVD_T_INT,             FPVD_EP_AIR, NULL },

    /* Link — shared radio (GS-local-first, pushed to drone server-side) */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT,     FPVD_EP_LINK, "both" },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT,     FPVD_EP_LINK, "both" },

    /* Link — GS card power (percent slider → GS link.txpower mBm, GS-only) */
    { "gs",  "link",  "rx_power",   "link.txpower",  FPVD_T_RXPOWER, FPVD_EP_LINK, "gs" },

    /* Link — drone TX power + modulation (drone-owned) */
    { "gs",  "wfbng", "txpower",    "link.txpower",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "mcs_index",  "link.mcs",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "stbc",       "link.stbc",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "wfbng", "ldpc",       "link.ldpc",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "wfbng", "fec_k",      "link.fec.k",    FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "fec_n",      "link.fec.n",    FPVD_T_INT,  FPVD_EP_AIR, NULL },

    /* Dynamic Link */
    { "air", "dlink", "enabled",              "dynamicLink.enabled",              FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "interleaving",         "dynamicLink.interleavingSupported",FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "mavlink_enable",       "dynamicLink.mavlinkEnable",        FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "osd_enabled",          "dynamicLink.osd.enabled",          FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "osd_debug_latency",    "dynamicLink.osd.debugLatency",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "health_timeout_ms",    "dynamicLink.healthTimeoutMs",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "min_idr_interval_ms",  "dynamicLink.minIdrIntervalMs",     FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "apply_stagger_ms",     "dynamicLink.applyStaggerMs",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "apply_subpace_ms",     "dynamicLink.applySubPaceMs",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_threshold_kbps", "dynamicLink.roiQp.thresholdKbps",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_low_anchor_kbps","dynamicLink.roiQp.lowAnchorKbps",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_floor",          "dynamicLink.roiQp.floor",          FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_step",           "dynamicLink.roiQp.step",           FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_mcs",             "dynamicLink.safe.mcs",             FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_k",               "dynamicLink.safe.k",               FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_n",               "dynamicLink.safe.n",               FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_depth",           "dynamicLink.safe.depth",           FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_bandwidth",       "dynamicLink.safe.bandwidth",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_txpower_dbm",     "dynamicLink.safe.txPowerDbm",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_bitrate_kbps",    "dynamicLink.safe.bitrateKbps",     FPVD_T_INT,  FPVD_EP_AIR, NULL },

    /* PixelPilot launch config → fpvd /config (pixelpilot.*); staged, applied on demand */
    { "gs",  "display", "screen_mode",      "pixelpilot.screenMode",          FPVD_T_STRING,          FPVD_EP_CONFIG, NULL },
    { "gs",  "display", "video_scale",      "pixelpilot.videoScale",          FPVD_T_PERCENT_TO_FRAC, FPVD_EP_CONFIG, NULL },
    { "gs",  "display", "rtp_jitter_ms",    "pixelpilot.rtpJitterMs",         FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_mode",         "pixelpilot.dvr.mode",            FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "rec_fps",          "pixelpilot.dvr.framerate",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_max_size",     "pixelpilot.dvr.maxSizeMb",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_reenc_codec",  "pixelpilot.dvr.reencCodec",      FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_reenc_resolution", "pixelpilot.dvr.reencResolution", FPVD_T_ENUM,        FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_reenc_fps",    "pixelpilot.dvr.reencFps",        FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_reenc_bitrate","pixelpilot.dvr.reencBitrate",    FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs",  "dvr",     "dvr_osd",          "pixelpilot.dvr.osd",             FPVD_T_BOOL,            FPVD_EP_CONFIG, NULL },
};

static const size_t KEYMAP_N = sizeof(KEYMAP) / sizeof(KEYMAP[0]);

const fpvd_keymap_entry_t *fpvd_keymap_lookup(const char *d, const char *p, const char *k) {
    /* NULL-safe: pp_row_text() reads rows with NULL domain/page (not yet wired),
     * relying on the provider returning "unknown" rather than dereferencing. */
    if (!d || !p || !k) return NULL;
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

const char *fpvd_write_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link";
    case FPVD_EP_CONFIG: return "/config";
    default:             return "/air/config";
    }
}
const char *fpvd_apply_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link/apply";
    case FPVD_EP_CONFIG: return "/apply";
    default:             return "/air/apply";
    }
}
const char *fpvd_read_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link";
    case FPVD_EP_CONFIG: return "/config";
    default:             return "/air/config";
    }
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
    case FPVD_T_RXPOWER:
        /* Read-back treated as raw int (mBm) for now; rendered as percent by caller. */
        if (cJSON_IsNumber(node)) {
            snprintf(buf, sizeof buf, "%d", (int)node->valuedouble);
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
    case FPVD_T_RXPOWER: {
        /* Unreachable in normal flow: run_job converts rx-power percent->mBm
         * and builds the body as FPVD_T_INT. Kept as a defensive int passthrough. */
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)v);
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

static const char *LOCKED_PATHS[] = {
    "link.mcs",
    "link.txpower",
    "link.fec",
    "link.width",
    "video.bitrate",
    "video.qpDelta",
    "video.roi",
};
static const size_t LOCKED_PATHS_N = sizeof(LOCKED_PATHS) / sizeof(LOCKED_PATHS[0]);

bool fpvd_is_locked_path(const char *path) {
    for (size_t i = 0; i < LOCKED_PATHS_N; i++) {
        size_t lp_len = strlen(LOCKED_PATHS[i]);
        if (strncmp(path, LOCKED_PATHS[i], lp_len) != 0) continue;
        /* Match either exact or extended by a '.' (subtree). */
        if (path[lp_len] == '\0' || path[lp_len] == '.') return true;
    }
    return false;
}

static size_t curl_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    fpvd_http_result_t *r = (fpvd_http_result_t *)ud;
    size_t add = sz * nm;
    char *nb = realloc(r->body, r->body_len + add + 1);
    if (!nb) return 0;
    r->body = nb;
    memcpy(r->body + r->body_len, ptr, add);
    r->body_len += add;
    r->body[r->body_len] = '\0';
    return add;
}

static void fpvd_curl_init_once(void) {
    static int done = 0;
    if (!done) { curl_global_init(CURL_GLOBAL_DEFAULT); done = 1; }
}

static fpvd_http_result_t http_do(const char *url, const char *method,
                                  const char *body) {
    fpvd_curl_init_once();
    fpvd_http_result_t r = { 0, NULL, 0 };
    CURL *c = curl_easy_init();
    if (!c) return r;
    struct curl_slist *hdrs = NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    if (strcmp(method, "GET") == 0) {
        /* default */
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        if (!body) curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    } else {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        r.status = (int)code;
    }
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

fpvd_http_result_t fpvd_http_get(const char *url)  { return http_do(url, "GET",  NULL); }
fpvd_http_result_t fpvd_http_post(const char *url) { return http_do(url, "POST", NULL); }
fpvd_http_result_t fpvd_http_patch_json(const char *url, const char *body) {
    return http_do(url, "PATCH", body);
}
fpvd_http_result_t fpvd_http_post_json(const char *url, const char *body) {
    return http_do(url, "POST", body);
}

void fpvd_http_result_free(fpvd_http_result_t *r) {
    if (r && r->body) { free(r->body); r->body = NULL; r->body_len = 0; }
}

/* -------------------------------------------------------------------------
 * Callback-dispatch helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    pp_settings_done_cb cb;
    void *user_data;
    int   rc;
    char  err[128];
} fpvd_done_dispatch_t;

static void done_dispatch_async(void *ptr) {
    fpvd_done_dispatch_t *d = (fpvd_done_dispatch_t *)ptr;
    if (d->cb) d->cb(d->rc, d->err[0] ? d->err : NULL, d->user_data);
    lv_free(d);
}

static void schedule_done(pp_settings_done_cb cb, void *ud,
                          int rc, const char *err) {
    if (!cb) return;
    lv_lock();
    fpvd_done_dispatch_t *d = lv_malloc(sizeof *d);
    if (!d) { lv_unlock(); return; }
    d->cb = cb;
    d->user_data = ud;
    d->rc = rc;
    if (err) { strncpy(d->err, err, sizeof d->err - 1); d->err[sizeof d->err - 1] = '\0'; }
    else d->err[0] = '\0';
    lv_async_call(done_dispatch_async, d);
    lv_unlock();
}

static void listener_dispatch_async(void *ptr) {
    (void)ptr;
    pthread_mutex_lock(&G.mu);
    pp_settings_snapshot_cb cb = G.listener_cb;
    void *ud = G.listener_ud;
    pthread_mutex_unlock(&G.mu);
    if (cb) cb(ud);
}

static void notify_listener(void) {
    lv_lock();
    lv_async_call(listener_dispatch_async, NULL);
    lv_unlock();
}

/* -------------------------------------------------------------------------
 * URL helper + snapshot refresh
 * ---------------------------------------------------------------------- */

static char *url_join(const char *base, const char *path) {
    size_t n = strlen(base) + strlen(path) + 1;
    char *u = malloc(n);
    if (!u) return NULL;
    snprintf(u, n, "%s%s", base, path);
    return u;
}

/* Called with G.mu HELD. Releases and re-acquires mutex around the HTTP calls.
 * Reachability tracks the GS daemon's own /link; /air/config is proxied to the
 * drone and may 502 while the GS is up — that leaves air_snapshot stale, not
 * a disconnect. */
static void refresh_snapshot_unlocked(void) {
    char *link_url   = url_join(G.base_url, "/link");
    char *air_url    = url_join(G.base_url, "/air/config");
    char *config_url = url_join(G.base_url, "/config?pending=true");
    if (!link_url || !air_url || !config_url) {
        free(link_url); free(air_url); free(config_url);
        G.connected = false; return;
    }
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t lr = fpvd_http_get(link_url);
    fpvd_http_result_t ar = fpvd_http_get(air_url);
    fpvd_http_result_t cr = fpvd_http_get(config_url);
    pthread_mutex_lock(&G.mu);
    free(link_url); free(air_url); free(config_url);

    bool was_connected = G.connected;
    if (lr.status == 200 && lr.body) {
        cJSON *flat = cJSON_Parse(lr.body);
        if (flat) {
            cJSON *wrap = cJSON_CreateObject();
            if (wrap) {
                cJSON_AddItemToObject(wrap, "link", flat);   /* wrap so paths resolve */
                if (G.gs_snapshot) cJSON_Delete(G.gs_snapshot);
                G.gs_snapshot = wrap;
                G.connected = true;
            } else {
                cJSON_Delete(flat);   /* OOM: don't orphan the parsed body */
            }
        }
    } else {
        G.connected = false;
    }
    if (ar.status == 200 && ar.body) {
        cJSON *a = cJSON_Parse(ar.body);
        if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
    }
    if (cr.status == 200 && cr.body) {
        cJSON *c = cJSON_Parse(cr.body);
        if (c) { if (G.config_snapshot) cJSON_Delete(G.config_snapshot); G.config_snapshot = c; }
    }
    fpvd_http_result_free(&lr);
    fpvd_http_result_free(&ar);
    fpvd_http_result_free(&cr);
    if (was_connected != G.connected) notify_listener();
    else if (G.connected) notify_listener();
}

/* -------------------------------------------------------------------------
 * Error parsing + job runner
 * ---------------------------------------------------------------------- */

static const char *parse_error_message(const char *body) {
    if (!body) return NULL;
    cJSON *r = cJSON_Parse(body);
    if (!r) return NULL;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(r, "error");
    const char *code = (err && cJSON_IsString(err)) ? err->valuestring : "";
    static char buf[160];
    buf[0] = '\0';
    if (strcmp(code, "dynamic_link_locked") == 0) {
        snprintf(buf, sizeof buf, "Locked by Dynamic Link");
    } else if (strcmp(code, "validation") == 0) {
        cJSON *det = cJSON_GetObjectItemCaseSensitive(r, "details");
        if (cJSON_IsArray(det) && cJSON_GetArraySize(det) > 0) {
            cJSON *first = cJSON_GetArrayItem(det, 0);
            cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
            if (msg && cJSON_IsString(msg)) {
                snprintf(buf, sizeof buf, "%s", msg->valuestring);
            } else {
                snprintf(buf, sizeof buf, "Validation failed");
            }
        } else {
            snprintf(buf, sizeof buf, "Validation failed");
        }
    } else {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "message");
        snprintf(buf, sizeof buf, "%s",
            (m && cJSON_IsString(m)) ? m->valuestring : "Request rejected");
    }
    cJSON_Delete(r);
    return buf[0] ? buf : NULL;
}

/* Mutex must be RELEASED on entry. */
static void run_job_unlocked(fpvd_job_t job) {
    char *patch_url = NULL, *apply_url = NULL, *body_s = NULL;
    int rc = 0;
    char err[160] = {0};

    /* PATCH phase — skipped entirely for apply-only jobs (explicit Apply). */
    if (!job.apply_only) {
        /* Build the PATCH body. rx-power converts percent -> driver mBm first. */
        cJSON *body = NULL;
        if (job.type == FPVD_T_RXPOWER) {
            pp_nic_driver_t drv = pp_rxpower_primary_driver();
            int mbm = 0;
            if (!pp_rxpower_pct_to_driver_value(drv, atoi(job.value), &mbm)) {
                schedule_done(job.on_done, job.user_data, -1, "No supported NIC driver");
                return;
            }
            char mbm_s[16]; snprintf(mbm_s, sizeof mbm_s, "%d", mbm);
            body = fpvd_build_patch_body(job.path, mbm_s, FPVD_T_INT);
        } else {
            body = fpvd_build_patch_body(job.path, job.value, job.type);
        }
        body_s = body ? cJSON_PrintUnformatted(body) : NULL;
        if (body) cJSON_Delete(body);

        patch_url = url_join(G.base_url, fpvd_write_path(job.endpoint));
        if (!patch_url) {
            if (body_s) free(body_s);
            schedule_done(job.on_done, job.user_data, -1, "Out of memory");
            return;
        }

        fpvd_http_result_t r = fpvd_http_patch_json(patch_url, body_s ? body_s : "{}");
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "GS unreachable");
            pthread_mutex_lock(&G.mu);
            bool was = G.connected; G.connected = false;
            pthread_mutex_unlock(&G.mu);
            if (was) notify_listener();
        } else if (r.status >= 400) {
            rc = -1;
            const char *m = parse_error_message(r.body);
            snprintf(err, sizeof err, "%s", m ? m : "Request rejected");
        }
        fpvd_http_result_free(&r);

        /* EP_CONFIG rows stage only: a successful PATCH marks pending dirty and
         * stops here (no apply). AIR/LINK fall through to apply immediately. */
        if (rc == 0 && job.endpoint == FPVD_EP_CONFIG) {
            pthread_mutex_lock(&G.mu);
            G.config_dirty = true;
            refresh_snapshot_unlocked();
            pthread_mutex_unlock(&G.mu);
            schedule_done(job.on_done, job.user_data, 0, NULL);
            free(patch_url); if (body_s) free(body_s);
            return;
        }
    }

    /* Apply phase — AIR/LINK after PATCH, or apply-only CONFIG (explicit Apply). */
    if (rc == 0) {
        apply_url = url_join(G.base_url, fpvd_apply_path(job.endpoint));
        if (!apply_url) {
            free(patch_url); if (body_s) free(body_s);
            schedule_done(job.on_done, job.user_data, -1, "Out of memory");
            return;
        }
        fpvd_http_result_t r;
        if (job.endpoint == FPVD_EP_LINK) {
            char apply_body[40];
            snprintf(apply_body, sizeof apply_body, "{\"applyTo\":\"%s\"}",
                     job.apply_to[0] ? job.apply_to : "both");
            r = fpvd_http_post_json(apply_url, apply_body);
        } else {
            r = fpvd_http_post(apply_url);
        }
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "GS unreachable");
            pthread_mutex_lock(&G.mu);
            bool was = G.connected; G.connected = false;
            pthread_mutex_unlock(&G.mu);
            if (was) notify_listener();
        } else if (r.status >= 400) {
            rc = -1;
            const char *m = parse_error_message(r.body);
            snprintf(err, sizeof err, "%s", m ? m : "Apply failed");
        }
        fpvd_http_result_free(&r);
        if (rc == 0 && job.apply_only) {
            pthread_mutex_lock(&G.mu);
            G.config_dirty = false;
            pthread_mutex_unlock(&G.mu);
        }
    }

    if (rc == 0) {
        pthread_mutex_lock(&G.mu);
        refresh_snapshot_unlocked();
        pthread_mutex_unlock(&G.mu);
    }

    schedule_done(job.on_done, job.user_data, rc, err[0] ? err : NULL);
    free(patch_url); free(apply_url); if (body_s) free(body_s);
}

/* -------------------------------------------------------------------------
 * Worker thread main loop
 * ---------------------------------------------------------------------- */

static void *worker_main(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&G.mu);
        while (!G.stop && G.queue_n == 0) {
            int wait_ms = G.connected ? (G.visible ? 3000 : 60000) : 2000;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int wr = pthread_cond_timedwait(&G.cv, &G.mu, &ts);
            if (wr == ETIMEDOUT) {
                refresh_snapshot_unlocked();
            }
        }
        if (G.stop) { pthread_mutex_unlock(&G.mu); break; }

        /* Pull the head job. */
        fpvd_job_t job = G.queue[0];
        for (size_t i = 1; i < G.queue_n; i++) G.queue[i-1] = G.queue[i];
        G.queue_n--;
        pthread_mutex_unlock(&G.mu);

        run_job_unlocked(job);

        struct timespec ts2 = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts2, NULL);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Enqueue + provider methods
 * ---------------------------------------------------------------------- */

static void enqueue_locked(const fpvd_keymap_entry_t *e, const char *value,
                           pp_settings_done_cb cb, void *ud) {
    for (size_t i = 0; i < G.queue_n; i++) {
        /* Two rows share path "link.txpower" on different endpoints (drone TX
         * power vs GS card power) — coalesce only within the same endpoint. */
        if (strcmp(G.queue[i].path, e->path) == 0 &&
            G.queue[i].endpoint == e->endpoint) {
            strncpy(G.queue[i].value, value, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            G.queue[i].on_done = cb;
            G.queue[i].user_data = ud;
            return;
        }
    }
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    strncpy(j->path,  e->path, sizeof j->path  - 1); j->path [sizeof j->path -1] = '\0';
    strncpy(j->value, value,   sizeof j->value - 1); j->value[sizeof j->value-1] = '\0';
    j->type       = e->type;
    j->endpoint   = e->endpoint;
    j->apply_only = false;   /* slot may be reused from a prior apply-only job */
    if (e->apply_to) { strncpy(j->apply_to, e->apply_to, sizeof j->apply_to - 1); j->apply_to[sizeof j->apply_to - 1] = '\0'; }
    else j->apply_to[0] = '\0';
    j->on_done   = cb;
    j->user_data = ud;
}

static char *prov_get(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return strdup("");
    pthread_mutex_lock(&G.mu);
    cJSON *snap;
    switch (e->endpoint) {
    case FPVD_EP_LINK:   snap = G.gs_snapshot;     break;
    case FPVD_EP_CONFIG: snap = G.config_snapshot; break;
    default:             snap = G.air_snapshot;    break;
    }
    char *out;
    if (e->type == FPVD_T_RXPOWER) {
        char *raw = snap ? fpvd_snapshot_read_string(snap, e->path, FPVD_T_INT) : strdup("");
        out = strdup("");
        if (raw && raw[0]) {
            int pct = 0;
            if (pp_rxpower_driver_value_to_pct(pp_rxpower_primary_driver(), atoi(raw), &pct)) {
                char buf[16]; snprintf(buf, sizeof buf, "%d", pct);
                free(out); out = strdup(buf);
            }
        }
        free(raw);
    } else {
        out = snap ? fpvd_snapshot_read_string(snap, e->path, e->type) : strdup("");
    }
    pthread_mutex_unlock(&G.mu);
    return out;
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) { schedule_done(cb, ud, -1, "Unknown setting"); return; }
    /* Dynamic-link lock only governs drone-owned (AIR) fields. */
    if (e->endpoint == FPVD_EP_AIR && fpvd_is_locked_path(e->path)) {
        pthread_mutex_lock(&G.mu);
        cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
        cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
        bool dlink_on = en && cJSON_IsTrue(en);
        pthread_mutex_unlock(&G.mu);
        if (dlink_on) { schedule_done(cb, ud, -1, "Locked by Dynamic Link"); return; }
    }
    pthread_mutex_lock(&G.mu);
    enqueue_locked(e, v, cb, ud);
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

/* Fire-and-forget sync set — fall through to async. */
static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    prov_set_async(d, p, k, v, NULL, NULL);
}

/* Commit staged /config changes: enqueue an apply-only job (POST /apply). */
static void prov_apply(pp_settings_done_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        pthread_mutex_unlock(&G.mu);
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    memset(j, 0, sizeof *j);
    j->endpoint   = FPVD_EP_CONFIG;
    j->apply_only = true;
    j->on_done    = cb;
    j->user_data  = ud;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static bool prov_is_available(const char *d, const char *p, const char *k) {
    return fpvd_keymap_lookup(d, p, k) != NULL;
}

static bool prov_has_pending(void) {
    pthread_mutex_lock(&G.mu);
    bool dirty = G.config_dirty;
    pthread_mutex_unlock(&G.mu);
    return dirty;
}

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    if (e->endpoint != FPVD_EP_AIR) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on = en && cJSON_IsTrue(en);
    pthread_mutex_unlock(&G.mu);
    return dl_on;
}

static bool prov_is_connected(void) {
    pthread_mutex_lock(&G.mu);
    bool c = G.connected;
    pthread_mutex_unlock(&G.mu);
    return c;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    G.listener_cb = cb;
    G.listener_ud = ud;
    pthread_mutex_unlock(&G.mu);
}

static void prov_set_visibility(bool v) {
    pthread_mutex_lock(&G.mu);
    G.visible = v;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

/* -------------------------------------------------------------------------
 * Provider vtable + registration
 * ---------------------------------------------------------------------- */

static const pp_settings_provider_t G_PROVIDER = {
    .set                    = prov_set,
    .get                    = prov_get,
    .set_async              = prov_set_async,
    .is_locked              = prov_is_locked,
    .is_connected           = prov_is_connected,
    .set_snapshot_listener  = prov_set_snapshot_listener,
    .set_visibility         = prov_set_visibility,
    .is_available           = prov_is_available,
    .apply                  = prov_apply,
    .has_pending            = prov_has_pending,
};

void pp_settings_register_fpvd(void) {
    /* lv_init is idempotent; calling it ensures the LVGL machinery
     * (used by lv_async_call/lv_malloc) is available even if our
     * caller initializes LVGL later. */
    static int lv_initted = 0;
    if (!lv_initted) { lv_init(); lv_initted = 1; }

    fpvd_curl_init_once();
    const char *u = getenv("PP_FPVD_URL");

    if (!G.worker_started) {
        pthread_mutex_init(&G.mu, NULL);
        pthread_cond_init(&G.cv, NULL);
    }
    pthread_mutex_lock(&G.mu);
    /* Write base_url under the lock — the worker reads it in refresh. */
    strncpy(G.base_url, u && *u ? u : FPVD_DEFAULT_URL, sizeof G.base_url - 1);
    G.base_url[sizeof G.base_url - 1] = '\0';
    G.stop      = false;
    G.visible   = false;
    G.connected = false;
    if (G.air_snapshot)    { cJSON_Delete(G.air_snapshot);    G.air_snapshot    = NULL; }
    if (G.gs_snapshot)     { cJSON_Delete(G.gs_snapshot);     G.gs_snapshot     = NULL; }
    if (G.config_snapshot) { cJSON_Delete(G.config_snapshot); G.config_snapshot = NULL; }
    G.config_dirty = false;
    G.queue_n   = 0;
    G.listener_cb = NULL;
    G.listener_ud = NULL;
    pthread_mutex_unlock(&G.mu);
    pthread_cond_signal(&G.cv);

    /* Prime snapshot synchronously (best-effort). */
    pthread_mutex_lock(&G.mu);
    refresh_snapshot_unlocked();
    pthread_mutex_unlock(&G.mu);

    if (!G.worker_started) {
        pthread_create(&G.worker, NULL, worker_main, NULL);
        G.worker_started = true;
    }
    pp_settings_register(&G_PROVIDER);
}
