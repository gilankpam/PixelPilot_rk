/* fpvd HTTP settings provider — talks to the local fpvd-GS daemon. */
#include "settings.h"
#include "settings_fpvd_internal.h"
#include "settings_runtime_cfg.h"
#include "../conn_state.h"

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
    fpvd_endpoint_t endpoint;     /* AIR or GS */
    fpvd_row_kind_t kind;         /* PLAIN, STAGED, SHARED, BEAMFORM */
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
    bool     refresh_now;         /* hidden→visible: probe now, don't wait a tick */
    bool     gs_connected;        /* fpvd-GS HTTP round-trips succeed */
    bool     worker_started;

    cJSON   *air_snapshot;        /* GET /air/config (drone), protected by mu */
    cJSON   *gs_snapshot;         /* GET /gs/config?pending=true (full GS tree) */
    cJSON   *status_snapshot;     /* GET /gs/status (radio txpower, beamforming localMac) */
    bool     config_dirty;        /* staged-but-unapplied /gs/config changes */

    fpvd_job_t queue[FPVD_QUEUE_CAP];
    size_t     queue_n;

    pp_settings_snapshot_cb listener_cb;
    void                   *listener_ud;
} fpvd_state_t;

static fpvd_state_t G;

static const fpvd_keymap_entry_t KEYMAP[] = {
    /* Camera — Video */
    { "air", "camera", "size",       "video.resolution",  FPVD_T_STRING,         FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "fps",        "video.fps",         FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "codec",      "video.codec",       FPVD_T_ENUM,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT,          FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "resilience", "video.resilience",  FPVD_T_ENUM,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "rc_mode",    "video.rcMode",      FPVD_T_ENUM,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "osd_enabled","osd.enabled",       FPVD_T_BOOL,           FPVD_EP_AIR, FPVD_ROW_PLAIN },

    /* Camera — ROI */
    { "air", "camera", "roi_enabled","video.roi.enabled", FPVD_T_BOOL,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "roi_qp",     "video.roi.qp",      FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "roi_center", "video.roi.center",  FPVD_T_PERCENT_TO_FRAC,FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "roi_steps",  "video.roi.steps",   FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },

    /* Camera — Image */
    { "air", "camera", "mirror",     "image.mirror",      FPVD_T_BOOL,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "flip",       "image.flip",        FPVD_T_BOOL,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "rotate",     "image.rotate",      FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },

    /* Camera — Recording */
    { "air", "camera", "rec_enable", "recording.enabled",    FPVD_T_BOOL,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "rec_split",  "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN,FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "camera", "rec_maxmb",  "recording.maxMB",      FPVD_T_INT,             FPVD_EP_AIR, FPVD_ROW_PLAIN },

    /* Link — shared radio (client-orchestrated: drone first, then GS) */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT, FPVD_EP_GS, FPVD_ROW_SHARED },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT, FPVD_EP_GS, FPVD_ROW_SHARED },

    /* Link — GS card power (dBm) and beamforming (client-owned handshake) */
    { "gs",  "link",  "rx_power",    "link.txPowerDbm",          FPVD_T_INT,  FPVD_EP_GS, FPVD_ROW_PLAIN },
    { "gs",  "link",  "beamforming", "link.beamforming.enabled", FPVD_T_BOOL, FPVD_EP_GS, FPVD_ROW_BEAMFORM },

    /* Link — drone TX power (dBm) + modulation (drone-owned) */
    { "gs",  "wfbng", "txpower",    "link.txPowerDbm", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "mcs_index",  "link.mcs",        FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "stbc",       "link.stbc",       FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "ldpc",       "link.ldpc",       FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_k",      "link.fec.k",      FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_n",      "link.fec.n",      FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_mode",         "link.fec.mode",        FPVD_T_ENUM, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_deadline_ms",  "link.fec.deadlineMs",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_overhead_pct", "link.fec.overheadPct", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },

    /* Dynamic Link */
    { "air", "dlink", "enabled",              "dynamicLink.enabled",              FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_DLINK },
    { "air", "dlink", "compute_base_redundancy",  "dynamicLink.compute.baseRedundancyRatio", FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_blocks_per_frame", "dynamicLink.compute.blocksPerFrame",      FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_min_bitrate_kbps", "dynamicLink.compute.minBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_max_bitrate_kbps", "dynamicLink.compute.maxBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "gs",  "dlink", "max_mcs",                  "dynamicLink.maxMcs",                      FPVD_T_INT,   FPVD_EP_GS,  FPVD_ROW_PLAIN },

    /* PixelPilot launch config → fpvd /gs/config (pixelpilot.*). screen_mode is
     * the only staged row left; it self-applies on change (see prov_set_async). */
    { "gs",  "display", "screen_mode",      "pixelpilot.screenMode",          FPVD_T_STRING,          FPVD_EP_GS, FPVD_ROW_STAGED },
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
    return ep == FPVD_EP_GS ? "/gs/config" : "/air/config";
}
const char *fpvd_apply_path(fpvd_endpoint_t ep) {
    return ep == FPVD_EP_GS ? "/gs/apply" : "/air/apply";
}
const char *fpvd_read_path(fpvd_endpoint_t ep) {
    return ep == FPVD_EP_GS ? "/gs/config?pending=true" : "/air/config";
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

static const char *LOCKED_PATHS[] = {
    "link.mcs",
    "link.txPowerDbm",
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

#define FPVD_GS_APPLY_RETRIES 3

static void step_init(fpvd_step_t *s, const char *method, const char *url,
                      const char *body, int retries, bool gs_side) {
    memset(s, 0, sizeof *s);
    snprintf(s->method, sizeof s->method, "%s", method);
    snprintf(s->url_path, sizeof s->url_path, "%s", url);
    if (body) snprintf(s->body, sizeof s->body, "%s", body);
    s->retries = retries;
    s->gs_side = gs_side;
}

/* Serialize a one-field patch body for `path`=`value` into buf. */
static bool patch_body_str(const char *path, const char *value,
                           fpvd_type_t type, char *buf, size_t n) {
    cJSON *body = fpvd_build_patch_body(path, value, type);
    if (!body) return false;
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!s) return false;
    int w = snprintf(buf, n, "%s", s);
    free(s);
    return w >= 0 && (size_t)w < n;
}

int fpvd_plan_steps(fpvd_row_kind_t kind, fpvd_endpoint_t ep,
                    const char *path, fpvd_type_t type, const char *value,
                    bool drone_reachable, const char *gs_local_mac,
                    fpvd_step_t *out, size_t max,
                    char *err, size_t errn) {
    char body[256];
    if (max < 4) { snprintf(err, errn, "Plan buffer too small"); return -1; }

    switch (kind) {
    case FPVD_ROW_PLAIN: {
        if (ep == FPVD_EP_AIR && !drone_reachable) {
            snprintf(err, errn, "Drone unreachable"); return -1;
        }
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        bool gs = (ep == FPVD_EP_GS);
        step_init(&out[0], "PATCH", fpvd_write_path(ep), body, 0, gs);
        step_init(&out[1], "POST",  fpvd_apply_path(ep), NULL, 0, gs);
        return 2;
    }
    case FPVD_ROW_STAGED: {
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        step_init(&out[0], "PATCH", "/gs/config", body, 0, true);
        return 1;
    }
    case FPVD_ROW_SHARED: {
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        size_t n = 0;
        if (drone_reachable) {
            /* Drone first: the GS then retunes onto the link the drone has
             * already moved to (api.md, client orchestration). */
            step_init(&out[n++], "PATCH", "/air/config", body, 0, false);
            step_init(&out[n++], "POST",  "/air/apply",  NULL, 0, false);
        }
        step_init(&out[n++], "PATCH", "/gs/config", body, FPVD_GS_APPLY_RETRIES, true);
        step_init(&out[n++], "POST",  "/gs/apply",  NULL, FPVD_GS_APPLY_RETRIES, true);
        return (int)n;
    }
    case FPVD_ROW_DLINK: {
        /* Adaptive-link arm/disarm. Drone-first both ways: on enable the GS
         * controller HELLO-handshakes the drone applier, so the applier must
         * come up first; disable mirrors it for symmetry. Same body on both
         * sides ({"dynamicLink":{"enabled":X}}). Unlike SHARED there is no
         * GS-only degradation — without the drone applier there is no link to
         * arm, so the drone must be reachable. */
        if (!drone_reachable) { snprintf(err, errn, "Drone unreachable"); return -1; }
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        step_init(&out[0], "PATCH", "/air/config", body, 0, false);
        step_init(&out[1], "POST",  "/air/apply",  NULL, 0, false);
        step_init(&out[2], "PATCH", "/gs/config", body, FPVD_GS_APPLY_RETRIES, true);
        step_init(&out[3], "POST",  "/gs/apply",  NULL, FPVD_GS_APPLY_RETRIES, true);
        return 4;
    }
    case FPVD_ROW_BEAMFORM: {
        if (!drone_reachable) { snprintf(err, errn, "Drone unreachable"); return -1; }
        bool enable = (strcmp(value, "on") == 0 || strcmp(value, "true") == 0);
        if (enable && (!gs_local_mac || !gs_local_mac[0])) {
            snprintf(err, errn, "GS card MAC unknown"); return -1;
        }
        /* STBC and TX-beamforming are mutually exclusive on the drone:
         * disable stbc when enabling BF, restore it when disabling. */
        if (enable) {
            int w = snprintf(body, sizeof body,
                "{\"link\":{\"beamforming\":{\"enabled\":true,\"remoteMac\":\"%s\"},"
                "\"stbc\":false}}", gs_local_mac);
            if (w < 0 || (size_t)w >= sizeof body) {
                snprintf(err, errn, "Invalid value"); return -1;
            }
        } else {
            snprintf(body, sizeof body,
                "{\"link\":{\"beamforming\":{\"enabled\":false},\"stbc\":true}}");
        }
        step_init(&out[0], "PATCH", "/air/config", body, 0, false);
        step_init(&out[1], "POST",  "/air/apply",  NULL, 0, false);
        snprintf(body, sizeof body,
            "{\"link\":{\"beamforming\":{\"enabled\":%s}}}", enable ? "true" : "false");
        step_init(&out[2], "PATCH", "/gs/config", body, FPVD_GS_APPLY_RETRIES, true);
        step_init(&out[3], "POST",  "/gs/apply",  NULL, FPVD_GS_APPLY_RETRIES, true);
        return 4;
    }
    }
    snprintf(err, errn, "Unknown row kind");
    return -1;
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

#ifndef PP_FPVD_TEST
/* conn_state pushes drone-link transitions; re-lock the menu rows on change. */
static void on_conn_state_change(const conn_state_t *st, void *ud) {
    (void)st; (void)ud;
    notify_listener();
}
#endif

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

/* Called with G.mu HELD. Releases and re-acquires the mutex around HTTP.
 * gs_connected tracks the GS daemon itself; drone reachability is owned by
 * conn_state (not derived here). /air/config is fetched to refresh air_snapshot.
 *
 * NEVER notifies the listener itself: returns true when the caller should
 * call notify_listener() AFTER releasing G.mu (notify_listener takes the
 * LVGL lock; calling it under G.mu inverts the UI thread's lock order). */
static bool refresh_snapshot_unlocked(void) {
    char *gs_url     = url_join(G.base_url, fpvd_read_path(FPVD_EP_GS));
    char *status_url = url_join(G.base_url, "/gs/status");
    char *air_url    = url_join(G.base_url, fpvd_read_path(FPVD_EP_AIR));
    if (!gs_url || !status_url || !air_url) {
        free(gs_url); free(status_url); free(air_url);
        bool was_gs = G.gs_connected;
        G.gs_connected = false;
        return was_gs;   /* notify only on the connected→down flip */
    }
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t gr = fpvd_http_get(gs_url);
    fpvd_http_result_t sr = fpvd_http_get(status_url);
    fpvd_http_result_t ar = fpvd_http_get(air_url);
    pthread_mutex_lock(&G.mu);
    free(gs_url); free(status_url); free(air_url);

    bool was_gs = G.gs_connected;

    if (gr.status == 200 && gr.body) {
        cJSON *g = cJSON_Parse(gr.body);
        if (g) { if (G.gs_snapshot) cJSON_Delete(G.gs_snapshot); G.gs_snapshot = g; }
        G.gs_connected = true;
    } else {
        G.gs_connected = false;
    }
    if (sr.status == 200 && sr.body) {
        cJSON *s = cJSON_Parse(sr.body);
        if (s) {
            if (G.status_snapshot) cJSON_Delete(G.status_snapshot);
            G.status_snapshot = s;
        }
    }
    /* /air/config is fetched only to refresh air_snapshot (the drone's config
     * values shown in the menu). Drone reachability is no longer derived here —
     * it comes from conn_state (fpvd's /gs/status.connection). */
    if (ar.status >= 200 && ar.status < 300 && ar.body) {
        cJSON *a = cJSON_Parse(ar.body);
        if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
    }
    fpvd_http_result_free(&gr);
    fpvd_http_result_free(&sr);
    fpvd_http_result_free(&ar);
    return was_gs != G.gs_connected || G.gs_connected;
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
        /* GS error shape is {"error":"<human text>"} with no message field —
         * surface the error string itself when no message exists. */
        cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "message");
        if (m && cJSON_IsString(m)) snprintf(buf, sizeof buf, "%s", m->valuestring);
        else if (err && cJSON_IsString(err) && err->valuestring[0])
            snprintf(buf, sizeof buf, "%s", err->valuestring);
        else snprintf(buf, sizeof buf, "Request rejected");
    }
    cJSON_Delete(r);
    return buf[0] ? buf : NULL;
}

/* Plan executor. Mutex must be RELEASED on entry. */
static void run_job_unlocked(fpvd_job_t job) {
    int rc = 0;
    char err[160] = {0};
    fpvd_step_t steps[FPVD_PLAN_MAX];
    int nsteps;
    bool air_committed = false, gs_applied = false;

    /* Snapshot base_url under the lock; G.base_url may be rewritten by
     * pp_settings_register_fpvd on another thread. */
    char base[128];
    pthread_mutex_lock(&G.mu);
    snprintf(base, sizeof base, "%s", G.base_url);
    pthread_mutex_unlock(&G.mu);

    if (job.apply_only) {
        step_init(&steps[0], "POST", "/gs/apply", NULL, 0, true);
        nsteps = 1;
    } else {
        bool reachable = (conn_state_get().state == CONN_CONNECTED);
        char mac[24] = {0};
        pthread_mutex_lock(&G.mu);
        cJSON *bf = G.status_snapshot ?
            cJSON_GetObjectItemCaseSensitive(G.status_snapshot, "beamforming") : NULL;
        cJSON *lm = bf ? cJSON_GetObjectItemCaseSensitive(bf, "localMac") : NULL;
        if (lm && cJSON_IsString(lm))
            snprintf(mac, sizeof mac, "%s", lm->valuestring);
        pthread_mutex_unlock(&G.mu);

        nsteps = fpvd_plan_steps(job.kind, job.endpoint, job.path, job.type,
                                 job.value, reachable, mac[0] ? mac : NULL,
                                 steps, FPVD_PLAN_MAX, err, sizeof err);
        if (nsteps < 0) { rc = -1; goto done; }
        if (job.kind == FPVD_ROW_SHARED && !reachable)
            fprintf(stderr, "fpvd: %s applied to GS only (drone unreachable)\n", job.path);
    }

    for (int i = 0; i < nsteps && rc == 0; i++) {
        char *url = url_join(base, steps[i].url_path);
        if (!url) { rc = -1; snprintf(err, sizeof err, "Out of memory"); break; }
        fpvd_http_result_t r = { 0, NULL, 0 };
        for (int a = 0; a <= steps[i].retries; a++) {
            if (a > 0) {
                struct timespec b = { 0, 500 * 1000 * 1000 };
                nanosleep(&b, NULL);
            }
            fpvd_http_result_free(&r);
            if (strcmp(steps[i].method, "PATCH") == 0)
                r = fpvd_http_patch_json(url, steps[i].body);
            else if (steps[i].body[0])
                r = fpvd_http_post_json(url, steps[i].body);
            else
                r = fpvd_http_post(url);
            if (r.status >= 200 && r.status < 300) break;
            /* Retry only transport failures and server-side errors; a 4xx
             * (validation, locked, …) is deterministic — fail immediately. */
            if (!(r.status == 0 || r.status >= 500)) break;
        }
        free(url);

        if (r.status >= 200 && r.status < 300) {
            if (strcmp(steps[i].url_path, "/air/apply") == 0) air_committed = true;
            if (strcmp(steps[i].url_path, "/gs/apply")  == 0) gs_applied = true;
        } else {
            rc = -1;
            const char *m = NULL;
            if (r.status == 0) {
                m = "GS unreachable";
                pthread_mutex_lock(&G.mu);
                bool was = G.gs_connected; G.gs_connected = false;
                pthread_mutex_unlock(&G.mu);
                if (was) notify_listener();
            } else {
                m = parse_error_message(r.body);
                /* A 502 from the /air/* proxy still surfaces a message; drone
                 * reachability is owned by conn_state, not by apply outcomes. */
                if (r.status == 502 && !steps[i].gs_side && !m) m = "Drone unreachable";
            }
            if (air_committed && steps[i].gs_side)
                snprintf(err, sizeof err, "Drone updated; GS apply failed: %s",
                         m ? m : "error");
            else
                snprintf(err, sizeof err, "%s", m ? m : "Request failed");
        }
        fpvd_http_result_free(&r);
    }

    if (rc == 0) {
        pthread_mutex_lock(&G.mu);
        if (job.kind == FPVD_ROW_STAGED && !job.apply_only) G.config_dirty = true;
        if (gs_applied) G.config_dirty = false;   /* any /gs/apply commits staged changes */
        bool notify = refresh_snapshot_unlocked();
        pthread_mutex_unlock(&G.mu);
        if (notify) notify_listener();
    }

done:
    schedule_done(job.on_done, job.user_data, rc, err[0] ? err : NULL);
}

/* -------------------------------------------------------------------------
 * Worker thread main loop
 * ---------------------------------------------------------------------- */

static void *worker_main(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&G.mu);
        while (!G.stop && G.queue_n == 0) {
            if (G.refresh_now) {
                G.refresh_now = false;
                bool notify = refresh_snapshot_unlocked();
                if (notify) {
                    pthread_mutex_unlock(&G.mu);
                    notify_listener();
                    pthread_mutex_lock(&G.mu);
                }
                continue;
            }
            int wait_ms = G.gs_connected ? (G.visible ? 3000 : 60000) : 2000;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int wr = pthread_cond_timedwait(&G.cv, &G.mu, &ts);
            if (wr == ETIMEDOUT) {
                bool notify = refresh_snapshot_unlocked();
                if (notify) {
                    pthread_mutex_unlock(&G.mu);
                    notify_listener();
                    pthread_mutex_lock(&G.mu);
                }
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
        /* Two rows share path "link.txPowerDbm" on different endpoints (drone TX
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
    /* Zero the slot first: it may be reused from a prior apply-only job, so any
     * field not explicitly written below (notably apply_only and kind) must
     * not inherit stale values. New job fields then auto-zero by default. */
    memset(j, 0, sizeof *j);
    strncpy(j->path,  e->path, sizeof j->path  - 1); j->path [sizeof j->path -1] = '\0';
    strncpy(j->value, value,   sizeof j->value - 1); j->value[sizeof j->value-1] = '\0';
    j->type       = e->type;
    j->endpoint   = e->endpoint;
    j->kind       = e->kind;
    j->on_done   = cb;
    j->user_data = ud;
}

static char *prov_get(const char *d, const char *p, const char *k) {
    if (pp_runtime_cfg_owns(d, p, k)) return pp_runtime_cfg_get(d, p, k);
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return strdup("");
    pthread_mutex_lock(&G.mu);
    cJSON *snap = (e->endpoint == FPVD_EP_GS) ? G.gs_snapshot : G.air_snapshot;
    char *out = snap ? fpvd_snapshot_read_string(snap, e->path, e->type) : strdup("");
    /* GS txPowerDbm may be null (driver default): fall back to the live
     * radio power reported by /gs/status. */
    if (out && out[0] == '\0' && e->endpoint == FPVD_EP_GS &&
        strcmp(e->path, "link.txPowerDbm") == 0 && G.status_snapshot) {
        cJSON *radio = cJSON_GetObjectItemCaseSensitive(G.status_snapshot, "radio");
        cJSON *first = (radio && cJSON_IsArray(radio)) ? cJSON_GetArrayItem(radio, 0) : NULL;
        cJSON *tx    = first ? cJSON_GetObjectItemCaseSensitive(first, "txpowerDbm") : NULL;
        if (tx && cJSON_IsNumber(tx)) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", (int)(tx->valuedouble + 0.5));
            free(out); out = strdup(buf);
        }
    }
    pthread_mutex_unlock(&G.mu);
    return out;
}

/* True when the Dynamic Link lock currently governs this field, applying the
 * FEC-mode-aware exceptions:
 *   - fec_mode is always editable (the user selects rs/swfec even with DL on).
 *   - In swfec mode, deadlineMs/overheadPct are editable, and the compute
 *     baseRedundancyRatio/blocksPerFrame become locked (swfec ignores them).
 * Caller passes the already-resolved keymap entry; must NOT hold G.mu. */
static bool dl_locks_field(const fpvd_keymap_entry_t *e,
                           const char *d, const char *p, const char *k) {
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on   = en && cJSON_IsTrue(en);
    cJSON *link  = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "link") : NULL;
    cJSON *fec   = link ? cJSON_GetObjectItemCaseSensitive(link, "fec") : NULL;
    cJSON *mode  = fec ? cJSON_GetObjectItemCaseSensitive(fec, "mode") : NULL;
    bool swfec   = mode && cJSON_IsString(mode) && strcmp(mode->valuestring, "swfec") == 0;
    pthread_mutex_unlock(&G.mu);

    if (!dl_on) return false;

    bool is_air_wfbng = (!strcmp(d, "air") && !strcmp(p, "wfbng"));
    bool is_air_dlink = (!strcmp(d, "air") && !strcmp(p, "dlink"));

    /* FEC Mode: always editable. */
    if (is_air_wfbng && !strcmp(k, "fec_mode")) return false;
    /* swfec: deadline/overhead editable; in rs they're hidden anyway. */
    if (swfec && is_air_wfbng &&
        (!strcmp(k, "fec_deadline_ms") || !strcmp(k, "fec_overhead_pct")))
        return false;
    /* swfec: the compute redundancy/blocks knobs are ignored, so grey them. */
    if (swfec && is_air_dlink &&
        (!strcmp(k, "compute_base_redundancy") || !strcmp(k, "compute_blocks_per_frame")))
        return true;

    /* Default: locked iff the path is under a locked prefix. */
    return fpvd_is_locked_path(e->path);
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    if (pp_runtime_cfg_owns(d, p, k)) {
        pp_runtime_cfg_set(d, p, k, v);
        schedule_done(cb, ud, 0, NULL);   /* applied + persisted synchronously */
        return;
    }
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) { schedule_done(cb, ud, -1, "Unknown setting"); return; }
    /* Dynamic-link lock only governs drone-owned (AIR) fields; the mode-aware
     * exceptions live in dl_locks_field (shared with prov_is_locked). */
    if (e->endpoint == FPVD_EP_AIR && dl_locks_field(e, d, p, k)) {
        schedule_done(cb, ud, -1, "Locked by Dynamic Link");
        return;
    }
    pthread_mutex_lock(&G.mu);
    if (e->kind == FPVD_ROW_STAGED) {
        /* No manual Apply button anymore: a staged change (screen_mode is the
         * only staged row) self-applies. Stage the value, then enqueue an
         * apply-only job (POST /apply -> pixelpilot restart). The completion cb
         * rides the apply job; the set itself carries none. */
        enqueue_locked(e, v, NULL, NULL);
        if (G.queue_n < FPVD_QUEUE_CAP) {
            fpvd_job_t *j = &G.queue[G.queue_n++];
            memset(j, 0, sizeof *j);
            j->endpoint   = FPVD_EP_GS;
            j->kind       = FPVD_ROW_STAGED;
            j->apply_only = true;
            j->on_done    = cb;
            j->user_data  = ud;
        } else {
            schedule_done(cb, ud, -1, "Settings queue full");
        }
    } else {
        enqueue_locked(e, v, cb, ud);
    }
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
    j->endpoint   = FPVD_EP_GS;
    j->kind       = FPVD_ROW_STAGED;
    j->apply_only = true;
    j->on_done    = cb;
    j->user_data  = ud;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static bool prov_is_available(const char *d, const char *p, const char *k) {
    if (pp_runtime_cfg_owns(d, p, k)) return true;
    return fpvd_keymap_lookup(d, p, k) != NULL;
}

