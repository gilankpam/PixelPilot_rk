/* tests/test_settings_gs_rxpower.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "gsmenu/settings_gs_rxpower.h"
}

TEST_CASE("rxpower: driver name -> enum", "[gs][rxpower]") {
    REQUIRE(pp_nic_driver_from_name("rtl88xxau_wfb") == PP_NIC_RTL88XXAU_WFB);
    REQUIRE(pp_nic_driver_from_name("rtl88x2eu")     == PP_NIC_RTL88X2EU);
    REQUIRE(pp_nic_driver_from_name("ath9k")         == PP_NIC_UNKNOWN);
    REQUIRE(pp_nic_driver_from_name(NULL)            == PP_NIC_UNKNOWN);
}

TEST_CASE("rxpower: rtl88xxau_wfb maps inverted range (-1000..-3000)", "[gs][rxpower]") {
    int v = 0;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 1,   &v) == 1);
    REQUIRE(v == -1020);   /* min_phy + (1/100 * (-2000)) */
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 50,  &v) == 1);
    REQUIRE(v == -2000);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 100, &v) == 1);
    REQUIRE(v == -3000);
}

TEST_CASE("rxpower: rtl88x2eu maps 1000..2900", "[gs][rxpower]") {
    int v = 0;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 1,   &v) == 1);
    REQUIRE(v == 1019);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 50,  &v) == 1);
    REQUIRE(v == 1950);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 100, &v) == 1);
    REQUIRE(v == 2900);
}

TEST_CASE("rxpower: unknown driver returns 0", "[gs][rxpower]") {
    int v = 99;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_UNKNOWN, 50, &v) == 0);
    REQUIRE(v == 0);
}

TEST_CASE("rxpower: json single NIC", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_RTL88XXAU_WFB };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j != nullptr);
    REQUIRE(std::strstr(j, "\"wlx00\": -2000") != nullptr);
    REQUIRE(j[0] == '{');
    REQUIRE(j[std::strlen(j)-1] == '}');
    free(j);
}

TEST_CASE("rxpower: json skips unknown driver NIC", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", "wlx01", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_UNKNOWN, PP_NIC_RTL88X2EU };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j != nullptr);
    REQUIRE(std::strstr(j, "wlx00") == nullptr);
    REQUIRE(std::strstr(j, "\"wlx01\": 1950") != nullptr);
    free(j);
}

TEST_CASE("rxpower: json all-unknown returns NULL", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_UNKNOWN };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j == nullptr);
}
