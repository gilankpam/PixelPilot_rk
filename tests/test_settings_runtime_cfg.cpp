#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>   // rand, free, remove, atoi
#include <cstring>   // strlen
#include <string>
extern "C" {
#include "gsmenu/settings_runtime_cfg.h"
}

static std::string write_tmp(const char *body) {
    std::string path = std::string("/tmp/pp_rtcfg_test_") + std::to_string(::rand()) + ".json";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    return path;
}

TEST_CASE("defaults are the documented built-ins") {
    pp_runtime_cfg_t c;
    pp_runtime_cfg_defaults(&c);
    REQUIRE(c.dvr_mode == 0);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    REQUIRE(c.cc_enabled == 0);
    REQUIRE(c.cc_gain == 25);
    REQUIRE(c.cc_offset == -15);
}

TEST_CASE("load missing file yields defaults and returns false") {
    pp_runtime_cfg_set_path("/tmp/pp_rtcfg_does_not_exist_12345.json");
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.cc_gain == 25);
}

TEST_CASE("load reads all fields") {
    std::string p = write_tmp(
        "{\"dvr\":{\"mode\":\"both\",\"maxSizeMb\":8000,\"reencBitrateKbps\":12000},"
        "\"colorCorrection\":{\"enabled\":true,\"gain\":30,\"offset\":-20}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 2);
    REQUIRE(c.dvr_max_size_mb == 8000);
    REQUIRE(c.dvr_reenc_kbps == 12000);
    REQUIRE(c.cc_enabled == 1);
    REQUIRE(c.cc_gain == 30);
    REQUIRE(c.cc_offset == -20);
    remove(p.c_str());
}

TEST_CASE("load tolerates partial json — absent fields keep defaults") {
    std::string p = write_tmp("{\"dvr\":{\"mode\":\"reencode\"}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 1);
    REQUIRE(c.dvr_max_size_mb == 4000);   /* default */
    REQUIRE(c.cc_gain == 25);             /* default */
    remove(p.c_str());
}

TEST_CASE("load on malformed json returns false and fills defaults") {
    std::string p = write_tmp("{ this is not json ");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    remove(p.c_str());
}
