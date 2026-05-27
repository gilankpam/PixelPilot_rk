#include "settings.h"
#include "settings_gs_enum.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const pp_settings_provider_t *g_provider = NULL;

/* Multi-listener fanout. The provider's single set_snapshot_listener slot
 * holds our dispatcher_fanout function; we maintain our own list of
 * subscribers so multiple page builders can each register independently. */
#define PP_SETTINGS_MAX_LISTENERS 16

typedef struct {
    pp_settings_snapshot_cb cb;
    void *ud;
} pp_listener_entry_t;

static pp_listener_entry_t g_listeners[PP_SETTINGS_MAX_LISTENERS];
static size_t              g_listeners_n   = 0;
static bool                g_fanout_armed  = false;

static void dispatcher_fanout(void *ud) {
    (void)ud;
    /* Snapshot the list under no lock — this is fine because all calls run
     * on the LVGL thread by contract (listeners are dispatched via
     * lv_async_call which drains on the main thread). */
    for (size_t i = 0; i < g_listeners_n; i++) {
        if (g_listeners[i].cb) g_listeners[i].cb(g_listeners[i].ud);
    }
}

void pp_settings_register(const pp_settings_provider_t *provider) {
    g_listeners_n = 0;
    g_fanout_armed = false;
    g_provider = provider;
}

void pp_settings_set(const char *domain, const char *page,
                     const char *key, const char *value) {
    if (g_provider && g_provider->set) {
        g_provider->set(domain, page, key, value);
    }
}

char *pp_settings_get(const char *domain, const char *page, const char *key) {
    if (g_provider && g_provider->get) {
        return g_provider->get(domain, page, key);
    }
    return NULL;
}

void pp_settings_set_async(const char *domain, const char *page,
                           const char *key, const char *value,
                           pp_settings_done_cb on_done, void *user_data) {
    if (g_provider && g_provider->set_async) {
        g_provider->set_async(domain, page, key, value, on_done, user_data);
    } else if (g_provider && g_provider->set) {
        /* Fall back to sync set + immediate callback. */
        g_provider->set(domain, page, key, value);
        if (on_done) on_done(0, NULL, user_data);
    } else if (on_done) {
        on_done(-1, "no provider registered", user_data);
    }
}

bool pp_settings_is_locked(const char *d, const char *p, const char *k) {
    if (g_provider && g_provider->is_locked) {
        return g_provider->is_locked(d, p, k);
    }
    return false;
}

bool pp_settings_is_connected(void) {
    if (g_provider && g_provider->is_connected) {
        return g_provider->is_connected();
    }
    return true;
}

void pp_settings_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    if (!cb) {
        /* cb=NULL clears all listeners (rare; mainly for tests). */
        g_listeners_n = 0;
        if (g_provider && g_provider->set_snapshot_listener) {
            g_provider->set_snapshot_listener(NULL, NULL);
            g_fanout_armed = false;
        }
        return;
    }
    /* Append (silently ignore duplicates of (cb, ud)). */
    for (size_t i = 0; i < g_listeners_n; i++) {
        if (g_listeners[i].cb == cb && g_listeners[i].ud == ud) return;
    }
    if (g_listeners_n >= PP_SETTINGS_MAX_LISTENERS) return;
    g_listeners[g_listeners_n].cb = cb;
    g_listeners[g_listeners_n].ud = ud;
    g_listeners_n++;
    /* Arm the dispatcher_fanout once. */
    if (!g_fanout_armed && g_provider && g_provider->set_snapshot_listener) {
        g_provider->set_snapshot_listener(dispatcher_fanout, NULL);
        g_fanout_armed = true;
    }
}

void pp_settings_set_visibility(bool visible) {
    if (g_provider && g_provider->set_visibility) {
        g_provider->set_visibility(visible);
    }
}

char *pp_settings_get_options(const char *domain, const char *page, const char *key) {
    if (!domain || !page || !key) return NULL;
    if (strcmp(domain, "gs") == 0 && strcmp(page, "wfbng") == 0 && strcmp(key, "gs_channel") == 0)
        return pp_gs_enum_channels();
    if (strcmp(domain, "gs") == 0 && strcmp(page, "display") == 0 && strcmp(key, "hdmi_mode") == 0)
        return pp_gs_enum_hdmi_modes();
    return NULL;
}
