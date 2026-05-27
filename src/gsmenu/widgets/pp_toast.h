#ifndef PP_TOAST_H
#define PP_TOAST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Display a transient error message at the bottom-center of the screen.
 * Replaces any previous toast. Auto-dismisses after ~2 seconds. Safe to
 * call from LVGL event handlers. */
void pp_toast_error(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
