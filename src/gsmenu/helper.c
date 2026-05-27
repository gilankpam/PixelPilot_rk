#include <stdio.h>
#include <stdbool.h>
#include "../../lvgl/lvgl.h"
#include "helper.h"

/* Minimal helper retained after the GSMenu redesign. The simulator uses
 * find_resource_file() to locate icon/background assets via LVGL's FS API.
 * All the legacy create / reload / generic helpers were removed along
 * with the old air and gs page builders. */

static bool file_exists(const char *path) {
    lv_fs_file_t f;
    lv_fs_res_t res = lv_fs_open(&f, path, LV_FS_MODE_RD);
    if (res == LV_FS_RES_OK) {
        lv_fs_close(&f);
        return true;
    }
    return false;
}

const char *find_resource_file(const char *relative_path) {
    static char path[256];

    const char *prefixes[] = {
        "/usr/local/share/pixelpilot",
        "/usr/share/pixelpilot",
        "./src/icons",
    };

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "A:%s/%s", prefixes[i], relative_path);
        if (file_exists(path)) return path;
    }
    return NULL;
}
