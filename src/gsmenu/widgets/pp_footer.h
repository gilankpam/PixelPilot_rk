#pragma once
#include "lvgl/lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Static key-legend row: ▲▼ NAVIGATE · ▶ ENTER · ◀ BACK. */
lv_obj_t *pp_footer_create(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
