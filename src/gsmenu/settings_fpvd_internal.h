#ifndef PP_SETTINGS_FPVD_INTERNAL_H
#define PP_SETTINGS_FPVD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    const char *domain;
    const char *page;
    const char *key;
    const char *path;
    fpvd_type_t type;
} fpvd_keymap_entry_t;

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

typedef struct {
    int   status;          /* 0 = transport failure; else HTTP status */
    char *body;            /* heap-allocated, may be NULL */
    size_t body_len;
} fpvd_http_result_t;

void fpvd_http_result_free(fpvd_http_result_t *r);

fpvd_http_result_t fpvd_http_get(const char *url);
fpvd_http_result_t fpvd_http_patch_json(const char *url, const char *body);
fpvd_http_result_t fpvd_http_post(const char *url);

/* For use by settings_router only. Returns the static provider table. */
const pp_settings_provider_t *pp_fpvd_provider_for_router(void);

#ifdef __cplusplus
}
#endif
#endif
