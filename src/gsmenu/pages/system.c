#include "system.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"
#include "../widgets/pp_toast.h"
#include "../settings.h"

static void action_done_cb(int rc, const char *err) {
    if (rc != 0) pp_toast_error(err ? err : "Failed to run action");
}

/* Keys mirror originals in gs_main.c, gs_wifi.c, gs_system.c (Receiver
 * section), air_telemetry.c, air_actions.c, gs_actions.c. */

static void build_wifi_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Networks");
    pp_row_text(body, LV_SYMBOL_WIFI,
                "(scan unavailable — stub backend)", NULL);
}

static void on_open_wifi(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "WiFi", build_wifi_drilldown, NULL);
}

static void on_action(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    const char *cmd = lv_event_get_user_data(e);
    pp_settings_set_async("system", "actions", cmd, "trigger", action_done_cb);
}

lv_obj_t *build_system_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "system");

    pp_section_header(page, "Info");
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Version",  "Version");
    pp_row_text(page, LV_SYMBOL_SD_CARD,  "Disk",     "Disk");
    pp_row_text(page, LV_SYMBOL_WIFI,     "Channel",  "Channel");
    pp_row_text(page, LV_SYMBOL_EYE_OPEN, "HDMI-OUT", "HDMI-OUT");
    pp_row_text(page, LV_SYMBOL_WIFI,     "WFB-NG NICs", "WFB_NICS");

    pp_section_header(page, "Network");
    lv_obj_t *wifi_row = pp_row_text(page, LV_SYMBOL_WIFI,
                                     "WiFi networks…", NULL);
    lv_obj_add_event_cb(wifi_row, on_open_wifi, LV_EVENT_KEY, NULL);
    pp_toggle(page, LV_SYMBOL_WIFI,   "Hotspot",  "gs", "wifi", "hotspot");
    pp_toggle(page, LV_SYMBOL_UPLOAD, "Restream", "gs", "wifi", "restream");

    pp_section_header(page, "Telemetry");
    pp_dropdown(page, LV_SYMBOL_DOWNLOAD, "Serial Port",
                "air", "telemetry", "serial",
                "ttyS0\nttyS1\nttyS2\ndisabled");
    pp_dropdown(page, LV_SYMBOL_DOWNLOAD, "Router",
                "air", "telemetry", "router",
                "off\nmavfwd\nmsp");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "OSD FPS",
              "air", "telemetry", "osd_fps", 0, 60);
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "GS Rendering",
              "air", "telemetry", "gs_rendering");

    pp_section_header(page, "Actions");
    lv_obj_t *r;
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot air", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"reboot_air");
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot GS", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"reboot_gs");
    r = pp_row_text(page, LV_SYMBOL_TRASH, "Factory reset air", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"factory_reset_air");
    r = pp_row_text(page, LV_SYMBOL_TRASH, "Factory reset GS", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"factory_reset_gs");

    /* Add focusable rows to the page's group. */
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }
    return page;
}
