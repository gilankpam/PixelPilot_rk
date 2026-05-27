#include "settings.h"
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
