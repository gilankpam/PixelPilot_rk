#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdlib.h>
#include <string.h>

static void stub_set(const char *d, const char *p, const char *k, const char *v) {
    LV_LOG_USER("settings.set %s/%s/%s = %s", d, p, k, v ? v : "(null)");
}

static char *stub_get(const char *d, const char *p, const char *k) {
    (void)d; (void)p; (void)k;
    /* Empty string means "known but unset" — widgets render their placeholder. */
    return strdup("");
}

static void stub_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb on_done) {
    LV_LOG_USER("settings.set_async %s/%s/%s = %s", d, p, k, v ? v : "(null)");
    if (on_done) on_done(0, NULL);
}

static const pp_settings_provider_t g_stub = {
    .set = stub_set,
    .get = stub_get,
    .set_async = stub_set_async,
};

void pp_settings_register_stub(void) {
    pp_settings_register(&g_stub);
}
