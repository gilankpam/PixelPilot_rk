#ifndef PP_SLIDER_ACCEL_H
#define PP_SLIDER_ACCEL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Pure helpers for slider hold-to-accelerate. Tested in isolation; the
 * pp_slider widget calls them on each UP/DOWN key event in EDIT mode. */

/* Scale `base_step` by the current `hold_count`. Returns the effective
 * step for this key press. Caps at 8× to prevent runaway. */
int32_t pp_slider_accel_step(int32_t base_step, int32_t hold_count);

/* Update the hold counter for the next event. Resets to 0 if too much
 * time has elapsed since the last event (>= 250 ms) or the key code
 * changed. Otherwise returns prev_count + 1. */
int32_t pp_slider_accel_update(uint32_t now_ms, uint32_t last_ms,
                               int32_t prev_key, int32_t this_key,
                               int32_t prev_count);

#ifdef __cplusplus
}
#endif
#endif
