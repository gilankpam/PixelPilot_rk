#ifndef PP_SETTINGS_GS_WRITERS_H
#define PP_SETTINGS_GS_WRITERS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   rc;       /* 0 = success */
    char *err;      /* heap; NULL on success */
} pp_gs_write_result_t;

void pp_gs_write_result_free(pp_gs_write_result_t *r);

/* All three operate on the config file at `cfg_path` (overridable for tests).
 * On success, the file contains the new value and is atomically rewritten.
 * On failure, the file is untouched and r.err describes the error. */
pp_gs_write_result_t pp_gs_wfbcfg_set_channel  (const char *cfg_path, const char *value);
pp_gs_write_result_t pp_gs_wfbcfg_set_bandwidth(const char *cfg_path, const char *value);

/* `txpower_json` is the pre-built JSON-ish dict (e.g. `{"wlx...": -2000}`).
 * Caller is responsible for building it via pp_gs_build_txpower_json. */
pp_gs_write_result_t pp_gs_wfbcfg_set_txpower  (const char *cfg_path, const char *txpower_json);

/* Upserts KEY=VALUE in a shell-env-style file (e.g. /etc/default/pixelpilot).
 * Quotes value if it contains whitespace or shell metacharacters. */
pp_gs_write_result_t pp_gs_env_set(const char *env_path, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
#endif
