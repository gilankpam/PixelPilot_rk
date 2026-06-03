#ifndef PP_ROW_H
#define PP_ROW_H
#include <lvgl.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key);

void pp_row_set_value(lv_obj_t *row, const char *value);

/* Show/hide a small spinner at the row's trailing edge, disable child
 * widget input while busy. Calls are nestable — call with the same flag
 * idempotently. Safe to call on rows that don't have a child input. */
void pp_row_set_busy(lv_obj_t *row, bool busy);

/* Mark the row as read-only. Disables input and applies a greyed style.
 * `reason` selects the lock icon (LOCK = dynamic link, OFFLINE = drone
 * unreachable). When false, restores the row to interactive state. */
typedef enum {
    PP_ROW_UNLOCKED = 0,
    PP_ROW_LOCKED_DYNAMIC = 1,
    PP_ROW_LOCKED_OFFLINE = 2,
    PP_ROW_LOCKED_UNAVAILABLE = 3,
} pp_row_lock_t;

void pp_row_set_locked(lv_obj_t *row, pp_row_lock_t state);
pp_row_lock_t pp_row_get_locked(lv_obj_t *row);

#ifdef __cplusplus
}
#endif
#endif
