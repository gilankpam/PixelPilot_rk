#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "gsmenu/settings.h"
#include "gsmenu/widgets/pp_slider_bounds.h"
}

/* Bound-computation tests use the dummy provider as the live-value source.
 * The dummy stores values by key (domain/page ignored). We seed via
 * pp_settings_set, then assert the bound math. */

static void seed(const char *key, const char *val) {
    pp_settings_set("air", "wfbng", key, val);
}

TEST_CASE("slider_bounds: no relation key returns static bound",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    REQUIRE(pp_slider_bound_max(31, NULL, NULL, NULL, 0) == 31);
    REQUIRE(pp_slider_bound_min(2,  NULL, NULL, NULL, 0) == 2);
}

TEST_CASE("slider_bounds: empty live value returns static bound",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    /* "not_a_real_key" is not in the dummy seed; dummy_get returns "". */
    REQUIRE(pp_slider_bound_max(31, "air", "wfbng",
                                "not_a_real_key", -2) == 31);
    REQUIRE(pp_slider_bound_min(2,  "air", "wfbng",
                                "not_a_real_key",  2) == 2);
}

TEST_CASE("slider_bounds: FEC_K max = N - 2 when N below static cap",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    seed("fec_n", "12");
    /* Static K max = 31. Live N = 12 → bound = 10. min(31, 10) = 10. */
    REQUIRE(pp_slider_bound_max(31, "air", "wfbng", "fec_n", -2) == 10);
}

TEST_CASE("slider_bounds: static cap wins when relation would allow more",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    seed("fec_n", "40");  /* hypothetical out-of-range value */
    /* Static K max = 31. Live N = 40 → bound = 38. min(31, 38) = 31. */
    REQUIRE(pp_slider_bound_max(31, "air", "wfbng", "fec_n", -2) == 31);
}

TEST_CASE("slider_bounds: FEC_N min = K + 2 when K above static floor",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    seed("fec_k", "8");
    /* Static N min = 2. Live K = 8 → bound = 10. max(2, 10) = 10. */
    REQUIRE(pp_slider_bound_min(2, "air", "wfbng", "fec_k", 2) == 10);
}

TEST_CASE("slider_bounds: static floor wins when relation would allow less",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    seed("fec_k", "1");
    /* Static N min = 2. Live K = 1 → bound = 3. max(2, 3) = 3. */
    REQUIRE(pp_slider_bound_min(2, "air", "wfbng", "fec_k", 2) == 3);

    /* When live K = -5 (degenerate, never reachable from UI but the
     * math should still pick the larger of the two): bound = -3.
     * max(2, -3) = 2. Static floor wins. */
    seed("fec_k", "-5");
    REQUIRE(pp_slider_bound_min(2, "air", "wfbng", "fec_k", 2) == 2);
}

TEST_CASE("slider_bounds: K-N pair invariant — k <= n - 2 from both sides",
          "[slider][bounds]") {
    pp_settings_register_dummy();
    /* Realistic mid-range pair. */
    seed("fec_k", "8");
    seed("fec_n", "12");
    int32_t k_max = pp_slider_bound_max(31, "air", "wfbng", "fec_n", -2);
    int32_t n_min = pp_slider_bound_min(2,  "air", "wfbng", "fec_k",  2);
    REQUIRE(k_max == 10);   /* K can grow to 10 max */
    REQUIRE(n_min == 10);   /* N can shrink to 10 min */
    /* Together: maximum K (10) and minimum N (10) collapse to k = n - 2
     * — never k > n - 2. */
    REQUIRE(k_max == n_min);
}
