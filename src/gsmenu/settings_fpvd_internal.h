#ifndef PP_SETTINGS_FPVD_INTERNAL_H
#define PP_SETTINGS_FPVD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "settings.h"
#include "conn_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* React to a drone-link state transition reported by conn_state: when the link
 * comes up, kick an immediate /air/config refresh (so air_snapshot is fresh
 * without waiting for the worker's idle poll, which is up to 60s while the menu
 * is hidden) and re-lock the menu rows. Production wires this via
 * conn_state_subscribe; exposed here so it is unit-testable. */
void fpvd_on_conn_state_change(const conn_state_t *st);

typedef enum {
    FPVD_T_INT,
    FPVD_T_FLOAT,
    FPVD_T_BOOL,
    FPVD_T_STRING,
    FPVD_T_ENUM,
    FPVD_T_BITRATE_KBPS,    /* UI string "15M" ↔ JSON int 15000 */
    FPVD_T_SECONDS_FROM_MIN, /* UI int minutes ↔ JSON int seconds */
    FPVD_T_PERCENT_TO_FRAC,  /* UI int 0..100 ↔ JSON float 0.0..1.0 */
} fpvd_type_t;

typedef enum {
    FPVD_EP_AIR, /* drone proxy: /air/config + /air/apply,  GET /air/config */
    FPVD_EP_GS,  /* GS tree:     /gs/config  + /gs/apply,   GET /gs/config?pending=true */
} fpvd_endpoint_t;

typedef enum {
    FPVD_ROW_PLAIN,    /* PATCH + apply on the row's endpoint */
    FPVD_ROW_STAGED,   /* PATCH /gs/config only; explicit Apply commits */
    FPVD_ROW_SHARED,   /* drone-first cross-device orchestration (channel, width) */
    FPVD_ROW_BEAMFORM, /* beamforming MAC handshake (drone + GS) */
    FPVD_ROW_DLINK,    /* adaptive-link arm/disarm: drone-first, reject if drone down */
} fpvd_row_kind_t;

typedef struct {
    const char     *domain;
    const char     *page;
    const char     *key;
    const char     *path;
    fpvd_type_t     type;
    fpvd_endpoint_t endpoint;
    fpvd_row_kind_t kind;
} fpvd_keymap_entry_t;

/* Endpoint → URL path. Pure; never NULL.
 * FPVD_EP_AIR: /air/config, /air/apply, /air/config
 * FPVD_EP_GS:  /gs/config,  /gs/apply,  /gs/config?pending=true */
const char *fpvd_write_path(fpvd_endpoint_t ep);
const char *fpvd_apply_path(fpvd_endpoint_t ep);
const char *fpvd_read_path (fpvd_endpoint_t ep);

const fpvd_keymap_entry_t *fpvd_keymap_lookup(const char *domain,
                                              const char *page,
                                              const char *key);

/* Iterate all entries (returns NULL on end). i starts at 0. */
const fpvd_keymap_entry_t *fpvd_keymap_at(size_t i);

/* Walk `root` along the dotted `path`, format the leaf value as a string
 * according to `type`. Returns a heap-allocated string. Returns strdup("")
 * if path is missing or types are incompatible. Never returns NULL. */
char *fpvd_snapshot_read_string(cJSON *root, const char *path, fpvd_type_t type);

/* Build a sparse nested cJSON object for a single field. Returns NULL on
 * value parse error. Caller frees with cJSON_Delete. */
cJSON *fpvd_build_patch_body(const char *path, const char *value, fpvd_type_t type);

/* Returns true iff `path` is an exact match or subtree child of any locked
 * JSON path (i.e. the paths owned by fpvd's dl-applier when dynamicLink is
 * active). */
bool fpvd_is_locked_path(const char *path);

#define FPVD_PLAN_MAX 6

typedef struct {
    char method[8];    /* "PATCH" | "POST" */
    char url_path[28]; /* e.g. "/gs/config" */
    char body[256];    /* "" => no body */
    int  retries;      /* extra attempts after first failure (0 = single try) */
    bool gs_side;      /* step targets the GS tree (for error wording + dirty flag) */
} fpvd_step_t;

/* Plan the HTTP steps for one settings write. Pure (no HTTP, no globals).
 * gs_local_mac may be NULL (only needed for FPVD_ROW_BEAMFORM enable).
 * Returns the number of steps written to out, or -1 with a message in err. */
int fpvd_plan_steps(fpvd_row_kind_t kind, fpvd_endpoint_t ep,
                    const char *path, fpvd_type_t type, const char *value,
                    bool drone_reachable, const char *gs_local_mac,
                    fpvd_step_t *out, size_t max,
                    char *err, size_t errn);

typedef struct {
    int   status;          /* 0 = transport failure; else HTTP status */
    char *body;            /* heap-allocated, may be NULL */
    size_t body_len;
} fpvd_http_result_t;

void fpvd_http_result_free(fpvd_http_result_t *r);

fpvd_http_result_t fpvd_http_get(const char *url);
fpvd_http_result_t fpvd_http_patch_json(const char *url, const char *body);
fpvd_http_result_t fpvd_http_post(const char *url);
fpvd_http_result_t fpvd_http_post_json(const char *url, const char *body);

#ifdef __cplusplus
}
#endif
#endif
