#include <catch2/catch.hpp>
#include <cstdint>
#include <vector>

#include "../src/latency_probe.hpp"

namespace lp = latency_probe;

namespace {
// Helper: build a synthetic RTP header with the given fields.
// Returns a 12-byte vector.
std::vector<uint8_t> make_header(uint8_t version, bool marker,
                                 uint32_t timestamp, uint32_t ssrc) {
    std::vector<uint8_t> b(12, 0);
    b[0] = (version & 0x03) << 6;            // V=version, P=0, X=0, CC=0
    b[1] = (marker ? 0x80 : 0x00) | 0x60;    // M + PT=96 (dyn)
    b[2] = 0; b[3] = 0;                      // seq
    b[4] = (timestamp >> 24) & 0xff;
    b[5] = (timestamp >> 16) & 0xff;
    b[6] = (timestamp >> 8)  & 0xff;
    b[7] = (timestamp     )  & 0xff;
    b[8]  = (ssrc >> 24) & 0xff;
    b[9]  = (ssrc >> 16) & 0xff;
    b[10] = (ssrc >> 8)  & 0xff;
    b[11] = (ssrc     )  & 0xff;
    return b;
}
}

TEST_CASE("RTP header parse: marker=1, ssrc, timestamp extracted",
          "[latency_probe][rtp]") {
    auto h = make_header(/*version=*/2, /*marker=*/true,
                         /*ts=*/0x11223344u, /*ssrc=*/0xDEADBEEFu);

    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.marker == true);
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
}

TEST_CASE("RTP header parse: marker=0 still parses but flags off",
          "[latency_probe][rtp]") {
    auto h = make_header(2, false, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.marker == false);
    REQUIRE(info.timestamp == 1u);
    REQUIRE(info.ssrc == 2u);
}

TEST_CASE("RTP header parse: wrong version rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(/*version=*/3, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), h.size(), info));
}

TEST_CASE("RTP header parse: non-zero CSRC count still parses (CC bits ignored)",
          "[latency_probe][rtp]") {
    // The parser only reads bytes 0..11. CSRCs sit AFTER byte 11, so they
    // do not affect us — but we should prove that a CC>0 packet still
    // parses correctly. Real RTP packets do sometimes carry CSRCs.
    auto h = make_header(2, true, 0x11223344u, 0xDEADBEEFu);
    h[0] |= 0x03;  // CC = 3 (three CSRC entries would follow if we read further)
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
    REQUIRE(info.marker == true);
}

TEST_CASE("RTP header parse: extension bit set still parses",
          "[latency_probe][rtp]") {
    // Extension header sits after CSRC list; the fixed 12-byte prefix
    // is unaffected.
    auto h = make_header(2, true, 0x11223344u, 0xDEADBEEFu);
    h[0] |= 0x10;  // X bit
    lp::RtpHeaderInfo info;
    REQUIRE(lp::parse_rtp_header(h.data(), h.size(), info));
    REQUIRE(info.timestamp == 0x11223344u);
    REQUIRE(info.ssrc == 0xDEADBEEFu);
    REQUIRE(info.marker == true);
}

TEST_CASE("RTP header parse: too short rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(2, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), 11, info));
    REQUIRE_FALSE(lp::parse_rtp_header(nullptr, 0, info));
}

TEST_CASE("ClockOffset: returns 0/0 before any sample",
          "[latency_probe][clock]") {
    lp::ClockOffset c;
    int64_t off = 999;
    uint64_t rtt = 999;
    c.get(off, rtt);
    REQUIRE(off == 0);
    REQUIRE(rtt == 0);
}

TEST_CASE("ClockOffset: picks min-RTT sample",
          "[latency_probe][clock]") {
    lp::ClockOffset c;

    // Sample 1: drone is +1000us ahead, RTT=200us
    //   t1=1000, t2=1500 (drone), t3=1600 (drone), t4=1200
    //   rtt = (t4-t1) - (t3-t2) = 200 - 100 = 100   (NOTE: small RTT here)
    //   offset = ((t2-t1) + (t3-t4))/2 = (500 + 400)/2 = 450
    c.add_sample(1000, 1500, 1600, 1200);

    // Sample 2: same offset, but with extra queueing in one direction.
    //   t1=2000, t2=2500 (true offset still ~500), t3=2600, t4=2800
    //   rtt = 800 - 100 = 700
    //   offset = (500 + -200)/2 = 150  (BIASED by queueing)
    c.add_sample(2000, 2500, 2600, 2800);

    int64_t off = 0; uint64_t rtt = 0;
    c.get(off, rtt);
    REQUIRE(rtt == 100);   // min-RTT picked
    REQUIRE(off == 450);   // its offset, not the biased one
}

TEST_CASE("ClockOffset: rescans after ring eviction of current best",
          "[latency_probe][clock]") {
    lp::ClockOffset c;

    // First sample becomes best (RTT=0).
    c.add_sample(0, 100, 110, 10);    // rtt = 10-0 - 10 = 0  (use generous arith)
    int64_t off; uint64_t rtt;
    c.get(off, rtt);
    auto best_rtt_before = rtt;
    auto best_off_before = off;

    // Fill 15 more samples with much larger RTT so the first stays best.
    for (int i = 0; i < 15; ++i) {
        uint64_t base = 1000ull + i * 100ull;
        // Big asymmetry -> big RTT.
        c.add_sample(base, base + 5000, base + 5100, base + 10000);
    }
    c.get(off, rtt);
    REQUIRE(rtt == best_rtt_before);
    REQUIRE(off == best_off_before);

    // Add one more -- the original best gets evicted (ring=16, this is the 17th).
    // The new best should be from the remaining 16 samples.
    c.add_sample(20'000, 25'000, 25'100, 30'000);
    c.get(off, rtt);
    REQUIRE(rtt != best_rtt_before);   // the original is gone
}