static bool prov_has_pending(void) {
    pthread_mutex_lock(&G.mu);
    bool dirty = G.config_dirty;
    pthread_mutex_unlock(&G.mu);
    return dirty;
}

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    if (pp_runtime_cfg_owns(d, p, k)) {
        /* DVR rows are read-only mid-recording; cc rows are always live. */
        bool is_dvr = (strcmp(p, "dvr") == 0);
        return is_dvr && pp_runtime_cfg_is_recording();
    }
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    /* DL governs drone-owned (AIR) fields. The Bandwidth row is the one
     * GS-row exception: gs/wfbng/bandwidth -> link.width is pushed to the
     * drone and rejected by its dynamic-link lock, so disable it too. Other
     * GS rows (e.g. rx_power = the GS card's own power) stay editable. */
    bool is_bandwidth = (!strcmp(d, "gs") && !strcmp(p, "wfbng") &&
                         !strcmp(k, "bandwidth"));
    if (e->endpoint != FPVD_EP_AIR && !is_bandwidth) return false;
    return dl_locks_field(e, d, p, k);
}

static bool prov_is_connected(void) {
    pthread_mutex_lock(&G.mu);
    bool c = G.gs_connected;
    pthread_mutex_unlock(&G.mu);
    return c;
}

static bool prov_is_reachable(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return true;
    /* AIR rows and the beamforming handshake need the drone; GS rows —
     * including SHARED channel/width (the offline recovery path) — do not. */
    if (e->endpoint != FPVD_EP_AIR && e->kind != FPVD_ROW_BEAMFORM) return true;
    return conn_state_get().state == CONN_CONNECTED;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    G.listener_cb = cb;
    G.listener_ud = ud;
    pthread_mutex_unlock(&G.mu);
}

