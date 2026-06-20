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
// Build one FU packet. fu_type is the real NAL type being fragmented.
std::vector<uint8_t> fu_pkt(uint8_t fu_type, bool s, bool e, std::vector<uint8_t> frag) {
    std::vector<uint8_t> v = { uint8_t(49 << 1), 0x01 };          // FU payload header
    uint8_t fuh = (s ? 0x80 : 0) | (e ? 0x40 : 0) | (fu_type & 0x3F);
    v.push_back(fuh);
    v.insert(v.end(), frag.begin(), frag.end());
    return v;
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

TEST_CASE("FU across three packets reassembles one NAL", "[depay][fu]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto p0 = fu_pkt(1, true,  false, {0xA0, 0xA1});
    auto p1 = fu_pkt(1, false, false, {0xB0, 0xB1});
    auto p2 = fu_pkt(1, false, true,  {0xC0});
    REQUIRE(d.on_payload(p0.data(), p0.size(), false, 4000));
    REQUIRE(d.on_payload(p1.data(), p1.size(), false, 4000));
    REQUIRE(d.on_payload(p2.data(), p2.size(), true,  4000));
    REQUIRE(sink.aus.size() == 1);
    auto nals = split_nals(sink.aus[0]);
    REQUIRE(nals.size() == 1);
    // reconstructed NAL = [rebuilt 2-byte header (type=1)] + A0 A1 B0 B1 C0
    std::vector<uint8_t> expect = { uint8_t(1 << 1), 0x01, 0xA0,0xA1,0xB0,0xB1,0xC0 };
    REQUIRE(nals[0] == expect);
}

TEST_CASE("FU missing the Start fragment is dropped", "[depay][fu]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto cont = fu_pkt(1, false, false, {0xB0});   // S=0 with no active FU
    bool ok = d.on_payload(cont.data(), cont.size(), false, 4100);
    REQUIRE_FALSE(ok);
    REQUIRE(d.stats().fu_drops == 1);
    auto end = fu_pkt(1, false, true, {0xC0});
    d.on_payload(end.data(), end.size(), true, 4100);
    REQUIRE(sink.aus.empty());   // AU corrupt → not emitted
}

TEST_CASE("on_discontinuity mid-FU drops the partial NAL", "[depay][fu]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto p0 = fu_pkt(1, true, false, {0xA0});
    d.on_payload(p0.data(), p0.size(), false, 4200);
    d.on_discontinuity();
    auto p1 = fu_pkt(1, false, true, {0xB0});
    d.on_payload(p1.data(), p1.size(), true, 4200);
    REQUIRE(d.stats().fu_drops >= 1);
    REQUIRE(sink.aus.empty());
}

TEST_CASE("timestamp change flushes a frame whose marker was lost", "[depay][emit]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    auto a = single_nal(1, {0x01});
    d.on_payload(a.data(), a.size(), /*marker=*/false, /*ts=*/5000);  // marker lost
    REQUIRE(sink.aus.empty());
    auto b = single_nal(1, {0x02});
    d.on_payload(b.data(), b.size(), /*marker=*/false, /*ts=*/5500);  // new frame
    REQUIRE(sink.aus.size() == 1);                                    // prior AU flushed
    REQUIRE(split_nals(sink.aus[0])[0] == a);
}

TEST_CASE("IRAP without param sets gets cached VPS/SPS/PPS prepended", "[depay][paramset]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    // Frame 1: VPS+SPS+PPS+IDR together — caches the param sets, emits as-is.
    auto vps = single_nal(32, {0x01});
    auto sps = single_nal(33, {0x02});
    auto pps = single_nal(34, {0x03});
    auto idr = single_nal(/*IDR_W_RADL=*/19, {0x04});
    d.on_payload(vps.data(), vps.size(), false, 6000);
    d.on_payload(sps.data(), sps.size(), false, 6000);
    d.on_payload(pps.data(), pps.size(), false, 6000);
    d.on_payload(idr.data(), idr.size(), true,  6000);
    REQUIRE(sink.aus.size() == 1);
    REQUIRE(d.stats().param_sets_reinserted == 0);   // already present

    // Frame 2: bare IDR, no param sets — depayloader prepends cached set.
    auto idr2 = single_nal(20, {0x05});
    d.on_payload(idr2.data(), idr2.size(), true, 6500);
    REQUIRE(sink.aus.size() == 2);
    REQUIRE(d.stats().param_sets_reinserted == 1);
    auto nals = split_nals(sink.aus[1]);
    REQUIRE(nals.size() == 4);                        // VPS, SPS, PPS, IDR
    REQUIRE(((nals[0][0] >> 1) & 0x3F) == 32);
    REQUIRE(((nals[1][0] >> 1) & 0x3F) == 33);
    REQUIRE(((nals[2][0] >> 1) & 0x3F) == 34);
    REQUIRE(((nals[3][0] >> 1) & 0x3F) == 20);
}
