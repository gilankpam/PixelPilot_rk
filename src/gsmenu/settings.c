#include "settings.h"
#include <stdbool.h>
#include <stddef.h>

static const pp_settings_provider_t *g_provider = NULL;

void pp_settings_register(const pp_settings_provider_t *provider) {
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
    if (g_provider && g_provider->set_snapshot_listener) {
        g_provider->set_snapshot_listener(cb, ud);
    }
}

void pp_settings_set_visibility(bool visible) {
    if (g_provider && g_provider->set_visibility) {
        g_provider->set_visibility(visible);
    }
}
