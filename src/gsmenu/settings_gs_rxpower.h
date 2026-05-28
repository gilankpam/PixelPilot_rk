/* src/gsmenu/settings_gs_rxpower.h */
#ifndef PP_SETTINGS_GS_RXPOWER_H
#define PP_SETTINGS_GS_RXPOWER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PP_NIC_RTL88XXAU_WFB,
    PP_NIC_RTL88X2EU,
    PP_NIC_UNKNOWN,
} pp_nic_driver_t;

/* Map driver name (from udevadm ID_NET_DRIVER) to enum. */
pp_nic_driver_t pp_nic_driver_from_name(const char *name);

/* Map percent (1..100, clamped) to a signed per-driver value.
 * Returns 0 and sets *out=0 for PP_NIC_UNKNOWN. Returns 1 on success. */
int pp_rxpower_pct_to_driver_value(pp_nic_driver_t driver, int pct, int *out);

/* Build the `wifi_txpower = { ... }` JSON-ish dict body for the given NICs.
 * `nics` is a NULL-terminated array of strings (e.g. {"wlx00", NULL}).
 * `driver_for` is a parallel array of driver enums. Returns a heap-allocated
 * string like `{"wlx00": -2000, "wlx01": -2500}` or NULL on alloc failure or
 * if all NICs are PP_NIC_UNKNOWN. Caller frees. */
char *pp_rxpower_build_json(const char *const *nics,
                            const pp_nic_driver_t *driver_for,
                            int pct);

/* Enumerate wlx* interfaces from /sys/class/net (path overridable via env
 * PP_GS_SYS_CLASS_NET — used in tests). Returns NULL-terminated heap array
 * of heap-allocated strings; caller frees each + array. */
char **pp_rxpower_list_wlx_nics(void);

/* Look up the NET driver name for a given iface via /sys/class/net/<if>/device/uevent
 * (parses MODALIAS / DRIVER). Returns heap string or NULL. Caller frees. */
char *pp_rxpower_nic_driver_name(const char *iface);

#ifdef __cplusplus
}
#endif
#endif
