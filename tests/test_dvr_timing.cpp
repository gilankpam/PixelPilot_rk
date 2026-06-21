#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "../src/dvr_timing.h"

/* The re-encode DVR must stamp each MP4 frame with the REAL elapsed time
 * between captures (in 90 kHz units) rather than a fixed 1/fps. The pacer's
 * timer drifts slightly slower than nominal (it re-anchors after late frames),
 * so fixed-duration muxing tags the file shorter than reality and it plays back
 * faster. Deriving each sample's duration from the capture-timestamp delta makes
 * the recorded duration match wall-clock regardless of pacing jitter. These
 * tests pin that arithmetic. */

TEST_CASE("first frame of a segment falls back to nominal 1/fps", "[dvr_timing]") {
    // last_pts < 0 means "no previous frame yet" -> nominal duration.
    REQUIRE(dvr_frame_duration_90k(/*pts_ms=*/123456, /*last_pts_ms=*/-1, /*fps=*/30) == 3000);
    REQUIRE(dvr_frame_duration_90k(0, -1, 60) == 1500);
}

TEST_CASE("steady-state duration tracks the real inter-frame delta", "[dvr_timing]") {
    // 33 ms gap at 30 fps -> 33 * 90 = 2970 (slightly under nominal 3000).
    REQUIRE(dvr_frame_duration_90k(1033, 1000, 30) == 2970);
    // 34 ms gap -> 3060 (slightly over nominal): this is what corrects the drift.
    REQUIRE(dvr_frame_duration_90k(1034, 1000, 30) == 3060);
}

TEST_CASE("non-monotonic or duplicate timestamps fall back to nominal", "[dvr_timing]") {
    REQUIRE(dvr_frame_duration_90k(1000, 1000, 30) == 3000); // equal timestamps
    REQUIRE(dvr_frame_duration_90k(900, 1000, 30) == 3000);  // regressing timestamp
}

TEST_CASE("pathological gaps are clamped so one bad ts can't wreck the timeline", "[dvr_timing]") {
    // A 5 s gap would be 450000; clamp to the 1 s (90000) ceiling.
    REQUIRE(dvr_frame_duration_90k(6000, 1000, 30) == 90000);
}

TEST_CASE("zero/invalid fps still yields a sane nominal", "[dvr_timing]") {
    REQUIRE(dvr_frame_duration_90k(0, -1, 0) == 3000);
}

/* The encoder emits SPS/PPS(/VPS) as their own output buffers, separate from
 * coded frames. Only frame (VCL) buffers advance the timeline, so the DVR must
 * tell them apart before computing a duration delta — otherwise the header send
 * (which carries the same timestamp as the next frame) would zero out that
 * frame's duration. */

static std::vector<uint8_t> annexb(std::initializer_list<uint8_t> nal_header) {
    std::vector<uint8_t> b = {0, 0, 0, 1};
    for (uint8_t x : nal_header) b.push_back(x);
    b.push_back(0x00); // a byte of payload
    return b;
}

TEST_CASE("H264 VCL slices are detected as frames", "[dvr_timing]") {
    auto idr     = annexb({0x65}); // nal_type 5 (IDR slice)
    auto non_idr = annexb({0x61}); // nal_type 1 (non-IDR slice)
    REQUIRE(dvr_buffer_has_vcl(idr.data(),     idr.size(),     /*is_h265=*/false));
    REQUIRE(dvr_buffer_has_vcl(non_idr.data(), non_idr.size(), false));
}

TEST_CASE("H264 parameter-set-only buffers are not frames", "[dvr_timing]") {
    auto sps = annexb({0x67}); // nal_type 7
    auto pps = annexb({0x68}); // nal_type 8
    REQUIRE_FALSE(dvr_buffer_has_vcl(sps.data(), sps.size(), false));
    REQUIRE_FALSE(dvr_buffer_has_vcl(pps.data(), pps.size(), false));
}

TEST_CASE("H265 VCL slices are detected as frames", "[dvr_timing]") {
    auto idr   = annexb({0x26, 0x01}); // nal_type 19 (IDR_W_RADL)
    auto trail = annexb({0x02, 0x01}); // nal_type 1  (TRAIL_R)
    REQUIRE(dvr_buffer_has_vcl(idr.data(),   idr.size(),   /*is_h265=*/true));
    REQUIRE(dvr_buffer_has_vcl(trail.data(), trail.size(), true));
}

TEST_CASE("H265 VPS/SPS/PPS-only buffers are not frames", "[dvr_timing]") {
    auto vps = annexb({0x40, 0x01}); // nal_type 32
    auto sps = annexb({0x42, 0x01}); // nal_type 33
    auto pps = annexb({0x44, 0x01}); // nal_type 34
    REQUIRE_FALSE(dvr_buffer_has_vcl(vps.data(), vps.size(), true));
    REQUIRE_FALSE(dvr_buffer_has_vcl(sps.data(), sps.size(), true));
    REQUIRE_FALSE(dvr_buffer_has_vcl(pps.data(), pps.size(), true));
}

TEST_CASE("a buffer mixing parameter sets and a slice counts as a frame", "[dvr_timing]") {
    // SPS + PPS + IDR concatenated (as a keyframe access unit might arrive).
    std::vector<uint8_t> au = {0,0,0,1, 0x67, 0x00,  0,0,0,1, 0x68, 0x00,  0,0,0,1, 0x65, 0x00};
    REQUIRE(dvr_buffer_has_vcl(au.data(), au.size(), false));
}

TEST_CASE("empty or null buffers are not frames", "[dvr_timing]") {
    REQUIRE_FALSE(dvr_buffer_has_vcl(nullptr, 0, false));
    std::vector<uint8_t> empty;
    REQUIRE_FALSE(dvr_buffer_has_vcl(empty.data(), empty.size(), true));
}
