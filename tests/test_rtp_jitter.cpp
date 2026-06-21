#include <catch2/catch.hpp>
#include "../src/rtp_jitter.hpp"

// 60 fps @ 90 kHz: 1500 ticks/frame; nominal arrival gap 16667 us.
TEST_CASE("rtp jitter: evenly spaced arrivals converge to zero", "[rtp_jitter]") {
    RtpJitterEstimator j;
    const uint32_t ssrc = 0x1111;
    uint32_t ts = 1000;
    uint64_t rx = 100000;
    REQUIRE(j.update(ssrc, ts, rx) == Approx(0.0));   // first sample, no reference
    for (int i = 0; i < 50; ++i) {
        ts += 1500;
        rx += 16667;
        j.update(ssrc, ts, rx);
    }
    REQUIRE(j.jitter_ms() < 0.5);
}

TEST_CASE("rtp jitter: a late frame raises the estimate", "[rtp_jitter]") {
    RtpJitterEstimator j;
    const uint32_t ssrc = 0x2222;
    uint32_t ts = 0;
    uint64_t rx = 0;
    j.update(ssrc, ts, rx);
    for (int i = 0; i < 20; ++i) { ts += 1500; rx += 16667; j.update(ssrc, ts, rx); }
    double before = j.jitter_ms();
    // One frame arrives 30 ms late but is stamped on time.
    ts += 1500; rx += 16667 + 30000;
    double after = j.update(ssrc, ts, rx);
    REQUIRE(after > before);
    REQUIRE(after > 1.0);
}

TEST_CASE("rtp jitter: uint32 timestamp wrap-around does not spike", "[rtp_jitter]") {
    RtpJitterEstimator j;
    const uint32_t ssrc = 0x3333;
    uint32_t ts = 0xFFFFFFFFu - 750u;   // 750 ticks before wrap
    uint64_t rx = 500000;
    j.update(ssrc, ts, rx);
    ts += 1500;        // wraps past 0
    rx += 16667;
    double after = j.update(ssrc, ts, rx);
    REQUIRE(after < 0.5);               // modular delta keeps D ~ 0
}

TEST_CASE("rtp jitter: ssrc change resets state", "[rtp_jitter]") {
    RtpJitterEstimator j;
    j.update(0xAAAA, 1000, 0);
    j.update(0xAAAA, 2500, 16667);
    REQUIRE(j.update(0xBBBB, 99999, 9999999) == Approx(0.0));   // new stream resets
}
