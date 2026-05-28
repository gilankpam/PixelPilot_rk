/* src/gsmenu/settings_router.c */
#include "settings_router_internal.h"
#include "settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PP_ROUTER_TEST
#include "settings_gs_local_internal.h"
/* Forward decl from fpvd. */
extern void pp_settings_register_fpvd(void);
extern const pp_settings_provider_t *pp_fpvd_provider_for_router(void);
#endif

/* Fan-out table: (in domain/page/key) -> (out domain/page/key on GS). */
typedef struct {
    const char *d, *p, *k;
    const char *gd, *gp, *gk;
} fanout_t;

static const fanout_t FANOUT[] = {
    { "gs",  "wfbng",  "gs_channel", "gs", "wfbng",   "gs_channel" },
    { "gs",  "wfbng",  "bandwidth",  "gs", "wfbng",   "bandwidth"  },
    { "air", "camera", "codec",      "gs", "pp",      "codec"      },
};
static const size_t FANOUT_N = sizeof(FANOUT) / sizeof(FANOUT[0]);

static const fanout_t *fanout_lookup(const char *d, const char *p, const char *k) {
    for (size_t i = 0; i < FANOUT_N; i++) {
        if (strcmp(FANOUT[i].d, d) == 0 &&
            strcmp(FANOUT[i].p, p) == 0 &&
            strcmp(FANOUT[i].k, k) == 0)
            return &FANOUT[i];
    }
    return NULL;
}

static const pp_settings_provider_t *g_drone;
static const pp_settings_provider_t *g_gs;

void pp_router_install_children(const pp_settings_provider_t *drone,
                                const pp_settings_provider_t *gs) {
    g_drone = drone;
    g_gs    = gs;
    pp_settings_register(pp_router_provider());
}

void pp_router_reset(void) {
    g_drone = NULL;
    g_gs    = NULL;
    pp_settings_register(NULL);
}

/* For fan-out, we need a 2-stage callback. */
typedef struct {
    const fanout_t *fan;
    char  value[128];
    pp_settings_done_cb cb;
    void *user_data;
} fanout_state_t;

static void on_gs_done(int rc, const char *err, void *ud) {
    fanout_state_t *st = (fanout_state_t *)ud;
    if (rc == 0) {
        if (st->cb) st->cb(0, NULL, st->user_data);
    } else {
        char buf[256];
        snprintf(buf, sizeof buf, "Drone applied; GS: %s",
                 err ? err : "write failed");
        if (st->cb) st->cb(-1, buf, st->user_data);
    }
    free(st);
}

static void on_drone_done(int rc, const char *err, void *ud) {
    fanout_state_t *st = (fanout_state_t *)ud;
    if (rc != 0) {
        if (st->cb) st->cb(rc, err, st->user_data);
        free(st);
        return;
    }
    /* Drone OK -> fire GS. */
    if (g_gs && g_gs->set_async) {
        g_gs->set_async(st->fan->gd, st->fan->gp, st->fan->gk,
                        st->value, on_gs_done, st);
    } else {
        if (st->cb) st->cb(0, NULL, st->user_data);
        free(st);
    }
}

static char *prov_get(const char *d, const char *p, const char *k) {
    /* Domain dispatch on read. fpvd knows some "gs" keys, gs_local knows
     * others — try the matching child first, fall back to the other. */
    if (strcmp(d, "gs") == 0 && g_gs && g_gs->get) {
        char *r = g_gs->get(d, p, k);
        if (r && *r) return r;
        if (r) free(r);
        if (g_drone && g_drone->get) return g_drone->get(d, p, k);
        return strdup("");
    }
    if (g_drone && g_drone->get) return g_drone->get(d, p, k);
    if (g_gs && g_gs->get)        return g_gs->get(d, p, k);
    return strdup("");
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    const fanout_t *fan = fanout_lookup(d, p, k);
    if (fan) {
        fanout_state_t *st = (fanout_state_t *)calloc(1, sizeof *st);
        if (!st) { if (cb) cb(-1, "Out of memory", ud); return; }
        st->fan = fan;
        strncpy(st->value, v ? v : "", sizeof st->value - 1);
        st->cb = cb; st->user_data = ud;
        if (g_drone && g_drone->set_async) {
            g_drone->set_async(d, p, k, v, on_drone_done, st);
        } else {
            on_drone_done(0, NULL, st);   /* no drone provider -> proceed to gs */
        }
        return;
    }
    /* Non-fanout: dispatch by domain. fpvd handles "air" and many "gs" keys;
     * gs_local handles GS-only keys it knows about. Try drone first; if it
     * reports "Unknown setting" let gs handle. We pick a simple rule: if
     * domain == "gs" and key is rx_power/hdmi_mode, route to gs_local; else
     * route to drone. */
    bool gs_only =
        (strcmp(d, "gs") == 0 &&
         ((strcmp(p, "link")    == 0 && strcmp(k, "rx_power")  == 0) ||
          (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) ||
          (strcmp(p, "pp")      == 0 && strcmp(k, "codec")     == 0) ||
          (strcmp(p, "actions") == 0)));
    const pp_settings_provider_t *child = gs_only ? g_gs : g_drone;
    if (child && child->set_async) child->set_async(d, p, k, v, cb, ud);
    else if (cb) cb(-1, "No provider", ud);
}

static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    prov_set_async(d, p, k, v, NULL, NULL);
}

static bool prov_is_connected(void) {
    bool dc = (!g_drone || !g_drone->is_connected) ? true : g_drone->is_connected();
    bool gc = (!g_gs    || !g_gs->is_connected)    ? true : g_gs->is_connected();
    return dc && gc;
}

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    bool dl = (g_drone && g_drone->is_locked) ? g_drone->is_locked(d,p,k) : false;
    bool gl = (g_gs    && g_gs->is_locked)    ? g_gs->is_locked(d,p,k)    : false;
    return dl || gl;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    if (g_drone && g_drone->set_snapshot_listener) g_drone->set_snapshot_listener(cb, ud);
    if (g_gs    && g_gs->set_snapshot_listener)    g_gs->set_snapshot_listener(cb, ud);
}

static void prov_set_visibility(bool v) {
    if (g_drone && g_drone->set_visibility) g_drone->set_visibility(v);
    if (g_gs    && g_gs->set_visibility)    g_gs->set_visibility(v);
}

static const pp_settings_provider_t G_ROUTER = {
    .set                   = prov_set,
    .get                   = prov_get,
    .set_async             = prov_set_async,
    .is_locked             = prov_is_locked,
    .is_connected          = prov_is_connected,
    .set_snapshot_listener = prov_set_snapshot_listener,
    .set_visibility        = prov_set_visibility,
};

const pp_settings_provider_t *pp_router_provider(void) { return &G_ROUTER; }

/* Public entry that the device build calls. fpvd's existing init code is
 * unchanged — we hand it through a small accessor (added in fpvd below).
 * Guard with PP_ROUTER_TEST so the test binary can link without settings_fpvd.c. */
#ifndef PP_ROUTER_TEST
void pp_settings_register_router(void) {
    pp_settings_register_fpvd();           /* spins up the worker + initial poll */
    /* Once fpvd is registered, its provider table is the active one; we now
     * yank it out and reinstall the router on top. fpvd exposes its provider
     * via pp_fpvd_provider_for_router(). */
    g_drone = pp_fpvd_provider_for_router();
    g_gs    = pp_gs_local_provider();
    pp_settings_register(&G_ROUTER);
}
#endif
