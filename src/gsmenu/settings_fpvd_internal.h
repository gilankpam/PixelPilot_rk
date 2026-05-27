#ifndef PP_SETTINGS_FPVD_INTERNAL_H
#define PP_SETTINGS_FPVD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

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

#ifdef __cplusplus
}
#endif
#endif
