#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
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
                           const char *, pp_settings_done_cb cb) {
    set_async_calls++;
    if (cb) cb(0, NULL);
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

static void test_done_cb(int rc, const char *) {
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
    pp_settings_set_async("d", "p", "k", "v", test_done_cb);
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
    pp_settings_set_async("d", "p", "k", "fallback", test_done_cb);
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
    pp_settings_set_async("a", "b", "c", "d", nullptr);
}

TEST_CASE("dispatch: set_async with no provider invokes callback with rc=-1") {
    reset_recorders();
    pp_settings_register(nullptr);
    pp_settings_set_async("a", "b", "c", "d", test_done_cb);
    REQUIRE(cb_calls == 1);
    REQUIRE(cb_last_rc == -1);
}
