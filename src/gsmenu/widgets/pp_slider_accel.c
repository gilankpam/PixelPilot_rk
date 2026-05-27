#include "pp_slider_accel.h"

#define PP_SLIDER_HOLD_GAP_MS 250

int32_t pp_slider_accel_step(int32_t base_step, int32_t hold_count) {
    if (hold_count <  4) return base_step;
    if (hold_count <  8) return base_step * 2;
    if (hold_count < 16) return base_step * 4;
    return base_step * 8;
}

int32_t pp_slider_accel_update(uint32_t now_ms, uint32_t last_ms,
                               int32_t prev_key, int32_t this_key,
                               int32_t prev_count) {
    if (prev_key != this_key) return 0;
    if (now_ms - last_ms >= PP_SLIDER_HOLD_GAP_MS) return 0;
    return prev_count + 1;
}
