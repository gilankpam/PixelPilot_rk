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

TEST_CASE("update_peak: stutter-classified frame promotes peak when larger", "[VideoStutter]") {
    long peak = 0;
    uint64_t peak_ts = 0;

    update_peak(peak, peak_ts, /*interval=*/47, /*is_stutter=*/true, /*now_ms=*/1000);
    REQUIRE(peak == 47);
    REQUIRE(peak_ts == 1000);

    // Smaller interval, stutter: should NOT lower peak
    update_peak(peak, peak_ts, 30, true, 1500);
    REQUIRE(peak == 47);
    REQUIRE(peak_ts == 1000);

    // Larger interval, stutter: should promote
    update_peak(peak, peak_ts, 80, true, 2000);
    REQUIRE(peak == 80);
    REQUIRE(peak_ts == 2000);
}

TEST_CASE("update_peak: non-stutter frames never promote peak", "[VideoStutter]") {
    long peak = 0;
    uint64_t peak_ts = 0;

    // Even a huge interval that isn't classified as stutter must not promote.
    update_peak(peak, peak_ts, 500, /*is_stutter=*/false, /*now_ms=*/1000);
    REQUIRE(peak == 0);
    REQUIRE(peak_ts == 0);
}

TEST_CASE("update_peak: expires after 10s and may then be replaced by smaller value", "[VideoStutter]") {
    long peak = 47;
    uint64_t peak_ts = 1000;

    // 9.9s later — not expired yet
    update_peak(peak, peak_ts, 20, true, 1000 + 9'900);
    REQUIRE(peak == 47); // 20 < 47 and not expired, no change

    // 10.001s after original peak_ts — expired; then 20 promotes.
    update_peak(peak, peak_ts, 20, true, 1000 + 10'001);
    REQUIRE(peak == 20);
    REQUIRE(peak_ts == 1000 + 10'001);
}

TEST_CASE("update_peak: expiry without a new stutter resets peak to 0", "[VideoStutter]") {
    long peak = 47;
    uint64_t peak_ts = 1000;

    // 11s later, non-stutter frame: peak must reset to 0.
    update_peak(peak, peak_ts, 16, /*is_stutter=*/false, /*now_ms=*/12'000);
    REQUIRE(peak == 0);
    REQUIRE(peak_ts == 0);
}
