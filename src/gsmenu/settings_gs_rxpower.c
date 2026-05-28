/* src/gsmenu/settings_gs_rxpower.c */
#include "settings_gs_rxpower.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pp_nic_driver_t pp_nic_driver_from_name(const char *name) {
    if (!name) return PP_NIC_UNKNOWN;
    if (strcmp(name, "rtl88xxau_wfb") == 0) return PP_NIC_RTL88XXAU_WFB;
    if (strcmp(name, "rtl88x2eu")     == 0) return PP_NIC_RTL88X2EU;
    return PP_NIC_UNKNOWN;
}

int pp_rxpower_pct_to_driver_value(pp_nic_driver_t drv, int pct, int *out) {
    if (!out) return 0;
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    int min_v, max_v;
    switch (drv) {
    case PP_NIC_RTL88XXAU_WFB: min_v = -1000; max_v = -3000; break;
    case PP_NIC_RTL88X2EU:     min_v =  1000; max_v =  2900; break;
    default: *out = 0; return 0;
    }
    int range = max_v - min_v;          /* signed; can be negative */
    /* Old script: position = pct - 1 (so pct=1 means position=0). But the
     * original arithmetic actually used position = pct and range/100. We
     * mirror that: value = min + (pct * range) / 100. */
    *out = min_v + (pct * range) / 100;
    return 1;
}

char *pp_rxpower_build_json(const char *const *nics,
                            const pp_nic_driver_t *drv,
                            int pct) {
    if (!nics) return NULL;
    char buf[1024];
    size_t off = 0;
    buf[off++] = '{';
    bool first = true;
    for (size_t i = 0; nics[i]; i++) {
        int v;
        if (!pp_rxpower_pct_to_driver_value(drv[i], pct, &v)) continue;
        int wrote = snprintf(buf + off, sizeof buf - off,
                             "%s\"%s\": %d", first ? "" : ", ", nics[i], v);
        if (wrote < 0 || (size_t)wrote >= sizeof buf - off) return NULL;
        off += (size_t)wrote;
        first = false;
    }
    if (first) return NULL;             /* nothing written */
    if (off + 2 > sizeof buf) return NULL;
    buf[off++] = '}';
    buf[off]   = '\0';
    return strdup(buf);
}

/* Enumeration: walk /sys/class/net and pick names starting with "wlx".
 * Root path overridable via PP_GS_SYS_CLASS_NET for tests. */
char **pp_rxpower_list_wlx_nics(void) {
    const char *root = getenv("PP_GS_SYS_CLASS_NET");
    if (!root) root = "/sys/class/net";
    DIR *d = opendir(root);
    if (!d) return NULL;
    char **out = (char **)calloc(16, sizeof(char *));
    if (!out) { closedir(d); return NULL; }
    size_t cap = 16, n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "wlx", 3) != 0) continue;
        if (n + 1 >= cap) {
            cap *= 2;
            char **nx = (char **)realloc(out, cap * sizeof(char *));
            if (!nx) break;
            out = nx;
        }
        out[n++] = strdup(de->d_name);
    }
    out[n] = NULL;
    closedir(d);
    return out;
}

/* Read /sys/class/net/<iface>/device/uevent — DRIVER=<name>. */
char *pp_rxpower_nic_driver_name(const char *iface) {
    if (!iface) return NULL;
    const char *root = getenv("PP_GS_SYS_CLASS_NET");
    if (!root) root = "/sys/class/net";
    char path[512];
    snprintf(path, sizeof path, "%s/%s/device/uevent", root, iface);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[256];
    char *out = NULL;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "DRIVER=", 7) == 0) {
            size_t l = strlen(line);
            if (l && line[l-1] == '\n') line[l-1] = '\0';
            out = strdup(line + 7);
            break;
        }
    }
    fclose(f);
    return out;
}
