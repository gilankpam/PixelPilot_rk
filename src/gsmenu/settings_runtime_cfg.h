#ifndef PP_SETTINGS_RUNTIME_CFG_H
#define PP_SETTINGS_RUNTIME_CFG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The six hot-reloadable settings, in menu-scale units. */
typedef struct {
    int dvr_mode;        /* 0=raw, 1=reencode, 2=both */
    int dvr_max_size_mb; /* megabytes */
    int dvr_reenc_kbps;  /* kbps */
    int cc_enabled;      /* 0/1 */
    int cc_gain;         /* 0..50   (shader gain   = cc_gain / 10.0)  */
    int cc_offset;       /* -50..50 (shader offset = cc_offset / 100.0) */
} pp_runtime_cfg_t;

/* Apply backend. The device build registers real functions; the simulator and
 * host tests leave it NULL so calls become no-ops and is_recording reads 0. */
typedef struct {
    void (*dvr_set_mode)(int mode);
    void (*dvr_set_max_size)(int mb);
    void (*dvr_reenc_set_bitrate)(int kbps);
    void (*colortrans_apply)(int enabled, float gain, float offset);
    int  (*is_recording)(void);
} pp_runtime_cfg_ops_t;

/* Override the JSON path (default "/etc/pixelpilot/runtime.json"). */
void pp_runtime_cfg_set_path(const char *path);

/* Fill *out with the built-in defaults. */
void pp_runtime_cfg_defaults(pp_runtime_cfg_t *out);

/* Read the JSON file into *out (and prime the internal cache). Missing file or
 * fields fall back to defaults. Returns true if the file existed and parsed. */
bool pp_runtime_cfg_load(pp_runtime_cfg_t *out);

/* Register apply ops (NULL clears). */
void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops);

/* True if (domain,page,key) is one of the six runtime-config keys. */
bool pp_runtime_cfg_owns(const char *domain, const char *page, const char *key);

/* Heap string (caller free()s) of the current value for a runtime key, or NULL
 * if not owned. Strings match the widget value formats. */
char *pp_runtime_cfg_get(const char *domain, const char *page, const char *key);

/* Apply (via ops) + persist (atomic JSON write) a runtime key. No-op if not owned. */
void pp_runtime_cfg_set(const char *domain, const char *page,
                        const char *key, const char *value);

/* True while DVR is recording (ops->is_recording); false if ops unregistered. */
bool pp_runtime_cfg_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif /* PP_SETTINGS_RUNTIME_CFG_H */
