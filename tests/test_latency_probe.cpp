#include <catch2/catch.hpp>
#include <cstdint>
#include <cstring>
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
}

TEST_CASE("RTP header parse: wrong version rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(/*version=*/3, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), h.size(), info));
}

TEST_CASE("RTP header parse: too short rejected",
          "[latency_probe][rtp]") {
    auto h = make_header(2, true, 1u, 2u);
    lp::RtpHeaderInfo info;
    REQUIRE_FALSE(lp::parse_rtp_header(h.data(), 11, info));
    REQUIRE_FALSE(lp::parse_rtp_header(nullptr, 0, info));
}
