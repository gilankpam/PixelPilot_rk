#ifndef PP_SETTINGS_H
#define PP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a real backend impl when an async set completes.
 * rc == 0 means success; err is NULL on success or a short message on failure.
 * user_data is the opaque pointer passed to pp_settings_set_async. */
typedef void (*pp_settings_done_cb)(int rc, const char *err, void *user_data);

/* Called on the LVGL thread when the provider's snapshot mutates (e.g.
 * after a successful apply, after a poll round, or on a connectivity
 * transition). UI listeners walk their rows and re-evaluate enabled/
 * disabled state. */
typedef void (*pp_settings_snapshot_cb)(void *user_data);

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
     * on_done synchronously if the operation is cheap. user_data is passed
     * through to on_done unchanged. */
    void  (*set_async)(const char *domain, const char *page,
                       const char *key, const char *value,
                       pp_settings_done_cb on_done, void *user_data);

    /* Optional: returns true if the key is currently read-only (e.g. owned
     * by a dynamic-link controller). NULL → dispatcher returns false. */
    bool  (*is_locked)(const char *domain, const char *page, const char *key);

    /* Optional: returns true if the backend is currently reachable.
     * NULL → dispatcher returns true. */
    bool  (*is_connected)(void);

    /* Optional: register a single listener for snapshot mutations. Passing
     * cb=NULL clears the listener. NULL pointer → dispatcher no-op. */
    void  (*set_snapshot_listener)(pp_settings_snapshot_cb cb, void *user_data);

    /* Optional: hint about UI visibility so the backend can throttle polls.
     * NULL pointer → dispatcher no-op. */
    void  (*set_visibility)(bool visible);
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
                            pp_settings_done_cb on_done, void *user_data);

/* Forwarding wrappers; safe to call regardless of which provider is
 * registered (return safe defaults when the underlying provider does
 * not implement the optional method). */
bool  pp_settings_is_locked(const char *domain, const char *page,
                            const char *key);
bool  pp_settings_is_connected(void);
void  pp_settings_set_snapshot_listener(pp_settings_snapshot_cb cb,
                                        void *user_data);
void  pp_settings_set_visibility(bool visible);

/* Registers the built-in no-op stub provider. */
void pp_settings_register_stub(void);

/* Registers an in-memory dummy provider with seeded values for every
 * key the page builders use. Writes update a per-session overlay;
 * nothing is persisted to disk. Intended only for the simulator. */
void pp_settings_register_dummy(void);

/* Registers the unified ground-station fpvd HTTP provider. Talks only to the
 * local GS fpvd; drone settings go through its /air/* proxy, GS radio through
 * /link. URL defaults to http://127.0.0.1:8080, overridable via PP_FPVD_URL. */
void pp_settings_register_fpvd(void);

/* Returns a newline-joined list of valid option strings for the given key,
 * or NULL if no enumerator is available. Caller frees with free().
 * The current implementation supports:
 *   ("gs","wfbng","gs_channel")  -> iw list channels
 *   ("gs","display","hdmi_mode") -> drm_info modes
 * All other tuples return NULL. */
char *pp_settings_get_options(const char *domain, const char *page, const char *key);

#ifdef __cplusplus
}
#endif

#endif
