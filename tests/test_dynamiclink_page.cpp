#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/styles.h"
#include "gsmenu/pages/dynamiclink.h"
#include "input.h"

/* pp_slider's key handler references the global control mode that normally
 * lives in input.cpp (not linked here). */
gsmenu_control_mode_t control_mode = GSMENU_CONTROL_MODE_NAV;

/* pp_page.c switches this indev's group on page enter/exit (normally from
 * src/menu.c). */
lv_indev_t *indev_drv;

static void dummy_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp);
}

static void dummy_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    data->state = LV_INDEV_STATE_RELEASED;
}
} /* extern "C" */

static lv_obj_t *setup_screen()
{
    static bool inited = false;
    if (!inited) {
        lv_init();
        static uint32_t disp_buf[240 * 240];
        lv_display_t *disp = lv_display_create(240, 240);
        lv_display_set_flush_cb(disp, dummy_flush_cb);
        lv_display_set_buffers(disp, (uint8_t *)disp_buf, NULL,
                               sizeof(disp_buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        style_init();
        pp_settings_register_stub();
        indev_drv = lv_indev_create();
        lv_indev_set_type(indev_drv, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(indev_drv, dummy_read_cb);
        inited = true;
    }
    return lv_obj_create(NULL);
}

/* Recursively search the page subtree for a label widget whose text matches
 * exactly. Row labels (and section headers) are plain lv_label children, so
 * an exact hit means that row/header is present in the built page. */
static bool subtree_has_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) &&
        strcmp(lv_label_get_text(obj), text) == 0) {
        return true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        if (subtree_has_label(lv_obj_get_child(obj, i), text)) return true;
    }
    return false;
}

TEST_CASE("Dynamic Link page drops the whole Failsafe section", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    /* Header + all four Failsafe rows are gone. */
    REQUIRE_FALSE(subtree_has_label(page, "Failsafe"));
    REQUIRE_FALSE(subtree_has_label(page, "MCS"));
    REQUIRE_FALSE(subtree_has_label(page, "FEC K"));
    REQUIRE_FALSE(subtree_has_label(page, "FEC N"));
    REQUIRE_FALSE(subtree_has_label(page, "Bitrate (kbps)"));

    lv_obj_delete(scr);
}

TEST_CASE("Dynamic Link page keeps the Compute rows", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    REQUIRE(subtree_has_label(page, "Base Redundancy"));
    REQUIRE(subtree_has_label(page, "Blocks / Frame"));
    REQUIRE(subtree_has_label(page, "Min Bitrate"));
    REQUIRE(subtree_has_label(page, "Max Bitrate"));
    REQUIRE(subtree_has_label(page, "Max MCS"));

    lv_obj_delete(scr);
}

TEST_CASE("Dynamic Link page has the Flight Log toggle", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    REQUIRE(subtree_has_label(page, "Flight Log"));

    lv_obj_delete(scr);
}
