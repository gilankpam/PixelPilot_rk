#ifndef PP_GSMENU_HELPER_H
#define PP_GSMENU_HELPER_H

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "ui.h"
#include "../../lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The new GSMenu UI does not use the old create / reload / generic
 * helpers. Only find_resource_file is still needed (simulator.c uses
 * it to load the fake video background image). */

const char *find_resource_file(const char *relative_path);

/* Walk a page's rows and re-apply lock/offline state via pp_row_set_locked.
 * Domain/page/key on each row are read from the row's user_data which
 * pp_toggle/pp_slider/pp_dropdown all populate with a struct whose first
 * three fields are `char *domain, *page, *key`. Rows without that shape
 * (section headers, the row's own holder child, etc.) are skipped. */
void pp_page_reapply_lock_state(lv_obj_t *page);

#ifdef __cplusplus
}
#endif

#endif
