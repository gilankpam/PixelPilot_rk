#pragma once

#include "../../lvgl/lvgl.h"

/* Types kept for compatibility with helper.h's legacy create_* APIs.
 * The new menu (src/menu.c + widgets/ + pages/) does not use these. */

typedef void (*ReloadFunc)(lv_obj_t * page, lv_obj_t * target);

typedef struct {
    const char *caption;
    lv_obj_t *target;
    ReloadFunc reload;
} PageEntry;

typedef struct {
    char type[100];
    char page[100];
    void (*page_load_callback)(lv_obj_t * page);
    lv_group_t *indev_group;
    size_t entry_count;
    PageEntry *page_entries;
} menu_page_data_t;
