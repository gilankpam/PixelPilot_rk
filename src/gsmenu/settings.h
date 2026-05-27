#ifndef PP_SETTINGS_H
#define PP_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a real backend impl when an async set completes.
 * rc == 0 means success; err is NULL on success or a short message on failure. */
typedef void (*pp_settings_done_cb)(int rc, const char *err);

typedef struct {
    /* Synchronous set. Backend may persist immediately or queue. */
    void  (*set)(const char *domain, const char *page,
                 const char *key, const char *value);

    /* Synchronous get. Returns a heap-allocated string (caller free()s)
     * or NULL if the key is unknown. Empty string ("") means "known but unset". */
    char *(*get)(const char *domain, const char *page,
                 const char *key);

    /* Asynchronous set for slow backends. on_done may be NULL.
     * The implementation is responsible for thread safety; it may call
     * on_done synchronously if the operation is cheap. */
    void  (*set_async)(const char *domain, const char *page,
                       const char *key, const char *value,
                       pp_settings_done_cb on_done);
} pp_settings_provider_t;

/* Install (or replace) the active provider. Pointer must outlive the program.
 *
 * Threading: registration is expected to happen once at startup, before any
 * pp_settings_set/get/set_async calls. The implementation is not safe under
 * concurrent register-vs-call. Real-world callers run in a single LVGL UI
 * thread so this is intentional. */
void pp_settings_register(const pp_settings_provider_t *provider);

/* Convenience wrappers around the registered provider. Safe to call before
 * registration (set is a no-op, get returns NULL). */
void  pp_settings_set(const char *domain, const char *page,
                      const char *key, const char *value);
char *pp_settings_get(const char *domain, const char *page,
                      const char *key);
void  pp_settings_set_async(const char *domain, const char *page,
                            const char *key, const char *value,
                            pp_settings_done_cb on_done);

/* Registers the built-in no-op stub provider. */
void pp_settings_register_stub(void);

/* Registers an in-memory dummy provider with seeded values for every
 * key the page builders use. Writes update a per-session overlay;
 * nothing is persisted to disk. Intended only for the simulator. */
void pp_settings_register_dummy(void);

#ifdef __cplusplus
}
#endif

#endif