static void prov_set_visibility(bool v) {
    pthread_mutex_lock(&G.mu);
    if (v && !G.visible) G.refresh_now = true;
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
    .is_reachable           = prov_is_reachable,
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
    G.refresh_now = false;
    G.gs_connected    = false;
    if (G.air_snapshot)    { cJSON_Delete(G.air_snapshot);    G.air_snapshot    = NULL; }
    if (G.gs_snapshot)     { cJSON_Delete(G.gs_snapshot);     G.gs_snapshot     = NULL; }
    if (G.status_snapshot) { cJSON_Delete(G.status_snapshot); G.status_snapshot = NULL; }
    G.config_dirty = false;
    G.queue_n   = 0;
    G.listener_cb = NULL;
    G.listener_ud = NULL;
    pthread_mutex_unlock(&G.mu);
    pthread_cond_signal(&G.cv);

    /* Prime snapshot synchronously (best-effort). */
    pthread_mutex_lock(&G.mu);
    bool notify = refresh_snapshot_unlocked();
    pthread_mutex_unlock(&G.mu);
    if (notify) notify_listener();

    if (!G.worker_started) {
        pthread_create(&G.worker, NULL, worker_main, NULL);
#ifndef PP_FPVD_TEST
        conn_state_start(G.base_url, 1000);
        conn_state_subscribe(on_conn_state_change, NULL);
#endif
        G.worker_started = true;
    }
    pp_settings_register(&G_PROVIDER);
}
