#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "../src/dvr_fps_cap.h"

// Drive a stream of input frames spaced `step_us` apart through the cap and
// return how many were emitted over `count` frames.
static int emitted(int cap_fps, int64_t step_us, int count) {
    FpsCap cap(cap_fps);
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (cap.should_emit((int64_t)i * step_us)) n++;
    return n;
}

TEST_CASE("first frame always emits", "[fps_cap]") {
    FpsCap cap(60);
    REQUIRE(cap.should_emit(0) == true);
}

TEST_CASE("input at the cap passes through unchanged", "[fps_cap]") {
    // 60 fps in, cap 60 -> all 60 frames in one second emit.
    REQUIRE(emitted(60, 16667, 60) == 60);
}

TEST_CASE("input below the cap passes through unchanged", "[fps_cap]") {
    // 30 fps in, cap 60 -> all 30 frames emit (no padding/repeats).
    REQUIRE(emitted(30, 33333, 30) == 30);
}

TEST_CASE("90 fps input decimates to ~60", "[fps_cap]") {
    // 90 frames over ~1s at the cap of 60 -> ~60 emitted (allow small jitter).
    int n = emitted(60, 11111, 90);
    REQUIRE(n >= 58);
    REQUIRE(n <= 62);
}

TEST_CASE("120 fps input decimates to ~60", "[fps_cap]") {
    int n = emitted(60, 8333, 120);
    REQUIRE(n >= 58);
    REQUIRE(n <= 62);
}

TEST_CASE("the decimator never sustains above the cap", "[fps_cap]") {
    // Even very fast input (240 fps) stays within ~the cap over a second.
    int n = emitted(60, 4166, 240);
    REQUIRE(n <= 62);
}

TEST_CASE("cap_fps <= 0 means no cap (emit everything)", "[fps_cap]") {
    REQUIRE(emitted(0, 11111, 50) == 50);
}

TEST_CASE("a long gap resyncs instead of bursting", "[fps_cap]") {
    FpsCap cap(60);
    REQUIRE(cap.should_emit(0) == true);
    // ... silence ...
    REQUIRE(cap.should_emit(5'000'000) == true);   // 5s later: emit
    REQUIRE(cap.should_emit(5'005'000) == false);  // 5ms later: dropped (no backlog burst)
}
