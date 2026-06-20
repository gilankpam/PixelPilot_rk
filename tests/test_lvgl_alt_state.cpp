#include <catch2/catch_test_macros.hpp>
#include <cstdint>

extern "C" {
#include "lvgl/lvgl.h"

/* Minimal headless flush callback required so LVGL has somewhere to
 * send rendered frames; immediately signals flush-ready without doing
 * any real I/O. */
static void dummy_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp);
}
} /* extern "C" */

TEST_CASE("LV_STATE_ALT can be set and cleared on a GSMenu panel", "[gsmenu][styles]") {
    lv_init();

    /* Allocate display buffer with uint32_t to guarantee the 4-byte
     * alignment required by lv_display_set_buffers (LV_DRAW_BUF_ALIGN=4).
     * We never actually render — the dummy flush_cb is a no-op. */
    static uint32_t disp_buf_aligned[240 * 240];
    uint8_t * disp_buf = (uint8_t *)disp_buf_aligned;
    const uint32_t buf_size = sizeof(disp_buf_aligned);

    lv_display_t * disp = lv_display_create(240, 240);
    lv_display_set_flush_cb(disp, dummy_flush_cb);
    lv_display_set_buffers(disp, disp_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Create a screen and a child obj that styles would be applied to.
     * We don't call style_init() because that pulls in font loading and
     * many other dependencies — we only need to assert state mechanics. */
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_t * panel = lv_obj_create(screen);

    REQUIRE((lv_obj_get_state(panel) & LV_STATE_ALT) == 0);

    lv_obj_add_state(panel, LV_STATE_ALT);
    REQUIRE((lv_obj_get_state(panel) & LV_STATE_ALT) != 0);

    lv_obj_remove_state(panel, LV_STATE_ALT);
    REQUIRE((lv_obj_get_state(panel) & LV_STATE_ALT) == 0);

    lv_obj_delete(screen);
    lv_display_delete(disp);
    lv_deinit();
}
