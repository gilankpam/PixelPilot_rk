#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "helper.h"
#include "ui.h"
#include "settings.h"

extern lv_indev_t *indev_drv;
extern lv_group_t *default_group;

/* These globals were previously populated by the error-dialog machinery.
 * They are kept here so that helper.c (which extern-references them) links
 * cleanly. The error dialog is gone; the variables stay NULL at all times. */
lv_group_t *error_group = NULL;
lv_obj_t   *msgbox      = NULL;

void generic_switch_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    const char *value = lv_obj_has_state(target, LV_STATE_CHECKED) ? "on" : "off";
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, value, NULL);
}

void generic_checkbox_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    const char *value = lv_obj_has_state(target, LV_STATE_CHECKED) ? "on" : "off";
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, value, NULL);
}

void generic_dropdown_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    char buf[128];
    lv_dropdown_get_selected_str(target, buf, sizeof buf);
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, buf, NULL);
}

void generic_slider_event_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    int32_t v = lv_slider_get_value(target);
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)v);
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, buf, NULL);
}

/* Legacy entry points kept for source compatibility during the migration.
 * They no longer spawn threads or show spinners — the stub provider is
 * synchronous. Real persistence will be re-introduced via a real
 * pp_settings_provider implementation, not here. */

char *run_command(const char *command) {
    LV_LOG_USER("run_command (no-op): %s", command);
    char *out = (char *)malloc(1);
    if (out) out[0] = '\0';
    return out;
}

void run_command_and_block(lv_event_t *e, const char *command,
                           callback_fn callback) {
    (void)e;
    LV_LOG_USER("run_command_and_block (no-op): %s", command);
    if (callback) callback();
}
