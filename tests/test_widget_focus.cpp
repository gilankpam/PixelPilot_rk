#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/styles.h"
#include "gsmenu/widgets/pp_dropdown.h"
#include "gsmenu/widgets/pp_toggle.h"
#include "../src/input.h"

/* pp_dropdown's key handler references the global control mode that
 * normally lives in input.cpp (not linked here). */
gsmenu_control_mode_t control_mode = GSMENU_CONTROL_MODE_NAV;

static void dummy_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp);
}
} /* extern "C" */

/* LVGL auto-adds widgets whose class has group_def=TRUE (lv_dropdown,
 * lv_switch) to the default group at creation. pp_dropdown and pp_toggle
 * drive their native child widgets from the row's key handler, so those
 * children must never be group-focusable: a focusable hidden lv_dropdown
 * receiving ENTER opens its unstyled native list over whatever screen is
 * visible, and a focusable lv_switch would apply a setting invisibly. */

static lv_obj_t *setup_screen() {
    static bool inited = false;
    if (!inited) {
        lv_init();
        static uint32_t disp_buf_aligned[240 * 240];
        lv_display_t * disp = lv_display_create(240, 240);
        lv_display_set_flush_cb(disp, dummy_flush_cb);
        lv_display_set_buffers(disp, (uint8_t *)disp_buf_aligned, NULL,
                               sizeof(disp_buf_aligned), LV_DISPLAY_RENDER_MODE_PARTIAL);
        style_init();
        pp_settings_register_stub();
        inited = true;
    }
    return lv_obj_create(NULL);
}

TEST_CASE("pp_dropdown's native dropdown never joins the default group", "[focus]") {
    lv_obj_t *scr = setup_screen();
    lv_group_t *def = lv_group_create();
    lv_group_set_default(def);

    pp_dropdown(scr, NULL, "Resolution", "air", "camera", "size",
                "1920x1080\n1280x720\n960x540");

    REQUIRE(lv_group_get_obj_count(def) == 0);

    lv_group_set_default(NULL);
    lv_group_delete(def);
    lv_obj_delete(scr);
}

TEST_CASE("pp_toggle's native switch never joins the default group", "[focus]") {
    lv_obj_t *scr = setup_screen();
    lv_group_t *def = lv_group_create();
    lv_group_set_default(def);

    pp_toggle(scr, NULL, "Beamforming", "gs", "wfbng", "beamforming");

    REQUIRE(lv_group_get_obj_count(def) == 0);

    lv_group_set_default(NULL);
    lv_group_delete(def);
    lv_obj_delete(scr);
}
