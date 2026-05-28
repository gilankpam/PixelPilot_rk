/* src/gsmenu/settings_gs_local.c */
#include "settings_gs_local_internal.h"
#include "settings_gs_writers.h"
#include "settings_gs_rxpower.h"
#include "settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl.h"

#define GS_QUEUE_CAP 16

typedef enum {
    GS_KEY_CHANNEL,
    GS_KEY_BANDWIDTH,
    GS_KEY_RXPOWER,
    GS_KEY_CODEC,
    GS_KEY_HDMI_MODE,
    GS_KEY_RESTART_PIXELPILOT,
    GS_KEY_NONE,
} gs_key_t;

typedef struct {
    gs_key_t key;
    char     value[128];
    pp_settings_done_cb cb;
    void    *user_data;
} gs_job_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            started, stop;
    gs_job_t        queue[GS_QUEUE_CAP];
    size_t          queue_n;

    /* Configurable paths / bins. */
    char wfb_cfg[256];
    char pp_env[256];
    char systemctl_bin[128];

    /* Snapshot. */
    char *channel;
    char *bandwidth;
    int   rxpower_pct;
    char *codec;
    char *hdmi_mode;

    pp_settings_snapshot_cb listener;
    void *listener_ud;

    bool connected;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .rxpower_pct = -1,
        .connected   = true };

static void set_path(char *dst, size_t sz, const char *src) {
    strncpy(dst, src, sz - 1); dst[sz - 1] = '\0';
}

static void init_paths_once(void) {
    if (G.wfb_cfg[0] == '\0') set_path(G.wfb_cfg, sizeof G.wfb_cfg, "/etc/wifibroadcast.cfg");
    if (G.pp_env[0]  == '\0') set_path(G.pp_env,  sizeof G.pp_env,  "/etc/default/pixelpilot");
    if (G.systemctl_bin[0] == '\0') set_path(G.systemctl_bin, sizeof G.systemctl_bin, "systemctl");
}

void pp_gs_local_set_paths(const char *wfb, const char *env) {
    pthread_mutex_lock(&G.mu);
    if (wfb) set_path(G.wfb_cfg, sizeof G.wfb_cfg, wfb);
    if (env) set_path(G.pp_env,  sizeof G.pp_env,  env);
    pthread_mutex_unlock(&G.mu);
}

void pp_gs_local_set_systemctl_bin(const char *bin) {
    pthread_mutex_lock(&G.mu);
    if (bin) set_path(G.systemctl_bin, sizeof G.systemctl_bin, bin);
    pthread_mutex_unlock(&G.mu);
}

/* Key resolution. */
static gs_key_t resolve_key(const char *d, const char *p, const char *k) {
    if (strcmp(d, "gs") == 0) {
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "gs_channel") == 0) return GS_KEY_CHANNEL;
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "bandwidth")  == 0) return GS_KEY_BANDWIDTH;
        if (strcmp(p, "link")  == 0 && strcmp(k, "rx_power")   == 0) return GS_KEY_RXPOWER;
        if (strcmp(p, "pp")    == 0 && strcmp(k, "codec")      == 0) return GS_KEY_CODEC;
        if (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) return GS_KEY_HDMI_MODE;
        if (strcmp(p, "actions") == 0 && strcmp(k, "restart_pixelpilot") == 0) return GS_KEY_RESTART_PIXELPILOT;
    }
    return GS_KEY_NONE;
}

/* Dispatch a callback on the LVGL thread. */
typedef struct { pp_settings_done_cb cb; void *ud; int rc; char err[160]; } gs_done_t;
static void done_async(void *ptr) {
    gs_done_t *d = (gs_done_t *)ptr;
    if (d->cb) d->cb(d->rc, d->err[0] ? d->err : NULL, d->ud);
    lv_free(d);
}
static void schedule_done(pp_settings_done_cb cb, void *ud, int rc, const char *err) {
    if (!cb) return;
    lv_lock();
    gs_done_t *d = lv_malloc(sizeof *d);
    if (!d) { lv_unlock(); return; }
    d->cb = cb; d->ud = ud; d->rc = rc;
    if (err) { strncpy(d->err, err, sizeof d->err - 1); d->err[sizeof d->err - 1] = '\0'; }
    else d->err[0] = '\0';
    lv_async_call(done_async, d);
    lv_unlock();
}

