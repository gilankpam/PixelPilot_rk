/* tests/test_settings_gs_enum.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "gsmenu/settings_gs_enum.h"
}

TEST_CASE("iw_list: extracts enabled channels in old gsmenu format", "[gs][enum]") {
    /* Subset of real `iw list` output. Lines without a [<n>] band marker
     * and disabled lines must be skipped. */
    const char *in =
      "    Frequencies:\n"
      "      * 2412 MHz [1] (20.0 dBm)\n"
      "      * 2417 MHz [2] (20.0 dBm) (radar detection)\n"
      "      * 2484 MHz [14] (disabled)\n"
      "      * 5180 MHz [36] (20.0 dBm)\n"
      "      * 5825 MHz [165] (20.0 dBm)\n";
    char *out = pp_gs_parse_iw_list_channels(in);
    REQUIRE(out != nullptr);
    std::string s(out); free(out);
    /* Expected format: "<chan> (<freq> MHz)" per old script. */
    REQUIRE(s.find("1 (2412 MHz)") != std::string::npos);
    REQUIRE(s.find("36 (5180 MHz)") != std::string::npos);
    REQUIRE(s.find("165 (5825 MHz)") != std::string::npos);
    REQUIRE(s.find("14 ") == std::string::npos);     /* disabled */
    REQUIRE(s.find("2 (2417 MHz)") == std::string::npos); /* radar */
}

TEST_CASE("iw_list: empty input -> NULL", "[gs][enum]") {
    REQUIRE(pp_gs_parse_iw_list_channels("") == nullptr);
    REQUIRE(pp_gs_parse_iw_list_channels("no channels here") == nullptr);
}

