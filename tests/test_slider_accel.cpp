#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "gsmenu/widgets/pp_slider_accel.h"
}

/* Acceleration step scaling: base step grows with hold count. */

TEST_CASE("accel_step: returns base for low hold counts", "[slider][accel]") {
    REQUIRE(pp_slider_accel_step(1,  0) == 1);
    REQUIRE(pp_slider_accel_step(1,  3) == 1);
    REQUIRE(pp_slider_accel_step(5,  3) == 5);
}

TEST_CASE("accel_step: doubles after 4 ticks", "[slider][accel]") {
    REQUIRE(pp_slider_accel_step(1,  4) == 2);
    REQUIRE(pp_slider_accel_step(1,  7) == 2);
    REQUIRE(pp_slider_accel_step(5,  4) == 10);
}

TEST_CASE("accel_step: 4x after 8 ticks", "[slider][accel]") {
    REQUIRE(pp_slider_accel_step(1,  8) == 4);
    REQUIRE(pp_slider_accel_step(1, 15) == 4);
    REQUIRE(pp_slider_accel_step(2,  8) == 8);
}

TEST_CASE("accel_step: 8x cap after 16 ticks", "[slider][accel]") {
    REQUIRE(pp_slider_accel_step(1,  16) == 8);
    REQUIRE(pp_slider_accel_step(1, 100) == 8);   /* no runaway */
    REQUIRE(pp_slider_accel_step(3,  20) == 24);
}

TEST_CASE("accel_step: negative hold counts treated as 0", "[slider][accel]") {
    /* Defensive: caller shouldn't pass negative, but if they do return base. */
    REQUIRE(pp_slider_accel_step(1, -1) == 1);
    REQUIRE(pp_slider_accel_step(5, -100) == 5);
}

/* Acceleration update: counts consecutive same-key events arriving close
 * in time. Resets when key changes or too much time elapses. */

TEST_CASE("accel_update: first press starts at 1", "[slider][accel]") {
    /* prev_count=0 implies no previous press tracked. Same-key after a
     * short gap → 1. */
    REQUIRE(pp_slider_accel_update(100, 50, 1, 1, 0) == 1);
}

TEST_CASE("accel_update: consecutive same-key within window increments",
          "[slider][accel]") {
    REQUIRE(pp_slider_accel_update(100,  50, 1, 1,  3) == 4);
    REQUIRE(pp_slider_accel_update(200, 100, 1, 1, 10) == 11);
}

TEST_CASE("accel_update: different key resets to 0", "[slider][accel]") {
    /* UP then DOWN (or different key code) — count resets. */
    REQUIRE(pp_slider_accel_update(100, 50, 1, 2, 5) == 0);
}

TEST_CASE("accel_update: gap > 250ms resets to 0", "[slider][accel]") {
    REQUIRE(pp_slider_accel_update(400, 100, 1, 1, 5) == 0);
    REQUIRE(pp_slider_accel_update(1000, 50, 1, 1, 100) == 0);
}

TEST_CASE("accel_update: gap at 250ms boundary is still a reset",
          "[slider][accel]") {
    /* Exactly 250ms gap — caller policy; we treat >= 250 as a reset. */
    REQUIRE(pp_slider_accel_update(300, 50, 1, 1, 5) == 0);
}

TEST_CASE("accel_update: gap just under 250ms continues the hold",
          "[slider][accel]") {
    REQUIRE(pp_slider_accel_update(249, 0, 1, 1, 5) == 6);
}