static int run_systemctl_restart(const char *service) {
    /* Returns 0 on exit code 0, non-zero otherwise. */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp(G.systemctl_bin, G.systemctl_bin, "restart", service, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static void notify_listener(void) {
    pthread_mutex_lock(&G.mu);
    pp_settings_snapshot_cb cb = G.listener;
    void *ud = G.listener_ud;
    pthread_mutex_unlock(&G.mu);
    if (cb) cb(ud);
}

/* Actual work, called outside the mutex. */
static void run_job(gs_job_t job) {
    pp_gs_write_result_t r = { -1, NULL };
    bool needs_restart = false;
    const char *toast_msg = NULL;

    switch (job.key) {
    case GS_KEY_CHANNEL:
        r = pp_gs_wfbcfg_set_channel(G.wfb_cfg, job.value);
        needs_restart = true;
        break;
    case GS_KEY_BANDWIDTH:
        r = pp_gs_wfbcfg_set_bandwidth(G.wfb_cfg, job.value);
        needs_restart = true;
        break;
    case GS_KEY_RXPOWER: {
        char **nics = pp_rxpower_list_wlx_nics();
        if (!nics || !nics[0]) {
            r.rc = -1; r.err = strdup("No wfb NICs detected");
            if (nics) free(nics);
        } else {
            size_t n = 0; while (nics[n]) n++;
            pp_nic_driver_t *drv = (pp_nic_driver_t *)calloc(n, sizeof *drv);
            for (size_t i = 0; i < n; i++) {
                char *name = pp_rxpower_nic_driver_name(nics[i]);
                drv[i] = pp_nic_driver_from_name(name);
                free(name);
            }
            int pct = atoi(job.value);
            char *json = pp_rxpower_build_json((const char *const *)nics, drv, pct);
            free(drv);
            for (size_t i = 0; i < n; i++) free(nics[i]);
            free(nics);
            if (!json) { r.rc = -1; r.err = strdup("No supported NIC drivers"); }
            else { r = pp_gs_wfbcfg_set_txpower(G.wfb_cfg, json); free(json); }
        }
        needs_restart = (r.rc == 0);
        break;
    }
    case GS_KEY_CODEC:
        r = pp_gs_env_set(G.pp_env, "CODEC", job.value);
        toast_msg = "Applies on next restart";
        break;
    case GS_KEY_HDMI_MODE:
        r = pp_gs_env_set(G.pp_env, "SCREEN_MODE", job.value);
        toast_msg = "Applies on next restart";
        break;
    case GS_KEY_RESTART_PIXELPILOT: {
        int xst = run_systemctl_restart("pixelpilot.service");
        if (xst != 0) {
            r.rc = -1; r.err = strdup("pixelpilot restart failed");
        } else {
            r.rc = 0; r.err = NULL;
            /* In practice this process is dying right now; the listener and
             * on_done dispatches below may never reach the UI. That's fine. */
        }
        break;
    }
    case GS_KEY_NONE:
        r.rc = -1; r.err = strdup("Unknown GS setting");
        break;
    }

    if (r.rc != 0) {
        schedule_done(job.cb, job.user_data, -1, r.err ? r.err : "GS write failed");
        pp_gs_write_result_free(&r);
        return;
    }
    pp_gs_write_result_free(&r);

    /* Restart, if any. */
    if (needs_restart) {
        int xst = run_systemctl_restart("wifibroadcast.service");
        if (xst != 0) {
            schedule_done(job.cb, job.user_data, -1, "wifibroadcast restart failed");
            return;
        }
    }

    /* Update snapshot under mutex. */
    pthread_mutex_lock(&G.mu);
    switch (job.key) {
    case GS_KEY_CHANNEL:   free(G.channel);   G.channel   = strdup(job.value); break;
    case GS_KEY_BANDWIDTH: free(G.bandwidth); G.bandwidth = strdup(job.value); break;
    case GS_KEY_RXPOWER:   G.rxpower_pct = atoi(job.value); break;
    case GS_KEY_CODEC:     free(G.codec);     G.codec     = strdup(job.value); break;
    case GS_KEY_HDMI_MODE: free(G.hdmi_mode); G.hdmi_mode = strdup(job.value); break;
    default: break;
    }
    pthread_mutex_unlock(&G.mu);

    notify_listener();
    /* On success-with-toast, pass message as the err string with rc=0 so widgets
     * render it as an informational toast. (Existing fpvd callers only react to
     * rc != 0; widgets that want the success-toast UX must opt in. For now we
     * pass rc=0, err=NULL because the existing widget code treats non-NULL err
     * as failure. Toast support for success messages is follow-up UI work.) */
    (void)toast_msg;
    schedule_done(job.cb, job.user_data, 0, NULL);
}

static void *worker_main(void *_) {
    (void)_;
    while (1) {
        pthread_mutex_lock(&G.mu);
        while (!G.stop && G.queue_n == 0) {
            pthread_cond_wait(&G.cv, &G.mu);
        }
        if (G.stop) { pthread_mutex_unlock(&G.mu); break; }
        gs_job_t job = G.queue[0];
        for (size_t i = 1; i < G.queue_n; i++) G.queue[i-1] = G.queue[i];
        G.queue_n--;
        pthread_mutex_unlock(&G.mu);

        run_job(job);
    }
    return NULL;
}

static void start_worker_once(void) {
    pthread_mutex_lock(&G.mu);
    if (!G.started) {
        init_paths_once();
        G.started = true;
        pthread_create(&G.thread, NULL, worker_main, NULL);
    }
    pthread_mutex_unlock(&G.mu);
}

void pp_gs_local_shutdown(void) {
    pthread_mutex_lock(&G.mu);
    if (!G.started) { pthread_mutex_unlock(&G.mu); return; }
    G.stop = true;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.thread, NULL);
    G.started = false;
    G.stop = false;
}

/* -------- vtable -------- */
static char *prov_get(const char *d, const char *p, const char *k) {
    gs_key_t key = resolve_key(d, p, k);
    if (key == GS_KEY_NONE) return strdup("");
    pthread_mutex_lock(&G.mu);
    char buf[32];
    char *out = strdup("");
    switch (key) {
    case GS_KEY_CHANNEL:   if (G.channel)   { free(out); out = strdup(G.channel); }   break;
    case GS_KEY_BANDWIDTH: if (G.bandwidth) { free(out); out = strdup(G.bandwidth); } break;
    case GS_KEY_RXPOWER:   if (G.rxpower_pct >= 0) { snprintf(buf, sizeof buf, "%d", G.rxpower_pct); free(out); out = strdup(buf); } break;
    case GS_KEY_CODEC:     if (G.codec)     { free(out); out = strdup(G.codec); }     break;
    case GS_KEY_HDMI_MODE: if (G.hdmi_mode) { free(out); out = strdup(G.hdmi_mode); } break;
    default: break;
    }
    pthread_mutex_unlock(&G.mu);
    return out;
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    gs_key_t key = resolve_key(d, p, k);
    if (key == GS_KEY_NONE) { schedule_done(cb, ud, -1, "Unknown GS setting"); return; }
    start_worker_once();
    pthread_mutex_lock(&G.mu);
    /* Coalesce. */
    for (size_t i = 0; i < G.queue_n; i++) {
        if (G.queue[i].key == key) {
            strncpy(G.queue[i].value, v, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            G.queue[i].cb = cb;
            G.queue[i].user_data = ud;
            pthread_mutex_unlock(&G.mu);
            return;
        }
    }
    if (G.queue_n >= GS_QUEUE_CAP) {
        pthread_mutex_unlock(&G.mu);
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    gs_job_t *j = &G.queue[G.queue_n++];
    j->key = key;
    strncpy(j->value, v, sizeof j->value - 1); j->value[sizeof j->value - 1] = '\0';
    j->cb = cb; j->user_data = ud;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    prov_set_async(d, p, k, v, NULL, NULL);
}

static bool prov_is_connected(void) {
    pthread_mutex_lock(&G.mu);
    bool c = G.connected;
    pthread_mutex_unlock(&G.mu);
    return c;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    G.listener = cb; G.listener_ud = ud;
    pthread_mutex_unlock(&G.mu);
}

static const pp_settings_provider_t G_PROVIDER = {
    .set                   = prov_set,
    .get                   = prov_get,
    .set_async             = prov_set_async,
    .is_locked             = NULL,
    .is_connected          = prov_is_connected,
    .set_snapshot_listener = prov_set_snapshot_listener,
    .set_visibility        = NULL,
};

const pp_settings_provider_t *pp_gs_local_provider(void) {
    return &G_PROVIDER;
}
