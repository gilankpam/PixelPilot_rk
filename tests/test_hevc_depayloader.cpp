#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "hevc_depayloader.h"

namespace {
// Collects emitted access units for assertions.
struct Sink {
    std::vector<std::vector<uint8_t>> aus;
    HevcDepayloader::FrameCallback cb() {
        return [this](const uint8_t* p, size_t n) { aus.emplace_back(p, p + n); };
    }
};

// One HEVC NAL header (2 bytes) for the given type, layer 0, tid 1.
std::vector<uint8_t> nal_hdr(uint8_t type) {
    return { uint8_t((type & 0x3F) << 1), 0x01 };
}
// Single-NAL RTP payload: 2-byte NAL header + body bytes.
std::vector<uint8_t> single_nal(uint8_t type, std::vector<uint8_t> body) {
    auto v = nal_hdr(type);
    v.insert(v.end(), body.begin(), body.end());
    return v;
}
// Split an Annex-B AU into NAL bodies (drops 00000001 start codes).
std::vector<std::vector<uint8_t>> split_nals(const std::vector<uint8_t>& au) {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    while (i + 4 <= au.size()) {
        // expect start code 00 00 00 01
        i += 4;
        size_t start = i;
        while (i + 4 <= au.size() &&
               !(au[i]==0 && au[i+1]==0 && au[i+2]==0 && au[i+3]==1)) i++;
        size_t end = (i + 4 <= au.size()) ? i : au.size();
        out.emplace_back(au.begin() + start, au.begin() + end);
        if (end == au.size()) break;
    }
    return out;
}
}

TEST_CASE("single-NAL frame emits one AU on marker", "[depay][single]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto p = single_nal(/*TRAIL_R=*/1, {0xAA, 0xBB, 0xCC});
    bool ok = d.on_payload(p.data(), p.size(), /*marker=*/true, /*ts=*/1000);
    REQUIRE(ok);
    REQUIRE(sink.aus.size() == 1);
    // AU = 00 00 00 01 + NAL
    std::vector<uint8_t> expect = {0,0,0,1};
    expect.insert(expect.end(), p.begin(), p.end());
    REQUIRE(sink.aus[0] == expect);
    REQUIRE(d.stats().aus_emitted == 1);
}

TEST_CASE("two single-NAL packets, one AU until marker", "[depay][single]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto a = single_nal(33, {0x01});           // SPS
    auto b = single_nal(1, {0x02, 0x03});      // slice
    REQUIRE(d.on_payload(a.data(), a.size(), false, 2000));
    REQUIRE(sink.aus.empty());                 // no marker yet
    REQUIRE(d.on_payload(b.data(), b.size(), true, 2000));
    REQUIRE(sink.aus.size() == 1);
    auto nals = split_nals(sink.aus[0]);
    REQUIRE(nals.size() == 2);
    REQUIRE(nals[0] == a);
    REQUIRE(nals[1] == b);
}

TEST_CASE("aggregation packet expands to multiple NALs", "[depay][ap]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto sps = single_nal(33, {0x11, 0x12});
    auto pps = single_nal(34, {0x21});
    // AP: 2-byte AP header (type 48) + [u16 size][nal] * 2
    std::vector<uint8_t> ap = { uint8_t(48 << 1), 0x01 };
    auto add = [&](const std::vector<uint8_t>& n) {
        ap.push_back(uint8_t(n.size() >> 8));
        ap.push_back(uint8_t(n.size() & 0xFF));
        ap.insert(ap.end(), n.begin(), n.end());
    };
    add(sps); add(pps);
    REQUIRE(d.on_payload(ap.data(), ap.size(), true, 3000));
    REQUIRE(sink.aus.size() == 1);
    auto nals = split_nals(sink.aus[0]);
    REQUIRE(nals.size() == 2);
    REQUIRE(nals[0] == sps);
    REQUIRE(nals[1] == pps);
}

TEST_CASE("aggregation packet with overrunning size is dropped+counted", "[depay][ap]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    // claim a 0x00FF-byte NAL but provide only 3 bytes
    std::vector<uint8_t> ap = { uint8_t(48 << 1), 0x01, 0x00, 0xFF, 0xAA, 0xBB, 0xCC };
    bool ok = d.on_payload(ap.data(), ap.size(), true, 3100);
    REQUIRE_FALSE(ok);
    REQUIRE(d.stats().malformed == 1);
    REQUIRE(sink.aus.empty());   // corrupt AU not emitted
}
