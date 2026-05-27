/* src/gsmenu/settings_gs_local_internal.h */
#ifndef PP_SETTINGS_GS_LOCAL_INTERNAL_H
#define PP_SETTINGS_GS_LOCAL_INTERNAL_H

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Construct the gs_local provider. Returns a pointer with static lifetime
 * (provider table inside the .c file). NULL on init failure. Idempotent. */
const pp_settings_provider_t *pp_gs_local_provider(void);

/* For tests: replace the systemctl binary path. */
void pp_gs_local_set_systemctl_bin(const char *bin);

/* For tests: replace the config file paths. */
void pp_gs_local_set_paths(const char *wfb_cfg, const char *pixelpilot_env);

/* For tests: shut down the worker thread cleanly. */
void pp_gs_local_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
