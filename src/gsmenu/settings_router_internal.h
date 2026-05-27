/* src/gsmenu/settings_router_internal.h */
#ifndef PP_SETTINGS_ROUTER_INTERNAL_H
#define PP_SETTINGS_ROUTER_INTERNAL_H

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For tests: install router with explicit children. Both children must
 * outlive the router. */
void pp_router_install_children(const pp_settings_provider_t *drone,
                                const pp_settings_provider_t *gs);

/* Reset router state (tests only). */
void pp_router_reset(void);

/* Returns the router's vtable (registration-only convenience for code paths
 * that need the table itself). */
const pp_settings_provider_t *pp_router_provider(void);

#ifdef __cplusplus
}
#endif
#endif
