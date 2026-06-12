#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/styles.h"
#include "gsmenu/helper.h"
#include "gsmenu/widgets/pp_page.h"
#include "gsmenu/widgets/pp_tabbar.h"
#include "gsmenu/widgets/pp_toggle.h"

/* Normally defined in src/menu.c (not linked here). pp_tabbar.c and
 * pp_page.c switch this indev's group on page enter/exit. */
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

/* LVGL's keypad indev only delivers keys to the group's focused object,
 * and only when that object is enabled (lv_indev.c). The page exit path
 * (HOME bubbling from the focused row to pp_page's handler) therefore
 * dies the moment lock state disables every row in the page group — the
 * exact situation on the all-"air" Camera page when the drone is off.
 * These tests pin the invariant that the indev never ends up parked on a
 * group whose focused object cannot receive keys. */

/* Provider with switchable connectivity, mimicking the fpvd backend when
 * the drone is powered off: every "air" row locks PP_ROW_LOCKED_OFFLINE. */
static bool g_connected = true;
static bool g_air_reachable = true;

static void prov_set(const char *, const char *, const char *, const char *) {}
static char *prov_get(const char *, const char *, const char *) { return NULL; }
static void prov_set_async(const char *, const char *, const char *,
                           const char *, pp_settings_done_cb on_done,
                           void *user_data)
{
    if (on_done) on_done(0, NULL, user_data);
}
static bool prov_is_connected(void) { return g_connected; }
static bool prov_is_reachable(const char *domain, const char *, const char *)
{
    if (domain && strcmp(domain, "air") == 0) return g_air_reachable;
    return true;
}

static const pp_settings_provider_t test_provider = {
    prov_set,
    prov_get,
    prov_set_async,
    NULL,               /* is_locked */
    prov_is_connected,
    prov_is_reachable,
    NULL,               /* set_snapshot_listener */
    NULL,               /* set_visibility */
    NULL,               /* is_available */
    NULL,               /* apply */
    NULL,               /* has_pending */
};

static void setup_lvgl()
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
        pp_settings_register(&test_provider);
        indev_drv = lv_indev_create();
        lv_indev_set_type(indev_drv, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(indev_drv, dummy_read_cb);
        inited = true;
    }
    g_connected = true;
    g_air_reachable = true;
}

struct menu_fixture {
    lv_obj_t    *scr;
    lv_obj_t    *page;
    pp_tabbar_t *tabbar;
    lv_obj_t    *tab;
};

/* One-tab menu wired like src/menu.c: rows joined to the page group,
 * back_group set to the tabbar group, indev parked on the tabbar. */
static menu_fixture build_menu(int n_air_rows, int n_gs_rows)
{
    setup_lvgl();
    menu_fixture f;
    f.scr  = lv_obj_create(NULL);
    f.page = pp_page_create(f.scr, "air", "camera");
    lv_obj_set_size(f.page, LV_PCT(100), LV_PCT(100));

    char key[16];
    for (int i = 0; i < n_air_rows; i++) {
        snprintf(key, sizeof(key), "a%d", i);
        lv_obj_t *row = pp_toggle(f.page, NULL, "Air row",
                                  "air", "camera", key);
        lv_group_add_obj(pp_page_group(f.page), row);
    }
    for (int i = 0; i < n_gs_rows; i++) {
        snprintf(key, sizeof(key), "g%d", i);
        lv_obj_t *row = pp_toggle(f.page, NULL, "GS row",
                                  "gs", "link", key);
        lv_group_add_obj(pp_page_group(f.page), row);
    }

    pp_tabbar_item_t item = { "Camera", NULL, f.page };
    f.tabbar = pp_tabbar_create(f.scr, &item, 1);
    pp_page_set_back_group(f.page, pp_tabbar_group(f.tabbar));
    lv_indev_set_group(indev_drv, pp_tabbar_group(f.tabbar));
    f.tab = lv_obj_get_child(pp_tabbar_root(f.tabbar), 0);
    return f;
}

static void teardown_menu(menu_fixture &f)
{
    lv_indev_set_group(indev_drv, NULL);
    lv_obj_delete(f.scr);
}

static void send_key(lv_obj_t *obj, uint32_t key)
{
    lv_obj_send_event(obj, LV_EVENT_KEY, &key);
}

TEST_CASE("ENTER on a tab refuses entry when every row is locked offline",
          "[pagenav]") {
    menu_fixture f = build_menu(3, 0);

    g_connected = false;
    pp_page_reapply_lock_state(f.page);

    send_key(f.tab, LV_KEY_ENTER);

    /* Entering would strand the user: a fully-disabled group swallows
     * every key, including HOME (back). Focus must stay on the tabbar. */
    REQUIRE(lv_indev_get_group(indev_drv) == pp_tabbar_group(f.tabbar));

    teardown_menu(f);
}

TEST_CASE("drone loss while inside an all-air page returns focus to the tabbar",
          "[pagenav]") {
    menu_fixture f = build_menu(3, 0);

    send_key(f.tab, LV_KEY_ENTER);
    REQUIRE(lv_indev_get_group(indev_drv) == pp_page_group(f.page));
    lv_obj_t *focused = lv_group_get_focused(pp_page_group(f.page));
    REQUIRE(focused != NULL);

    /* The fpvd snapshot listener runs this when connectivity flips. */
    g_connected = false;
    pp_page_reapply_lock_state(f.page);

    REQUIRE(lv_indev_get_group(indev_drv) == pp_tabbar_group(f.tabbar));
    REQUIRE(!lv_obj_has_state(focused, LV_STATE_FOCUS_KEY));

    teardown_menu(f);
}

TEST_CASE("entering a page with a stale-locked focused row lands on an enabled row",
          "[pagenav]") {
    /* Air row first: it grabs the group's initial focus at build time,
     * then locks when the drone becomes unreachable — a stale focus. */
    menu_fixture f = build_menu(1, 1);

    g_air_reachable = false;
    pp_page_reapply_lock_state(f.page);

    send_key(f.tab, LV_KEY_ENTER);

    REQUIRE(lv_indev_get_group(indev_drv) == pp_page_group(f.page));
    lv_obj_t *focused = lv_group_get_focused(pp_page_group(f.page));
    REQUIRE(focused != NULL);
    REQUIRE(!lv_obj_has_state(focused, LV_STATE_DISABLED));

    teardown_menu(f);
}

TEST_CASE("HOME from an enabled row still returns to the tabbar", "[pagenav]") {
    menu_fixture f = build_menu(2, 0);

    send_key(f.tab, LV_KEY_ENTER);
    REQUIRE(lv_indev_get_group(indev_drv) == pp_page_group(f.page));

    lv_obj_t *focused = lv_group_get_focused(pp_page_group(f.page));
    REQUIRE(focused != NULL);
    send_key(focused, LV_KEY_HOME);

    REQUIRE(lv_indev_get_group(indev_drv) == pp_tabbar_group(f.tabbar));

    teardown_menu(f);
}
