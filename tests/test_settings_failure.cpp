#include <catch2/catch_test_macros.hpp>
#include <stdlib.h>
#include <string.h>
#include <string>

extern "C" {
#include "gsmenu/settings.h"
}

/* Callback fixture. */
struct cb_result {
    bool fired;
    int rc;
    char err_copy[128];
    bool user_data_matched;
};

static void test_cb(int rc, const char *err, void *user_data) {
    struct cb_result *r = (struct cb_result *)user_data;
    r->fired = true;
    r->rc = rc;
    if (err) {
        strncpy(r->err_copy, err, sizeof(r->err_copy) - 1);
        r->err_copy[sizeof(r->err_copy) - 1] = '\0';
    } else {
        r->err_copy[0] = '\0';
    }
    r->user_data_matched = true; /* if we got here via user_data, it matches by construction */
}

TEST_CASE("PP_SIM_FAIL makes dummy_set_async report failure and skip the write",
          "[settings][failure]") {
    pp_settings_register_dummy();

    /* Seed a known starting value. */
    pp_settings_set("gs", "wifi", "hotspot", "off");
    char *initial = pp_settings_get("gs", "wifi", "hotspot");
    REQUIRE(initial != nullptr);
    REQUIRE(std::string(initial) == "off");
    free(initial);

    /* Now turn on fail-injection. */
    setenv("PP_SIM_FAIL", "1", 1);

    struct cb_result r = {};
    pp_settings_set_async("gs", "wifi", "hotspot", "on", test_cb, &r);

    REQUIRE(r.fired);
    REQUIRE(r.rc != 0);
    REQUIRE(strlen(r.err_copy) > 0);

    /* The value must NOT have been written. */
    char *after = pp_settings_get("gs", "wifi", "hotspot");
    REQUIRE(after != nullptr);
    REQUIRE(std::string(after) == "off");
    free(after);

    unsetenv("PP_SIM_FAIL");
}

TEST_CASE("Normal dummy_set_async writes the value and reports success",
          "[settings][failure]") {
    pp_settings_register_dummy();
    unsetenv("PP_SIM_FAIL");

    pp_settings_set("gs", "wifi", "hotspot", "off");

    struct cb_result r = {};
    pp_settings_set_async("gs", "wifi", "hotspot", "on", test_cb, &r);

    REQUIRE(r.fired);
    REQUIRE(r.rc == 0);

    char *after = pp_settings_get("gs", "wifi", "hotspot");
    REQUIRE(after != nullptr);
    REQUIRE(std::string(after) == "on");
    free(after);
}
