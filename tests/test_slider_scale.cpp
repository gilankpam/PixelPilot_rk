#include <catch2/catch_test_macros.hpp>
#include <string>
extern "C" {
#include "gsmenu/widgets/pp_slider_scale.h"
}

static const pp_slider_cfg_t GOP = {0, 300, 10, 1, 10, 10, 1, nullptr, PP_SER_FLOAT_DIV};
static const pp_slider_cfg_t BR  = {500, 26000, 500, 0, 0, 1000, 1, "Mbps", PP_SER_INT};

static std::string fmt(int32_t raw, const pp_slider_cfg_t& c) {
    char b[32]; pp_slider_fmt(raw, &c, b, sizeof b); return b;
}
static std::string ser(int32_t raw, const pp_slider_cfg_t& c) {
    char b[32]; pp_slider_ser(raw, &c, b, sizeof b); return b;
}

TEST_CASE("GOP fractional display", "[slider]") {
    REQUIRE(fmt(0,  GOP) == "0");
    REQUIRE(fmt(5,  GOP) == "0.5");
    REQUIRE(fmt(10, GOP) == "1");
    REQUIRE(fmt(20, GOP) == "2");
    REQUIRE(fmt(300,GOP) == "30");
}
TEST_CASE("GOP variable step is symmetric across 1.0", "[slider]") {
    REQUIRE(pp_slider_step(9,  &GOP, +1) == 1);   /* 0.9 -> 1.0 */
    REQUIRE(pp_slider_step(10, &GOP, +1) == 10);  /* 1.0 -> 2.0 */
    REQUIRE(pp_slider_step(20, &GOP, -1) == 10);  /* 2.0 -> 1.0 */
    REQUIRE(pp_slider_step(10, &GOP, -1) == 1);   /* 1.0 -> 0.9 */
}
TEST_CASE("GOP parse + serialize", "[slider]") {
    REQUIRE(pp_slider_parse("0.5", &GOP) == 5);
    REQUIRE(pp_slider_parse("2",   &GOP) == 20);
    REQUIRE(pp_slider_parse("",    &GOP) == 0);   /* empty -> raw_min */
    REQUIRE(ser(5,  GOP) == "0.5");
    REQUIRE(ser(20, GOP) == "2");
}
TEST_CASE("bitrate kbps<->Mbps display, raw wire, clamp", "[slider]") {
    REQUIRE(fmt(12500, BR) == "12.5");
    REQUIRE(fmt(26000, BR) == "26");
    REQUIRE(pp_slider_parse("12500", &BR) == 12500);
    REQUIRE(pp_slider_parse("99999", &BR) == 26000); /* clamp to max */
    REQUIRE(pp_slider_parse("100",   &BR) == 500);   /* clamp to min */
    REQUIRE(ser(12500, BR) == "12500");
    REQUIRE(pp_slider_step(12500, &BR, +1) == 500); /* uniform */
}
