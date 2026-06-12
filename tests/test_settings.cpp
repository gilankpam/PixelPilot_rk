#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "gsmenu/settings.h"
}

static int   set_calls = 0;
static int   set_async_calls = 0;
static int   cb_calls = 0;
static int   cb_last_rc = 0;
static char  last_v[64];

static void  rec_set(const char *, const char *, const char *, const char *v) {
    set_calls++;
    snprintf(last_v, sizeof last_v, "%s", v ? v : "");
}
static char *rec_get(const char *d, const char *p, const char *k) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s/%s/%s", d, p, k);
    return strdup(buf);
}
static void  rec_set_async(const char *, const char *, const char *,
                           const char *, pp_settings_done_cb cb, void *ud) {
    set_async_calls++;
    if (cb) cb(0, NULL, ud);
}

static const pp_settings_provider_t rec_provider = {
    rec_set, rec_get, rec_set_async,
};

static void reset_recorders() {
    set_calls = 0;
    set_async_calls = 0;
    cb_calls = 0;
    cb_last_rc = 0;
    last_v[0] = '\0';
}

static void test_done_cb(int rc, const char *, void *) {
    cb_calls++;
    cb_last_rc = rc;
}

TEST_CASE("dispatch: set forwards to provider") {
    reset_recorders();
    pp_settings_register(&rec_provider);
    pp_settings_set("air", "camera", "bitrate", "25");
    REQUIRE(set_calls == 1);
    REQUIRE(std::strcmp(last_v, "25") == 0);
}

TEST_CASE("dispatch: get returns caller-owned string") {
    reset_recorders();
    pp_settings_register(&rec_provider);
    char *v = pp_settings_get("a", "b", "c");
    REQUIRE(v != nullptr);
    REQUIRE(std::strcmp(v, "a/b/c") == 0);
    free(v);
}

TEST_CASE("dispatch: set_async forwards") {
    reset_recorders();
    pp_settings_register(&rec_provider);
    pp_settings_set_async("d", "p", "k", "v", test_done_cb, NULL);
    REQUIRE(set_async_calls == 1);
    REQUIRE(cb_calls == 1);
    REQUIRE(cb_last_rc == 0);
}

TEST_CASE("dispatch: set_async falls back to sync set when async absent") {
    reset_recorders();
    static const pp_settings_provider_t sync_only = {
        rec_set, rec_get, /* set_async */ nullptr,
    };
    pp_settings_register(&sync_only);
    pp_settings_set_async("d", "p", "k", "fallback", test_done_cb, NULL);
    REQUIRE(set_calls == 1);
    REQUIRE(std::strcmp(last_v, "fallback") == 0);
    REQUIRE(cb_calls == 1);
    REQUIRE(cb_last_rc == 0);
}

TEST_CASE("dispatch: no provider registered is safe") {
    reset_recorders();
    pp_settings_register(nullptr);
    pp_settings_set("a", "b", "c", "d");          /* no crash */
    REQUIRE(pp_settings_get("a", "b", "c") == nullptr);
    pp_settings_set_async("a", "b", "c", "d", nullptr, nullptr);
}

TEST_CASE("dispatch: set_async with no provider invokes callback with rc=-1") {
    reset_recorders();
    pp_settings_register(nullptr);
    pp_settings_set_async("a", "b", "c", "d", test_done_cb, NULL);
    REQUIRE(cb_calls == 1);
    REQUIRE(cb_last_rc == -1);
}

TEST_CASE("dispatch: is_locked returns false when provider lacks it") {
    pp_settings_register(&rec_provider);
    REQUIRE(pp_settings_is_locked("a", "b", "c") == false);
}

TEST_CASE("dispatch: is_connected returns true when provider lacks it") {
    pp_settings_register(&rec_provider);
    REQUIRE(pp_settings_is_connected() == true);
}

TEST_CASE("dispatch: set_visibility is a no-op when provider lacks it") {
    pp_settings_register(&rec_provider);
    pp_settings_set_visibility(true);
    pp_settings_set_visibility(false);
    /* No crash. */
    REQUIRE(true);
}

TEST_CASE("dispatch: set_snapshot_listener is a no-op when provider lacks it") {
    pp_settings_register(&rec_provider);
    pp_settings_set_snapshot_listener(nullptr, nullptr);
    REQUIRE(true);
}

TEST_CASE("dispatch: forwards is_locked / is_connected when provider has them") {
    static bool locked_called = false;
    static bool connected_called = false;
    static auto _is_locked = +[](const char *, const char *, const char *) -> bool {
        locked_called = true; return true;
    };
    static auto _is_connected = +[]() -> bool {
        connected_called = true; return false;
    };
    static const pp_settings_provider_t full = {
        .set = rec_set, .get = rec_get, .set_async = rec_set_async,
        .is_locked = _is_locked, .is_connected = _is_connected,
        .set_snapshot_listener = nullptr, .set_visibility = nullptr,
    };
    pp_settings_register(&full);
    REQUIRE(pp_settings_is_locked("x", "y", "z") == true);
    REQUIRE(locked_called == true);
    REQUIRE(pp_settings_is_connected() == false);
    REQUIRE(connected_called == true);
}

TEST_CASE("provider wrappers return safe defaults when methods are absent", "[settings][caps]") {
    pp_settings_register_stub();                 // stub omits the new methods
    REQUIRE(pp_settings_is_available("x","y","z") == true);   // default available
    REQUIRE(pp_settings_has_pending() == false);             // default no pending
    int rc = 99;
    pp_settings_apply([](int r, const char*, void* ud){ *(int*)ud = r; }, &rc);
    REQUIRE(rc == -1);                            // no apply method → error callback
}

TEST_CASE("dispatch: is_reachable defaults to true without provider method", "[settings][caps]") {
    pp_settings_register_stub();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);
}

TEST_CASE("dummy: txpower seeds are dBm and beamforming row exists", "[dummy]") {
    pp_settings_register_dummy();
    char *v = pp_settings_get("gs", "wfbng", "txpower");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "20"); free(v);
    v = pp_settings_get("gs", "link", "rx_power");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "20"); free(v);
    v = pp_settings_get("gs", "link", "beamforming");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "off"); free(v);
}

TEST_CASE("dummy: PP_SIM_DRONE_OFFLINE gates air rows and beamforming", "[dummy]") {
    setenv("PP_SIM_DRONE_OFFLINE", "1", 1);
    pp_settings_register_dummy();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);
    REQUIRE(pp_settings_is_reachable("gs", "link", "beamforming") == false);
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "txpower") == false); /* drone TX power */
    /* GS rows stay reachable — including shared channel/bandwidth. */
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "gs_channel") == true);
    REQUIRE(pp_settings_is_reachable("gs", "link", "rx_power") == true);
    unsetenv("PP_SIM_DRONE_OFFLINE");
    pp_settings_register_dummy();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);
}
