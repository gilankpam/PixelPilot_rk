#include <catch2/catch.hpp>
#include <deque>

#include "../src/video_stutter.hpp"

TEST_CASE("is_stutter returns false during warm-up (ring < 30 samples)", "[VideoStutter]") {
    std::deque<long> recent;
    // empty ring
    REQUIRE_FALSE(is_stutter(100, recent, 1.5));

    // 29 samples (still warm-up)
    for (int i = 0; i < 29; ++i) recent.push_back(16);
    REQUIRE_FALSE(is_stutter(500, recent, 1.5));
}

TEST_CASE("is_stutter fires when interval exceeds factor * median", "[VideoStutter]") {
    std::deque<long> recent;
    for (int i = 0; i < 60; ++i) recent.push_back(16);

    // median == 16, threshold = 24
    REQUIRE_FALSE(is_stutter(16, recent, 1.5));   // equal to median
    REQUIRE_FALSE(is_stutter(23, recent, 1.5));   // below threshold
    REQUIRE_FALSE(is_stutter(24, recent, 1.5));   // at threshold (strict >)
    REQUIRE(is_stutter(25, recent, 1.5));         // above threshold
    REQUIRE(is_stutter(100, recent, 1.5));        // way above
}

TEST_CASE("is_stutter adapts to varying stream FPS via median", "[VideoStutter]") {
    std::deque<long> recent;
    // 33ms intervals = ~30fps stream; median == 33, threshold = 49
    for (int i = 0; i < 60; ++i) recent.push_back(33);

    REQUIRE_FALSE(is_stutter(40, recent, 1.5));   // normal jitter
    REQUIRE(is_stutter(50, recent, 1.5));         // genuine stutter
}
