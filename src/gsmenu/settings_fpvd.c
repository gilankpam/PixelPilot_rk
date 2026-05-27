/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"
#include "settings_fpvd_internal.h"

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

#define FPVD_DEFAULT_URL "http://10.5.0.10:8080"
#define FPVD_QUEUE_CAP   32

typedef struct fpvd_job {
    char     path[128];           /* dotted json path */
    char     value[128];          /* UI value string */
    fpvd_type_t type;
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

    cJSON   *snapshot;            /* protected by mu */

    fpvd_job_t queue[FPVD_QUEUE_CAP];
    size_t     queue_n;

    pp_settings_snapshot_cb listener_cb;
    void                   *listener_ud;
} fpvd_state_t;

static fpvd_state_t G;

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
    fpvd_done_dispatch_t *d = lv_malloc(sizeof *d);
    d->cb = cb;
    d->user_data = ud;
    d->rc = rc;
    if (err) { strncpy(d->err, err, sizeof d->err - 1); d->err[sizeof d->err - 1] = '\0'; }
    else d->err[0] = '\0';
    lv_async_call(done_dispatch_async, d);
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
    lv_async_call(listener_dispatch_async, NULL);
}

/* -------------------------------------------------------------------------
 * URL helper + snapshot refresh
 * ---------------------------------------------------------------------- */

static char *url_join(const char *base, const char *path) {
    size_t n = strlen(base) + strlen(path) + 1;
    char *u = malloc(n);
    snprintf(u, n, "%s%s", base, path);
    return u;
}

/* Called with G.mu HELD. Releases and re-acquires mutex around the HTTP call. */
static void refresh_snapshot_unlocked(void) {
    char *u = url_join(G.base_url, "/config");
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t r = fpvd_http_get(u);
    pthread_mutex_lock(&G.mu);
    free(u);
    bool was_connected = G.connected;
    if (r.status == 200 && r.body) {
        cJSON *new_snap = cJSON_Parse(r.body);
        if (new_snap) {
            if (G.snapshot) cJSON_Delete(G.snapshot);
            G.snapshot = new_snap;
            G.connected = true;
        }
    } else {
        G.connected = false;
    }
    fpvd_http_result_free(&r);
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
    cJSON *body = fpvd_build_patch_body(job.path, job.value, job.type);
    char *body_s = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body) cJSON_Delete(body);

    char *patch_url = url_join(G.base_url, "/config");
    char *apply_url = url_join(G.base_url, "/apply");

    fpvd_http_result_t r = fpvd_http_patch_json(patch_url, body_s ? body_s : "{}");
    int rc = 0;
    char err[160] = {0};
    if (r.status == 0) {
        rc = -1; snprintf(err, sizeof err, "Drone unreachable");
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

    if (rc == 0) {
        r = fpvd_http_post(apply_url);
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "Drone unreachable");
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
        if (strcmp(G.queue[i].path, e->path) == 0) {
            /* Coalesce: replace value + callback (the older callback is dropped —
             * the originating widget will see only the new callback's outcome). */
            strncpy(G.queue[i].value, value, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            G.queue[i].on_done = cb;
            G.queue[i].user_data = ud;
            return;
        }
    }
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        if (cb) cb(-1, "Settings queue full", ud);  /* caller is LVGL thread, safe */
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    strncpy(j->path,  e->path,  sizeof j->path  - 1); j->path [sizeof j->path -1] = '\0';
    strncpy(j->value, value,    sizeof j->value - 1); j->value[sizeof j->value-1] = '\0';
    j->type      = e->type;
    j->on_done   = cb;
    j->user_data = ud;
}

static char *prov_get(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return strdup("");
    pthread_mutex_lock(&G.mu);
    char *out = G.snapshot
        ? fpvd_snapshot_read_string(G.snapshot, e->path, e->type)
        : strdup("");
    pthread_mutex_unlock(&G.mu);
    return out;
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) { schedule_done(cb, ud, -1, "Unknown setting"); return; }
    if (fpvd_is_locked_path(e->path)) {
        pthread_mutex_lock(&G.mu);
        cJSON *dlink = G.snapshot ? cJSON_GetObjectItemCaseSensitive(G.snapshot, "dynamicLink") : NULL;
        cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
        bool dlink_on = en && cJSON_IsTrue(en);
        pthread_mutex_unlock(&G.mu);
        if (dlink_on) {
            schedule_done(cb, ud, -1, "Locked by Dynamic Link");
            return;
        }
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

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.snapshot ? cJSON_GetObjectItemCaseSensitive(G.snapshot, "dynamicLink") : NULL;
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
};

void pp_settings_register_fpvd(void) {
    /* lv_init is idempotent; calling it ensures the LVGL machinery
     * (used by lv_async_call/lv_malloc) is available even if our
     * caller initializes LVGL later. */
    static int lv_initted = 0;
    if (!lv_initted) { lv_init(); lv_initted = 1; }

    fpvd_curl_init_once();
    const char *u = getenv("PP_FPVD_URL");
    strncpy(G.base_url, u && *u ? u : FPVD_DEFAULT_URL, sizeof G.base_url - 1);
    G.base_url[sizeof G.base_url - 1] = '\0';

    if (!G.worker_started) {
        pthread_mutex_init(&G.mu, NULL);
        pthread_cond_init(&G.cv, NULL);
    }
    G.stop      = false;
    G.visible   = false;
    G.connected = false;
    if (G.snapshot) { cJSON_Delete(G.snapshot); G.snapshot = NULL; }
    G.queue_n   = 0;
    G.listener_cb = NULL;
    G.listener_ud = NULL;

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
