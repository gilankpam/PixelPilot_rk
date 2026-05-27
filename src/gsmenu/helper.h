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

#ifdef __cplusplus
}
#endif

#endif
