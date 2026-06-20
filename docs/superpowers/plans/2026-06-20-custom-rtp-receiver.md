# Custom RTP Receiver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace GStreamer on the latency-critical live RTP path with a hand-rolled HEVC depayloader feeding MPP directly, keeping GStreamer only for DVR mp4 playback.

**Architecture:** A pure `HevcDepayloader` (RFC 7798, zero deps) emits one Annex-B access unit per frame on the RTP marker bit; a single-threaded `RtpVideoReceiver` owns the socket + recv loop and the side-channels (last-hop, gap→IDR, stream-presence, restream, latency probe); the old `GstRtpReceiver` is slimmed to a DVR-only `GstFilePlayer`. The system collapses to H265-only on the decode path (the runtime MPP decoder-reinit is deleted).

**Tech Stack:** C++17, Rockchip MPP, GStreamer 1.x (DVR only), spdlog, Catch2 v3 (host `USE_SIMULATOR` tests), CMake.

**Spec:** `docs/superpowers/specs/2026-06-20-custom-rtp-receiver-design.md`

## Global Constraints

- **H265-only decode path.** Live receiver and MPP decoder are HEVC only. MPP inits once as `MPP_VIDEO_CodingHEVC`; no runtime reinit. (Spec D6, §7)
- **Keep `VideoCodec` enum intact, incl. `H264`** — the DVR re-encoder (`dvr_reenc_set_codec`, `mpp_encoder`) still uses `VideoCodec::H264`. Do not touch the re-encoder. (Spec §7)
- **Preserve the `NEW_FRAME_CALLBACK` shape** `std::function<void(std::shared_ptr<std::vector<uint8_t>>)>` feeding MPP. (Spec §5)
- **Preserve the `idr_*` and `restream_*` C-API signatures** exactly (gsmenu consumes them). (Spec §5)
- **Preserve the `gstreamer.received_bytes` OSD fact name** (osd.cpp matches it). (Spec §7)
- **In-order passthrough, no reorder buffer.** Match production `--rtp-jitter-ms 0`. (Spec D3)
- **`HevcDepayloader` has zero dependencies** — no gst, no MPP, no spdlog. `<cstdint>/<cstddef>/<cstring>/<vector>/<functional>` only. (Enables host tests + fuzzing.)
- **`sprop-max-don-diff = 0` assumed** — no DONL/DOND fields in AP/FU. (Spec §6.1)
- **Frame boundary = RTP marker bit**, with RTP-timestamp-change as fallback for a lost marker packet. (Spec §6.1; validated 357/357 in §4)
- **Bounds-check every read** in the depayloader — it is the network-facing attack surface. Malformed input drops-and-counts, never crashes/OOB. (Spec §6.1)

## File Structure

**New**
- `src/video_codec.h` — `enum class VideoCodec` + `video_codec(const char*)`, extracted from `gstrtpreceiver.h`.
- `src/hevc_depayloader.h` / `.cpp` — pure RFC 7798 depayloader (the tested crux).
- `src/rtp_video_receiver.h` / `.cpp` — live receiver: socket + recv thread + side-channels + the `idr_*`/`restream_*` C-API.
- `src/gst_file_player.h` / `.cpp` — DVR file player (slimmed from `gstrtpreceiver`).
- `tests/test_hevc_depayloader.cpp` — host unit tests (new `rtp_depay_tests` target).
- `tools/gen_hevc_golden.py` — capture + golden-fixture generator (gst host, documented one-time step).
- `tests/files/hevc_capture.bin`, `tests/files/hevc_golden.bin` — golden parity fixtures.

**Deleted**
- `src/gstrtpreceiver.h` / `.cpp` — content split across the three new modules + `video_codec.h`.

**Modified**
- `src/main.cpp`, `src/main.h` — receiver type swap, callback wiring, `switch_pipeline_source` + playback routing, remove MPP reinit, fixed HEVC init, `--codec` warn.
- `CMakeLists.txt` — source list swap, drop `gstnet`+`gio`, add `rtp_depay_tests` host target.
- gsmenu/`*` files including `gstrtpreceiver.h` for the C-API — repoint include.

---

# Phase 1 — `HevcDepayloader` (pure, host-tested)

This phase delivers a standalone, fully-tested depayloader with no app dependency. Build/run for every step in this phase:

```bash
nix-shell shell-sim.nix --run "mkdir -p build-test && cd build-test && cmake -DUSE_SIMULATOR=ON .. && make rtp_depay_tests && ./rtp_depay_tests"
```

Run a single test case: `./build-test/rtp_depay_tests "<case name>"`.

---

### Task 1: Depayloader skeleton + single-NAL + marker emit

**Files:**
- Create: `src/hevc_depayloader.h`
- Create: `src/hevc_depayloader.cpp`
- Create: `tests/test_hevc_depayloader.cpp`
- Modify: `CMakeLists.txt` (add `rtp_depay_tests` target in the `if(USE_SIMULATOR)` block, near the other standalone test targets, e.g. after `osd_buf_tests`)

**Interfaces:**
- Produces: `class HevcDepayloader` with `using FrameCallback = std::function<void(const uint8_t* au, size_t len)>;`, ctor `HevcDepayloader(FrameCallback)`, `bool on_payload(const uint8_t* payload, size_t len, bool marker, uint32_t rtp_ts)`, `void on_discontinuity()`, `void reset()`, and `const Stats& stats() const` where `struct Stats { uint64_t malformed, fu_drops, aus_emitted, param_sets_reinserted; }`.

- [ ] **Step 1: Create the header**

`src/hevc_depayloader.h`:

```cpp
#ifndef HEVC_DEPAYLOADER_H
#define HEVC_DEPAYLOADER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Pure RFC 7798 HEVC RTP depayloader. No sockets, threads, gstreamer, or MPP.
// Feed RTP *payloads* (bytes after the 12-byte RTP header) plus the marker bit and
// RTP timestamp; it emits one complete Annex-B access unit per frame via the
// callback (4-byte start codes, parameter sets ensured before each IRAP).
class HevcDepayloader {
public:
    using FrameCallback = std::function<void(const uint8_t* au, size_t len)>;
    explicit HevcDepayloader(FrameCallback on_access_unit);

    // Returns false if this payload was malformed (dropped); true otherwise.
    bool on_payload(const uint8_t* payload, size_t len, bool marker, uint32_t rtp_ts);
    // Receiver detected an RTP sequence gap: drop partial FU + mark AU corrupt.
    void on_discontinuity();
    // Clear parameter-set cache and pending AU (stream restart).
    void reset();

    struct Stats {
        uint64_t malformed = 0;
        uint64_t fu_drops = 0;
        uint64_t aus_emitted = 0;
        uint64_t param_sets_reinserted = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    void append_nal_with_startcode(const uint8_t* nal, size_t len);
    void flush_au();
    void handle_single_nal(const uint8_t* p, size_t len);
    bool handle_ap(const uint8_t* p, size_t len);
    bool handle_fu(const uint8_t* p, size_t len);

    FrameCallback cb_;
    std::vector<uint8_t> au_;   // current access unit (Annex-B)
    std::vector<uint8_t> fu_;   // in-progress FU NAL (Annex-B, incl. rebuilt header)
    bool fu_active_ = false;
    bool au_has_data_ = false;
    bool au_corrupt_ = false;
    bool have_ts_ = false;
    uint32_t cur_ts_ = 0;
    bool au_has_irap_ = false;
    bool au_has_vps_ = false, au_has_sps_ = false, au_has_pps_ = false;
    std::vector<uint8_t> vps_, sps_, pps_;  // cached most-recent param sets (no start code)
    Stats stats_;
};

#endif // HEVC_DEPAYLOADER_H
```

- [ ] **Step 2: Write the failing test**

`tests/test_hevc_depayloader.cpp`:

```cpp
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
```

- [ ] **Step 3: Add the `rtp_depay_tests` CMake target**

In `CMakeLists.txt`, inside the `if(USE_SIMULATOR)` block (next to `osd_buf_tests`), add:

```cmake
    add_executable(rtp_depay_tests
      src/hevc_depayloader.cpp
      tests/test_hevc_depayloader.cpp)
    target_include_directories(rtp_depay_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR})
    target_link_libraries(rtp_depay_tests Catch2::Catch2WithMain)
```

- [ ] **Step 4: Run the test — verify it fails to link/compile**

Run: `nix-shell shell-sim.nix --run "mkdir -p build-test && cd build-test && cmake -DUSE_SIMULATOR=ON .. && make rtp_depay_tests"`
Expected: FAIL — `undefined reference` to `HevcDepayloader::HevcDepayloader`/`on_payload` (no `.cpp` body yet).

- [ ] **Step 5: Implement the minimal body**

`src/hevc_depayloader.cpp`:

```cpp
#include "hevc_depayloader.h"
#include <cstring>

HevcDepayloader::HevcDepayloader(FrameCallback on_access_unit)
    : cb_(std::move(on_access_unit)) {}

void HevcDepayloader::append_nal_with_startcode(const uint8_t* nal, size_t len) {
    if (len == 0) return;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    au_.insert(au_.end(), sc, sc + 4);
    au_.insert(au_.end(), nal, nal + len);
    au_has_data_ = true;
    const uint8_t t = (nal[0] >> 1) & 0x3F;
    if (t == 32) { vps_.assign(nal, nal + len); au_has_vps_ = true; }
    else if (t == 33) { sps_.assign(nal, nal + len); au_has_sps_ = true; }
    else if (t == 34) { pps_.assign(nal, nal + len); au_has_pps_ = true; }
    if (t >= 16 && t <= 21) au_has_irap_ = true;
}

void HevcDepayloader::flush_au() {
    if (au_has_data_ && !au_corrupt_) {
        cb_(au_.data(), au_.size());
        stats_.aus_emitted++;
    }
    au_.clear();
    au_has_data_ = false;
    au_corrupt_ = false;
    au_has_irap_ = false;
    au_has_vps_ = au_has_sps_ = au_has_pps_ = false;
    have_ts_ = false;
    if (fu_active_) { stats_.fu_drops++; fu_active_ = false; fu_.clear(); }
}

void HevcDepayloader::handle_single_nal(const uint8_t* p, size_t len) {
    append_nal_with_startcode(p, len);
}

bool HevcDepayloader::handle_ap(const uint8_t* p, size_t len) { (void)p; (void)len; return true; }
bool HevcDepayloader::handle_fu(const uint8_t* p, size_t len) { (void)p; (void)len; return true; }

bool HevcDepayloader::on_payload(const uint8_t* p, size_t len, bool marker, uint32_t rtp_ts) {
    if (len < 2) { stats_.malformed++; return false; }
    if (have_ts_ && rtp_ts != cur_ts_) flush_au();   // lost-marker fallback
    cur_ts_ = rtp_ts;
    have_ts_ = true;

    const uint8_t type = (p[0] >> 1) & 0x3F;
    bool ok = true;
    if (type <= 47)      handle_single_nal(p, len);
    else if (type == 48) ok = handle_ap(p, len);
    else if (type == 49) ok = handle_fu(p, len);
    else { stats_.malformed++; ok = false; }

    if (marker) flush_au();
    return ok;
}

void HevcDepayloader::on_discontinuity() {
    if (fu_active_) { stats_.fu_drops++; fu_active_ = false; fu_.clear(); }
    au_corrupt_ = true;
}

void HevcDepayloader::reset() {
    au_.clear(); fu_.clear();
    fu_active_ = au_has_data_ = au_corrupt_ = have_ts_ = au_has_irap_ = false;
    au_has_vps_ = au_has_sps_ = au_has_pps_ = false;
    vps_.clear(); sps_.clear(); pps_.clear();
    cur_ts_ = 0;
}
```

- [ ] **Step 6: Run the tests — verify pass**

Run: `nix-shell shell-sim.nix --run "cd build-test && make rtp_depay_tests && ./rtp_depay_tests '[single]'"`
Expected: PASS (2 assertions-cases in `[single]`).

- [ ] **Step 7: Commit**

```bash
git add src/hevc_depayloader.h src/hevc_depayloader.cpp tests/test_hevc_depayloader.cpp CMakeLists.txt
git commit -m "feat(depay): HevcDepayloader skeleton — single-NAL + marker-bit emit"
```

---

### Task 2: Aggregation Packets (AP, type 48)

**Files:**
- Modify: `src/hevc_depayloader.cpp` (`handle_ap`)
- Test: `tests/test_hevc_depayloader.cpp`

**Interfaces:**
- Consumes: `HevcDepayloader` from Task 1.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_hevc_depayloader.cpp`:

```cpp
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
```

- [ ] **Step 2: Run — verify the AP tests fail**

Run: `./build-test/rtp_depay_tests "[ap]"` (after `make`)
Expected: FAIL — `handle_ap` is a stub, so the first test sees 0 NALs and the second still emits.

- [ ] **Step 3: Implement `handle_ap`**

Replace the stub `handle_ap` in `src/hevc_depayloader.cpp` with:

```cpp
bool HevcDepayloader::handle_ap(const uint8_t* p, size_t len) {
    size_t off = 2;  // skip 2-byte AP payload header
    while (off + 2 <= len) {
        const size_t nal_size = (size_t(p[off]) << 8) | p[off + 1];
        off += 2;
        if (nal_size == 0 || off + nal_size > len) {
            stats_.malformed++;
            au_corrupt_ = true;
            return false;
        }
        append_nal_with_startcode(p + off, nal_size);
        off += nal_size;
    }
    return true;
}
```

- [ ] **Step 4: Run — verify pass**

Run: `make rtp_depay_tests && ./build-test/rtp_depay_tests "[ap]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/hevc_depayloader.cpp tests/test_hevc_depayloader.cpp
git commit -m "feat(depay): aggregation-packet (AP) support"
```

---

### Task 3: Fragmentation Units (FU, type 49) + discontinuity

**Files:**
- Modify: `src/hevc_depayloader.cpp` (`handle_fu`)
- Test: `tests/test_hevc_depayloader.cpp`

**Interfaces:**
- Consumes: `HevcDepayloader` from Tasks 1-2.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_hevc_depayloader.cpp`:

```cpp
namespace {
// Build one FU packet. fu_type is the real NAL type being fragmented.
std::vector<uint8_t> fu_pkt(uint8_t fu_type, bool s, bool e, std::vector<uint8_t> frag) {
    std::vector<uint8_t> v = { uint8_t(49 << 1), 0x01 };          // FU payload header
    uint8_t fuh = (s ? 0x80 : 0) | (e ? 0x40 : 0) | (fu_type & 0x3F);
    v.push_back(fuh);
    v.insert(v.end(), frag.begin(), frag.end());
    return v;
}
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
```

Note: the second and third FU tests exercise the `au_corrupt_` path — a lost Start fragment or a mid-FU discontinuity must prevent the AU from being emitted at all.

- [ ] **Step 2: Run — verify FU tests fail**

Run: `./build-test/rtp_depay_tests "[fu]"`
Expected: FAIL — `handle_fu` is a stub.

- [ ] **Step 3: Implement `handle_fu`**

Replace the stub `handle_fu` in `src/hevc_depayloader.cpp` with:

```cpp
bool HevcDepayloader::handle_fu(const uint8_t* p, size_t len) {
    if (len < 3) { stats_.malformed++; return false; }  // 2-byte hdr + 1-byte FU hdr
    const uint8_t fuh = p[2];
    const bool start = (fuh & 0x80) != 0;
    const bool end   = (fuh & 0x40) != 0;
    const uint8_t fu_type = fuh & 0x3F;
    const uint8_t* frag = p + 3;
    const size_t frag_len = len - 3;

    if (start) {
        if (fu_active_) { stats_.fu_drops++; au_corrupt_ = true; }  // lost End of prior FU
        fu_.clear();
        // Rebuild the 2-byte NAL header: keep forbidden/layer/tid, set type=fu_type.
        fu_.push_back(uint8_t((p[0] & 0x81) | (fu_type << 1)));
        fu_.push_back(p[1]);
        fu_.insert(fu_.end(), frag, frag + frag_len);
        fu_active_ = true;
    } else {
        if (!fu_active_) { stats_.fu_drops++; au_corrupt_ = true; return false; }  // lost Start
        fu_.insert(fu_.end(), frag, frag + frag_len);
    }

    if (end && fu_active_) {
        append_nal_with_startcode(fu_.data(), fu_.size());
        fu_active_ = false;
        fu_.clear();
    }
    return true;
}
```

- [ ] **Step 4: Run — verify pass**

Run: `make rtp_depay_tests && ./build-test/rtp_depay_tests "[fu]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/hevc_depayloader.cpp tests/test_hevc_depayloader.cpp
git commit -m "feat(depay): FU reassembly + discontinuity handling"
```

---

### Task 4: Lost-marker fallback + parameter-set reinsertion

**Files:**
- Modify: `src/hevc_depayloader.cpp` (`flush_au`)
- Test: `tests/test_hevc_depayloader.cpp`

**Interfaces:**
- Consumes: `HevcDepayloader` from Tasks 1-3.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_hevc_depayloader.cpp`:

```cpp
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
```

- [ ] **Step 2: Run — verify the param-set test fails**

Run: `./build-test/rtp_depay_tests "[paramset]"`
Expected: FAIL — Frame 2 emits only 1 NAL (no reinsertion yet); `param_sets_reinserted == 0`. (The `[emit]` lost-marker case already passes from Task 1's `flush_au` on ts-change.)

- [ ] **Step 3: Implement reinsertion in `flush_au`**

Replace `flush_au` in `src/hevc_depayloader.cpp` with:

```cpp
void HevcDepayloader::flush_au() {
    if (au_has_data_ && !au_corrupt_) {
        const bool need_ps = au_has_irap_ &&
            !(au_has_vps_ && au_has_sps_ && au_has_pps_) &&
            !vps_.empty() && !sps_.empty() && !pps_.empty();
        if (need_ps) {
            static const uint8_t sc[4] = {0, 0, 0, 1};
            std::vector<uint8_t> out;
            out.reserve(au_.size() + vps_.size() + sps_.size() + pps_.size() + 12);
            auto add = [&](const std::vector<uint8_t>& n) {
                out.insert(out.end(), sc, sc + 4);
                out.insert(out.end(), n.begin(), n.end());
            };
            add(vps_); add(sps_); add(pps_);
            out.insert(out.end(), au_.begin(), au_.end());
            cb_(out.data(), out.size());
            stats_.param_sets_reinserted++;
        } else {
            cb_(au_.data(), au_.size());
        }
        stats_.aus_emitted++;
    }
    au_.clear();
    au_has_data_ = false;
    au_corrupt_ = false;
    au_has_irap_ = false;
    au_has_vps_ = au_has_sps_ = au_has_pps_ = false;
    have_ts_ = false;
    if (fu_active_) { stats_.fu_drops++; fu_active_ = false; fu_.clear(); }
}
```

- [ ] **Step 4: Run — verify pass**

Run: `make rtp_depay_tests && ./build-test/rtp_depay_tests "[emit],[paramset]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/hevc_depayloader.cpp tests/test_hevc_depayloader.cpp
git commit -m "feat(depay): lost-marker fallback + param-set reinsertion before IRAP"
```

---

### Task 5: Malformed-input hardening + fuzz harness

**Files:**
- Test: `tests/test_hevc_depayloader.cpp`
- Create: `tests/fuzz_hevc_depayloader.cpp`
- Modify: `CMakeLists.txt` (optional `rtp_depay_fuzz` target, guarded by a `BUILD_FUZZERS` option)

**Interfaces:**
- Consumes: `HevcDepayloader` from Tasks 1-4.

- [ ] **Step 1: Write malformed-input tests**

Append to `tests/test_hevc_depayloader.cpp`:

```cpp
TEST_CASE("short/empty payloads are dropped, never crash", "[depay][malformed]") {
    Sink sink;
    HevcDepayloader d(sink.cb());
    REQUIRE_FALSE(d.on_payload(nullptr, 0, true, 1));
    uint8_t one = 0x40;
    REQUIRE_FALSE(d.on_payload(&one, 1, true, 1));     // < 2-byte header
    std::vector<uint8_t> fu2 = { uint8_t(49 << 1), 0x01 };  // FU with no FU header
    REQUIRE_FALSE(d.on_payload(fu2.data(), fu2.size(), true, 1));
    std::vector<uint8_t> paci = { uint8_t(50 << 1), 0x01, 0x00 };  // PACI unsupported
    REQUIRE_FALSE(d.on_payload(paci.data(), paci.size(), true, 1));
    REQUIRE(d.stats().malformed >= 3);
    REQUIRE(sink.aus.empty());
}
```

- [ ] **Step 2: Run — verify pass (logic already bounds-checks)**

Run: `make rtp_depay_tests && ./build-test/rtp_depay_tests "[malformed]"`
Expected: PASS (Tasks 1-4 already bounds-check; this locks the behavior in). If any case crashes under ASan, fix the bounds check in the relevant `handle_*` before proceeding.

- [ ] **Step 3: Write the fuzz harness**

`tests/fuzz_hevc_depayloader.cpp`:

```cpp
#include <cstddef>
#include <cstdint>
#include "hevc_depayloader.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    HevcDepayloader d([](const uint8_t*, size_t) {});
    // Derive marker + ts from the first bytes, feed the rest as a payload.
    bool marker = size ? (data[0] & 1) : false;
    uint32_t ts = size >= 5 ? (uint32_t(data[1])<<24 | uint32_t(data[2])<<16 |
                               uint32_t(data[3])<<8  | data[4]) : 0;
    const uint8_t* payload = size > 5 ? data + 5 : data;
    size_t plen = size > 5 ? size - 5 : 0;
    d.on_payload(payload, plen, marker, ts);
    return 0;
}
```

- [ ] **Step 4: Add the fuzz target (optional, guarded)**

In `CMakeLists.txt`, near `rtp_depay_tests`:

```cmake
    option(BUILD_FUZZERS "Build libFuzzer targets" OFF)
    if(BUILD_FUZZERS)
      add_executable(rtp_depay_fuzz
        src/hevc_depayloader.cpp
        tests/fuzz_hevc_depayloader.cpp)
      target_include_directories(rtp_depay_fuzz PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR})
      target_compile_options(rtp_depay_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options(rtp_depay_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
```

- [ ] **Step 5: Smoke-run the fuzzer (clang only) and commit**

Run (if clang available): `nix-shell shell-sim.nix --run "cd build-test && cmake -DUSE_SIMULATOR=ON -DBUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++ .. && make rtp_depay_fuzz && ./rtp_depay_fuzz -max_total_time=30"`
Expected: no crashes in 30 s. (If clang/libFuzzer is unavailable, skip the run — the harness still compiles in CI when enabled.)

```bash
git add tests/test_hevc_depayloader.cpp tests/fuzz_hevc_depayloader.cpp CMakeLists.txt
git commit -m "test(depay): malformed-input hardening + libFuzzer harness"
```

---

# Phase 2 — `video_codec.h` + `RtpVideoReceiver`

This phase builds the live receiver. It compiles in the device build only (sockets are host-fine, but it pulls in spdlog and the IDR/restream code). Verify each task with the **device** test build:

```bash
nix-shell --run "mkdir -p build && cd build && cmake -DBUILD_TESTS=ON .. && make pixelpilot_tests"
```

---

### Task 6: Extract `VideoCodec` into `src/video_codec.h`

**Files:**
- Create: `src/video_codec.h`
- Modify: `src/gstrtpreceiver.h` (remove the enum + helper, include the new header)

**Interfaces:**
- Produces: `enum class VideoCodec { UNKNOWN=0, H264, H265 };` and `inline VideoCodec video_codec(const char*)` in `src/video_codec.h`.

- [ ] **Step 1: Create `src/video_codec.h`**

```cpp
#ifndef VIDEO_CODEC_H
#define VIDEO_CODEC_H

#include <cstring>

enum class VideoCodec {
    UNKNOWN = 0,
    H264,
    H265
};

inline VideoCodec video_codec(const char* str) {
    if (!strcmp(str, "h264")) return VideoCodec::H264;
    if (!strcmp(str, "h265")) return VideoCodec::H265;
    return VideoCodec::UNKNOWN;
}

#endif // VIDEO_CODEC_H
```

- [ ] **Step 2: Replace the inline definitions in `gstrtpreceiver.h`**

In `src/gstrtpreceiver.h`, delete the `enum class VideoCodec {...}` and the `static VideoCodec video_codec(...)` function (lines ~23-37) and add near the top includes:

```cpp
#include "video_codec.h"
```

- [ ] **Step 3: Build to verify nothing broke**

Run: `nix-shell --run "cd build && cmake -DBUILD_TESTS=ON .. && make pixelpilot_tests"`
Expected: PASS (compiles; `VideoCodec` now resolves via the new header).

- [ ] **Step 4: Commit**

```bash
git add src/video_codec.h src/gstrtpreceiver.h
git commit -m "refactor: extract VideoCodec enum into src/video_codec.h"
```

---

### Task 7: Create `rtp_video_receiver.{h,cpp}` — move side-channels + C-API off gst

**Files:**
- Create: `src/rtp_video_receiver.h`
- Create: `src/rtp_video_receiver.cpp`
- Modify: `CMakeLists.txt` (add the two files to `LIB_SOURCE_FILES`)

This task moves the side-channel machinery from `gstrtpreceiver.cpp` into the new module, swapping the gst-specific I/O for plain sockets, but **does not yet wire up `main.cpp`** (that is Task 9). The old `gstrtpreceiver.cpp` keeps compiling until Task 9.

**Interfaces:**
- Consumes: `HevcDepayloader` (Phase 1), `latency_probe::on_rtp_buffer/now_us/active` (`src/latency_probe.hpp`).
- Produces:
  - `class RtpVideoReceiver` with `using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;`, ctors `RtpVideoReceiver(int udp_port)` and `RtpVideoReceiver(const char* unix_sock)`, `void start(NEW_FRAME_CALLBACK)`, `void stop()`.
  - The C-API (unchanged signatures): `idr_set_enabled/idr_get_enabled/restream_set_enabled/restream_get_enabled/restream_scan_clients/restream_set_manual_ip/restream_get_manual_ip/restream_set_pinned_ip/idr_request_record_start/idr_request_decoder_issue/idr_notify_decoded_frame`.

- [ ] **Step 1: Create the header**

`src/rtp_video_receiver.h`:

```cpp
#ifndef RTP_VIDEO_RECEIVER_H
#define RTP_VIDEO_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "video_codec.h"
#include "hevc_depayloader.h"

// Custom, single-threaded HEVC RTP receiver that replaces the GStreamer live path.
// Receives RTP datagrams (UDP or abstract AF_UNIX), depayloads to Annex-B access
// units, and invokes the frame callback once per frame (feeding MPP + DVR record).
class RtpVideoReceiver {
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;

    explicit RtpVideoReceiver(int udp_port);       // UDP source (production: 5600)
    explicit RtpVideoReceiver(const char* unix_sock); // abstract AF_UNIX dgram (compat)
    ~RtpVideoReceiver();

    void start(NEW_FRAME_CALLBACK on_frame);
    void stop();

private:
    void recv_loop();
    void open_socket();

    int m_port = -1;
    std::string m_unix_socket;       // empty => UDP
    int m_sock = -1;
    int m_restream_sock = -1;
    std::atomic<bool> m_run{false};
    std::thread m_thread;
    NEW_FRAME_CALLBACK m_cb;
    HevcDepayloader m_depay;
    uint16_t m_last_seq = 0;
    bool m_have_seq = false;
};

#ifdef __cplusplus
extern "C" {
#endif
void idr_set_enabled(bool enabled);
bool idr_get_enabled();
void restream_set_enabled(bool enabled);
bool restream_get_enabled();
void restream_scan_clients(char* buf, size_t buf_len);
void restream_set_manual_ip(const char* ip);
const char* restream_get_manual_ip();
void restream_set_pinned_ip(const char* ip);
void idr_request_record_start();
void idr_request_decoder_issue(const char* reason);
void idr_notify_decoded_frame();
#ifdef __cplusplus
}
#endif

#endif // RTP_VIDEO_RECEIVER_H
```

- [ ] **Step 2: Create the .cpp — move the pure side-channel code verbatim**

Create `src/rtp_video_receiver.cpp`. Copy **unchanged** from `gstrtpreceiver.cpp` the entire anonymous-namespace block of pure helpers and the C-API implementations — these have **no gst dependency** and compile as-is:

- The constants block (`kIdrUdpPort`…`kRtpSeqResetMs`) and all `g_*` atomics/mutexes/strings.
- `now_ms`, `contains_ip`, `is_stream_idr_reason`, `is_record_idr_reason`, `ensure_idr_socket`.
- `scan_hotspot_clients`, `find_first_hotspot_client_ip`.
- `secure_random_u32`, `make_idr_token3`, `send_idr_token_to_ip`, `send_idr_burst`, `request_idr_bursts`.
- `has_idr_frame`, `for_each_nal`, `maybe_mark_idr_received`.
- `maybe_request_decode_stall`, `tick_stream_presence`, `reset_stream_tracking`, `maybe_request_idr_rate_limited`.
- The C-API: `idr_set_enabled/idr_get_enabled/idr_request_record_start/idr_request_decoder_issue/idr_notify_decoded_frame`, and `restream_set_enabled/restream_get_enabled/restream_scan_clients/restream_set_manual_ip/restream_get_manual_ip/restream_set_pinned_ip`.

Add the includes at the top:

```cpp
#include "rtp_video_receiver.h"
#include "latency_probe.hpp"
#include "spdlog/spdlog.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#if defined(__linux__)
#include <sys/random.h>
#endif
```

- [ ] **Step 3: Rewrite the restream control to use a UDP socket (replaces gst valve/udpsink)**

In the moved code, the restream functions referenced gst (`g_restream_valve`, `g_restream_sink`, `g_object_set`). Replace them with socket state. Add these near the other `g_restream_*` globals and replace `update_restream_valve`, `maybe_update_restream_target`, `clear_restream_valve`, `set_restream_valve_locked`, `bind_restream_valve` with:

```cpp
namespace {
    static std::atomic<bool> g_restream_open{false};        // valve "drop=false" equivalent
    static int g_restream_fd = -1;                          // set by the receiver
    static sockaddr_in g_restream_dst{};                    // current target

    static void restream_set_fd(int fd) { g_restream_fd = fd; }

    static void set_restream_open_locked(bool open) { g_restream_open.store(open, std::memory_order_relaxed); }

    static void update_restream_valve(bool enabled) {
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (!enabled) set_restream_open_locked(false);
    }

    static void maybe_update_restream_target(bool force) {
        static uint64_t last_probe_ms = 0;
        const uint64_t now = now_ms();
        if (!force && (now - last_probe_ms) < 1000) return;
        last_probe_ms = now;

        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (!g_restream_enabled.load(std::memory_order_relaxed)) { set_restream_open_locked(false); return; }
        const std::string next_ip = !g_restream_manual_ip.empty()
            ? g_restream_manual_ip : find_first_hotspot_client_ip();
        if (next_ip.empty()) {
            if (!g_restream_target_ip.empty()) {
                spdlog::info("[RESTREAM] No target client; stopping unicast restream");
                g_restream_target_ip.clear();
            }
            set_restream_open_locked(false);
            return;
        }
        if (next_ip != g_restream_target_ip) {
            g_restream_target_ip = next_ip;
            std::memset(&g_restream_dst, 0, sizeof(g_restream_dst));
            g_restream_dst.sin_family = AF_INET;
            g_restream_dst.sin_port = htons(5600);
            inet_pton(AF_INET, g_restream_target_ip.c_str(), &g_restream_dst.sin_addr);
            spdlog::info("[RESTREAM] Streaming to {}:{}", g_restream_target_ip, 5600);
        }
        set_restream_open_locked(true);
    }

    // Called from the recv loop for every datagram.
    static void restream_forward(const uint8_t* data, size_t len) {
        if (!g_restream_open.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> lock(g_restream_mutex);
        if (g_restream_fd < 0 || g_restream_dst.sin_family != AF_INET) return;
        sendto(g_restream_fd, data, len, 0,
               reinterpret_cast<sockaddr*>(&g_restream_dst), sizeof(g_restream_dst));
    }
}
```

In the moved `restream_set_enabled`, keep the body calling `update_restream_valve(enabled)` (unchanged). The `restream_scan_clients/set_manual_ip/get_manual_ip/set_pinned_ip` move verbatim.

- [ ] **Step 4: Rewrite the per-packet hook to take raw bytes + sender (replaces GstBuffer)**

Replace the gst `on_incoming_stream_buffer(GstBuffer*, tag)` / `maybe_update_last_hop_from_buffer` / `maybe_track_rtp_sequence` / `extract_sender_ip_from_buffer` / `extract_rtp_sequence` with byte/sockaddr versions:

```cpp
namespace {
    static void update_last_hop_ip(const sockaddr_in& from) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) return;
        char ip[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) return;
        std::lock_guard<std::mutex> lock(g_last_hop_mutex);
        if (g_last_hop_ip != ip) { g_last_hop_ip = ip; spdlog::info("[NET] Last-hop sender: {}", g_last_hop_ip); }
    }

    static void on_incoming_stream_bytes(const sockaddr_in& from, const char* tag) {
        if (!g_idr_enabled.load(std::memory_order_relaxed)) return;
        g_last_pkt_ms.store(now_ms(), std::memory_order_relaxed);
        update_last_hop_ip(from);
        if (!g_stream_up.exchange(true)) {
            spdlog::info("[NET] Stream UP ({})", tag ? tag : "unknown");
            request_idr_bursts("stream-up", kIdrRepeatCount, false);
        }
        if (g_pending_rec_idr.load(std::memory_order_relaxed)) {
            if (!g_record_idr_pending.load(std::memory_order_relaxed)) {
                g_pending_rec_idr.store(false, std::memory_order_relaxed);
            } else if (!get_last_hop_ip_copy().empty()) {
                g_pending_rec_idr.store(false, std::memory_order_relaxed);
                request_idr_bursts("record-start(pending)", kIdrRecordRepeatCount, false);
            }
        }
    }

    // RTP seq is bytes 2-3 of the datagram. Returns true if a gap was detected.
    static bool track_rtp_sequence(const uint8_t* rtp, size_t len, uint16_t& last_seq, bool& have_seq) {
        if (!g_idr_enabled.load(std::memory_order_relaxed) || len < 4) return false;
        const uint16_t seq = uint16_t((rtp[2] << 8) | rtp[3]);
        bool gap = false;
        if (have_seq) {
            const uint16_t diff = uint16_t(seq - last_seq);
            if (diff != 0 && diff < 30000 && diff > 1) {
                gap = true;
                maybe_request_idr_for_rtp_gap(uint16_t(diff - 1));
            }
        }
        last_seq = seq; have_seq = true;
        return gap;
    }
}
```

Move `maybe_request_idr_for_rtp_gap` and `get_last_hop_ip_copy` verbatim from `gstrtpreceiver.cpp`.

- [ ] **Step 5: Implement the receiver class (socket + recv loop)**

Append to `src/rtp_video_receiver.cpp`:

```cpp
RtpVideoReceiver::RtpVideoReceiver(int udp_port)
    : m_port(udp_port),
      m_depay([this](const uint8_t* au, size_t len) {
          auto buf = std::make_shared<std::vector<uint8_t>>(au, au + len);
          if (au && len) maybe_mark_idr_received(au, len, VideoCodec::H265);
          if (m_cb) m_cb(buf);
      }) {}

RtpVideoReceiver::RtpVideoReceiver(const char* unix_sock)
    : m_unix_socket(unix_sock ? unix_sock : ""),
      m_depay([this](const uint8_t* au, size_t len) {
          auto buf = std::make_shared<std::vector<uint8_t>>(au, au + len);
          if (au && len) maybe_mark_idr_received(au, len, VideoCodec::H265);
          if (m_cb) m_cb(buf);
      }) {}

RtpVideoReceiver::~RtpVideoReceiver() { stop(); }

void RtpVideoReceiver::open_socket() {
    if (m_unix_socket.empty()) {
        m_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0) throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
        int reuse = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int rcvbuf = 4 * 1024 * 1024;  // absorb bursts during transient MPP stalls
        setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(uint16_t(m_port));
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    } else {
        m_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (m_sock < 0) throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';  // abstract namespace
        strncpy(addr.sun_path + 1, m_unix_socket.c_str(), sizeof(addr.sun_path) - 2);
        socklen_t addr_len = sizeof(addr.sun_family) + 1 + m_unix_socket.size();
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), addr_len) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }
    // Recv timeout so the loop can observe stop().
    timeval tv{0, 200 * 1000};
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_restream_sock = socket(AF_INET, SOCK_DGRAM, 0);
    restream_set_fd(m_restream_sock);
}

void RtpVideoReceiver::start(NEW_FRAME_CALLBACK on_frame) {
    m_cb = std::move(on_frame);
    open_socket();
    m_run.store(true);
    m_thread = std::thread([this]() {
        pthread_setname_np(pthread_self(), "rtp-recv");
        recv_loop();
    });
}

void RtpVideoReceiver::stop() {
    m_run.store(false);
    if (m_thread.joinable()) m_thread.join();
    if (m_sock >= 0) { close(m_sock); m_sock = -1; }
    if (m_restream_sock >= 0) { restream_set_fd(-1); close(m_restream_sock); m_restream_sock = -1; }
    reset_stream_tracking();
    m_depay.reset();
    m_have_seq = false;
}

void RtpVideoReceiver::recv_loop() {
    std::vector<uint8_t> buf(MAX_PACKET_SIZE);
    while (m_run.load()) {
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(m_sock, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                tick_stream_presence();
                maybe_update_restream_target(false);
                continue;
            }
            spdlog::warn("[RTP] recvfrom error: {}", strerror(errno));
            continue;
        }
        if (n <= RTP_HEADER_LEN) { continue; }
        const uint8_t* rtp = buf.data();
        const size_t len = size_t(n);

        if (latency_probe::active.load(std::memory_order_acquire))
            latency_probe::on_rtp_buffer(rtp, len, latency_probe::now_us());

        on_incoming_stream_bytes(from, "udpsrc");
        const bool gap = track_rtp_sequence(rtp, len, m_last_seq, m_have_seq);
        if (gap) m_depay.on_discontinuity();

        restream_forward(rtp, len);

        // RTP header (no CSRC/extension assumed): marker = bit7 of byte 1, ts = bytes 4-7.
        const bool marker = (rtp[1] & 0x80) != 0;
        const uint32_t ts = (uint32_t(rtp[4]) << 24) | (uint32_t(rtp[5]) << 16) |
                            (uint32_t(rtp[6]) << 8) | uint32_t(rtp[7]);
        m_depay.on_payload(rtp + RTP_HEADER_LEN, len - RTP_HEADER_LEN, marker, ts);

        tick_stream_presence();
        maybe_update_restream_target(false);
    }
}
```

Add `#define MAX_PACKET_SIZE 4096` and `#define RTP_HEADER_LEN 12` near the top of the .cpp (or include from a shared header) — these match the old `gstrtpreceiver.h` values.

- [ ] **Step 6: Register only the depayloader in `LIB_SOURCE_FILES`; defer the receiver to Task 8**

In `CMakeLists.txt`, add to the `LIB_SOURCE_FILES` list (next to `src/gstrtpreceiver.cpp`):

```cmake
      src/hevc_depayloader.cpp
      src/hevc_depayloader.h
```

Do **not** add `rtp_video_receiver.*` to CMake yet — its C-API symbols would collide with the still-present `gstrtpreceiver.cpp`. Task 8 strips the old file **and** registers `rtp_video_receiver.*` in the same commit, so the symbols move atomically (no duplicate-definition window).

- [ ] **Step 7: Commit (these files are compiled in Task 8)**

```bash
git add src/rtp_video_receiver.h src/rtp_video_receiver.cpp CMakeLists.txt
git commit -m "feat(rtp): add RtpVideoReceiver (recv loop + side-channels); CMake wiring lands in Task 8"
```

> **Tasks 7-9 are a refactor sequence.** `pixelpilot_tests` always includes `main.cpp`, so the full **link** is green only at Task 9. Tasks 7-8 are verified by per-file compilation; the green-link gate is Task 9. If your executor enforces a green build at every task boundary, run Tasks 7-8-9 as one reviewable unit.

---

# Phase 3 — Slim gst to DVR, integrate, delete H264 decode

### Task 8: Convert `gstrtpreceiver` → `gst_file_player` (DVR only)

**Files:**
- Rename: `git mv src/gstrtpreceiver.cpp src/gst_file_player.cpp`; `git mv src/gstrtpreceiver.h src/gst_file_player.h`
- Modify: both renamed files; `CMakeLists.txt`

**Interfaces:**
- Produces: `class GstFilePlayer` with `using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;`, ctor `GstFilePlayer(VideoCodec /*unused, always H265*/)` or default, `void start(const char* file_path, NEW_FRAME_CALLBACK)`, `void stop()`, `void fast_forward(double rate=2.0)`, `void fast_rewind(double rate=2.0)`, `void normal_playback()`, `void skip_duration(int64_t)`, `void pause()`, `void resume()`.

- [ ] **Step 1: Rename the files**

```bash
git mv src/gstrtpreceiver.cpp src/gst_file_player.cpp
git mv src/gstrtpreceiver.h src/gst_file_player.h
```

- [ ] **Step 2: Strip the live path + side-channels from `gst_file_player.h`**

Rename the class to `GstFilePlayer`. Keep only the DVR surface. The header becomes:

```cpp
#ifndef GST_FILE_PLAYER_H
#define GST_FILE_PLAYER_H

#include <stdint.h>
#ifndef USE_SIMULATOR
#include <gst/gst.h>
#endif
#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include "video_codec.h"

#define MAX_PACKET_SIZE 4096
#define RTP_HEADER_LEN 12

// GStreamer-backed DVR mp4 file player (H265). Decodes filesrc->qtdemux->h265parse
// ->appsink, emitting Annex-B access units via the callback, with transport controls.
class GstFilePlayer {
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
    GstFilePlayer();
    ~GstFilePlayer();
    // Returns the detected codec (always H265 in production recordings).
    VideoCodec start(const char* file_path, NEW_FRAME_CALLBACK cb);
    void stop();
    void fast_forward(double rate = 2.0);
    void fast_rewind(double rate = 2.0);
    void normal_playback();
    void skip_duration(int64_t skip_ms);
    void pause();
    void resume();
private:
    std::string construct_file_playback_pipeline(const char* file_path);
    void loop_pull_samples();
    void set_playback_rate(double rate);
    GstElement* m_gst_pipeline = nullptr;
    GstElement* m_app_sink_element = nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_playback_codec = VideoCodec::H265;
    bool m_pull_samples_run = false;
    std::unique_ptr<std::thread> m_pull_samples_thread;
    double m_playback_rate = 1.0;
    bool m_is_paused = false;
    double m_pre_pause_rate = 1.0;
};

#endif // GST_FILE_PLAYER_H
```

Delete the `idr_*` / `restream_*` `extern "C"` block from this header (it now lives in `rtp_video_receiver.h`).

- [ ] **Step 3: Strip `gst_file_player.cpp` to the DVR essentials**

In `src/gst_file_player.cpp`:
- Delete the entire anonymous-namespace side-channel block and all the C-API function definitions (now in `rtp_video_receiver.cpp`).
- Delete the live-path methods: `construct_gstreamer_pipeline`, `switch_to_stream`, `switch_to_file_playback`'s stream half, `start_receiving`, `stop_receiving`, the appsrc/buffer-pool/socket-reader code, `attach_last_hop_probes`, `udp_last_hop_probe`, `bind_restream_valve`, `create_restream_branch`, `on_new_sample`'s IDR hook.
- Keep `detect_mp4_codec` but **remove its H264 branch** (return H265 or UNKNOWN only). Keep `construct_file_playback_pipeline` but hardcode `h265parse` and `video/x-h265,...alignment=au`.
- Rename the surviving methods to the `GstFilePlayer` class and adapt `start()` to launch the file pipeline + pull thread (lifted from the old `switch_to_file_playback`), and `stop()` to tear it down (lifted from `stop_receiving`'s pipeline teardown).
- The pull loop keeps emitting via `m_cb` but **drops** the `on_incoming_stream_buffer`/`maybe_mark_idr_received` calls (those are live-only).

`construct_file_playback_pipeline` becomes:

```cpp
std::string GstFilePlayer::construct_file_playback_pipeline(const char* file_path) {
    m_playback_codec = VideoCodec::H265;
    std::stringstream ss;
    ss << "filesrc location=" << file_path << " ! qtdemux ! ";
    ss << "h265parse config-interval=-1 ! ";
    ss << "video/x-h265, stream-format=\"byte-stream\", alignment=au ! ";
    ss << "appsink drop=true name=out_appsink";
    return ss.str();
}
```

- [ ] **Step 4: Update CMake source names + DROP gstnet/gio**

In `CMakeLists.txt`:
- In `LIB_SOURCE_FILES`, replace `src/gstrtpreceiver.cpp`/`src/gstrtpreceiver.h` with `src/gst_file_player.cpp`/`src/gst_file_player.h`, **and add** `src/rtp_video_receiver.cpp`/`src/rtp_video_receiver.h` (deferred from Task 7). The old file's C-API symbols disappear and the new file's appear in the same commit — no duplicate-definition window.
- In the **device** link block (lines ~411-414) remove `PkgConfig::gstnet` and `PkgConfig::gio`, and remove the `pkg_search_module(gstnet ...)` / `pkg_search_module(gio ...)` lines (371-372).
- In the **pixelpilot_tests** block remove `PkgConfig::gstnet PkgConfig::gio` from line 468 and the `pkg_search_module(gstnet ...)`/`pkg_search_module(gio ...)` at 466-467.

- [ ] **Step 5: Build (still references old class in main.cpp — expect main.cpp errors only)**

Run: `nix-shell --run "cd build && cmake -DBUILD_TESTS=ON .. && make pixelpilot_tests 2>&1 | tail -30"`
Expected: `gst_file_player.cpp` + `rtp_video_receiver.cpp` compile and no duplicate symbols; remaining errors are all in `main.cpp` (`GstRtpReceiver` undefined) — fixed in Task 9.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: slim GstRtpReceiver into DVR-only GstFilePlayer; drop gstnet/gio"
```

---

### Task 9: Wire `main.cpp` to the new receiver + delete MPP reinit

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/main.h` (only if playback decls reference the receiver type — they do not; no change expected)
- Modify: any gsmenu file that `#include "gstrtpreceiver.h"`

**Interfaces:**
- Consumes: `RtpVideoReceiver` (Task 7), `GstFilePlayer` (Task 8).

- [ ] **Step 1: Repoint includes**

Run: `grep -rl 'gstrtpreceiver.h' src` then in each hit replace `#include "gstrtpreceiver.h"` (or `"...gstrtpreceiver.h"`) with both:
```cpp
#include "rtp_video_receiver.h"
#include "gst_file_player.h"
```
In `main.cpp` specifically (line ~55), make this replacement.

- [ ] **Step 2: Swap the globals + delete reinit machinery**

In `src/main.cpp`:
- Line 801: replace `std::unique_ptr<GstRtpReceiver> receiver;` with the two owners plus
  the shared callback + decode-packet globals (declared here, before `switch_pipeline_source`
  at line ~833, so it can see them):
```cpp
std::unique_ptr<RtpVideoReceiver> receiver;
std::unique_ptr<GstFilePlayer>    file_player;
RtpVideoReceiver::NEW_FRAME_CALLBACK g_video_frame_cb;  // shared live/DVR frame sink
static MppPacket* g_decode_packet = nullptr;            // set before main_loop()
```
- Delete the reinit globals (lines 108-109, 802-803): `mpp_reinit_mutex`, `mpp_reinit_pending`, `current_mpp_type`, `stream_mpp_type`.
- Delete `reinit_mpp_decoder` entirely (lines 805-831).
- Delete `set_mpp_decoding_parameters` forward decl reference to reinit if any (keep `set_mpp_decoding_parameters` itself).

- [ ] **Step 3: Simplify the decode thread**

In `__FRAME_THREAD__` (lines ~302-323), remove the reinit coordination so the loop body starts:

```cpp
    while (!frm_eos) {
        ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
        if (frame) {
            if (mpp_frame_get_info_change(frame)) {
                init_buffer(frame);
            } else {
                idr_notify_decoded_frame();
                // ... existing errinfo/discard + display logic unchanged ...
```

Delete the `pthread_mutex_lock(&mpp_reinit_mutex)` / `mpp_reinit_pending` check block (lines 306-322).

- [ ] **Step 4: Fix MPP init to HEVC-only**

Replace lines 1528-1533:

```cpp
    MppCodingType mpp_type = MPP_VIDEO_CodingHEVC;
    if (codec != VideoCodec::H265) {
        spdlog::warn("Live path is H265-only; ignoring --codec (decoder forced to HEVC)");
    }
```

(Leaves `mpp_create`/`mpp_init(ctx, MPP_CTX_DEC, mpp_type)` at 1572-1577 unchanged — `mpp_type` is now always HEVC.)

- [ ] **Step 5: Rewrite `switch_pipeline_source` + playback controls**

Replace the `switch_pipeline_source` body (lines 833-845) and the playback control bodies (847-869):

```cpp
void switch_pipeline_source(const char * source_type, const char * source_path) {
    if (strcmp(source_type, "file") == 0) {
        if (receiver) receiver->stop();
        file_player = std::make_unique<GstFilePlayer>();
        file_player->start(source_path, g_video_frame_cb);
    } else if (strcmp(source_type, "stream") == 0) {
        if (file_player) { file_player->stop(); file_player.reset(); }
        if (receiver) receiver->start(g_video_frame_cb);
    } else {
        spdlog::error("Unknown source type: {}", source_type);
    }
}

void fast_forward(double rate){ if (file_player) file_player->fast_forward(); }
void fast_rewind(double rate){ if (file_player) file_player->fast_rewind(); }
void skip_duration(int64_t skip_ms){ if (file_player) file_player->skip_duration(skip_ms); }
void normal_playback() { if (file_player) file_player->normal_playback(); }
void pause_playback() { if (file_player) file_player->pause(); }
void resume_playback() { if (file_player) file_player->resume(); }
```

This references a shared `g_video_frame_cb` — define it (Step 6).

- [ ] **Step 6: Swap receiver construction + assign the shared callback**

Replace `read_gstreamerpipe_stream` (lines 1000-1047). The callback is assigned to the
file-scope `g_video_frame_cb` (declared in Step 2) and captures **nothing** — it reaches the
MPP packet through the file-scope `g_decode_packet`, so it stays valid when
`switch_pipeline_source` re-arms it on a DVR toggle (capturing `&packet` by reference would
dangle):

```cpp
void read_gstreamerpipe_stream(MppPacket *packet, int gst_udp_port, const char *sock, const VideoCodec& codec){
    (void)codec;  // live path is H265-only
    g_decode_packet = packet;
    if (sock) receiver = std::make_unique<RtpVideoReceiver>(sock);
    else      receiver = std::make_unique<RtpVideoReceiver>(gst_udp_port);

    g_video_frame_cb = [](std::shared_ptr<std::vector<uint8_t>> frame){
        osd_publish_uint_fact("gstreamer.received_bytes", NULL, 0, frame->size());
        const bool fed_ok = feed_packet_to_decoder(g_decode_packet, frame->data(), frame->size());
        static int stall_count = 0;
        static uint64_t last_stall_idr_ms = 0;
        const uint64_t now = get_time_ms();
        if (!fed_ok) {
            if (++stall_count >= 3 && (now - last_stall_idr_ms) > 500) {
                last_stall_idr_ms = now; stall_count = 0;
                idr_request_decoder_issue("decoder-feed-stall");
            }
        } else stall_count = 0;
        if (dvr_enabled && dvr_raw != NULL) dvr_raw->frame(frame);
    };

    receiver->start(g_video_frame_cb);
    main_loop();
    receiver->stop();
    spdlog::info("Feeding eos");
    mpp_packet_set_eos(packet);
    mpp_packet_set_length(packet, 0);
    int ret = 0;
    while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) usleep(10000);
}
```

Note: `set_jitter_ms` is dropped (no jitter buffer on the live path). `--rtp-jitter-ms` may
stay parsed but is now ignored — emit a one-time `spdlog::info` if `video_rtp_jitter_ms != 0`.
The `gstreamer.received_bytes` fact name is **kept** (osd.cpp matches it).

- [ ] **Step 7: Build the device tests**

Run: `nix-shell --run "cd build && cmake -DBUILD_TESTS=ON .. && make pixelpilot_tests"`
Expected: PASS (links cleanly; no `GstRtpReceiver`, no `gstnet`/`gio`).

- [ ] **Step 8: Run existing tests to confirm no regression**

Run: `./build/pixelpilot_tests`
Expected: PASS (all existing osd/stutter/latency tests green).

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat: wire main.cpp to RtpVideoReceiver + GstFilePlayer; delete MPP reinit (H265-only)"
```

---

# Phase 4 — Golden parity + on-target validation

### Task 10: Golden parity fixture + test

**Files:**
- Create: `tools/gen_hevc_golden.py`
- Create: `tests/files/hevc_capture.bin`, `tests/files/hevc_golden.bin` (generated artifacts, checked in)
- Modify: `tests/test_hevc_depayloader.cpp` (golden replay test)

**Interfaces:**
- Consumes: `HevcDepayloader` (Phase 1).

- [ ] **Step 1: Write the capture+golden generator**

`tools/gen_hevc_golden.py` — run ONCE on a host that can see the stream while it flows
(stop fpvd first so udp/5600 is free). It (a) binds UDP 5600 and records raw RTP packets to
`hevc_capture.bin` (`[u32 len][packet]`, little-endian), and (b) replays them through a gst
`appsrc ! rtph265depay ! h265parse ! appsink` reference, writing each emitted Annex-B access
unit to `hevc_golden.bin` (`[u32 len][au]`). The capture step needs only plain sockets; the
golden step needs `python3-gi` + gst plugins (run it elsewhere via `--golden-only` if needed).

```python
#!/usr/bin/env python3
"""Generate golden depayloader fixtures from a live HEVC RTP stream on udp/5600.

  tests/files/hevc_capture.bin : repeated [u32 len][len bytes raw RTP packet]
  tests/files/hevc_golden.bin  : repeated [u32 len][len bytes Annex-B access unit]

Run with fpvd/pixelpilot stopped (udp/5600 free) while the air unit streams:
  ./tools/gen_hevc_golden.py [--frames 400] [--golden-only]
The golden step needs python3-gi + gst1.0 plugins (good/bad).
"""
import argparse, os, socket, struct, sys

CAP = "tests/files/hevc_capture.bin"
GOLD = "tests/files/hevc_golden.bin"

def capture(frames):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 5600))
    s.settimeout(5.0)
    pkts, markers = [], 0
    while markers < frames:
        try:
            d = s.recv(65535)
        except socket.timeout:
            print("timeout on udp/5600 (air unit streaming? fpvd stopped?)", file=sys.stderr)
            break
        if len(d) < 12 or (d[0] >> 6) != 2:
            continue
        pkts.append(d)
        if d[1] & 0x80:
            markers += 1
    os.makedirs(os.path.dirname(CAP), exist_ok=True)
    with open(CAP, "wb") as f:
        for p in pkts:
            f.write(struct.pack("<I", len(p))); f.write(p)
    print(f"wrote {CAP}: {len(pkts)} packets, {markers} frames")

def build_golden():
    import gi
    gi.require_version("Gst", "1.0")
    from gi.repository import Gst
    Gst.init(None)
    pipe = Gst.parse_launch(
        "appsrc name=src is-live=false format=time ! "
        "application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=97 ! "
        "rtph265depay ! h265parse config-interval=-1 ! "
        "video/x-h265,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink sync=false")
    src = pipe.get_by_name("src"); sink = pipe.get_by_name("sink")
    pipe.set_state(Gst.State.PLAYING)
    with open(CAP, "rb") as f:
        data = f.read()
    off, ts = 0, 0
    while off + 4 <= len(data):
        (n,) = struct.unpack_from("<I", data, off); off += 4
        pkt = data[off:off+n]; off += n
        buf = Gst.Buffer.new_allocate(None, len(pkt), None)
        buf.fill(0, pkt); buf.pts = ts
        if len(pkt) >= 2 and (pkt[1] & 0x80):
            ts += Gst.SECOND // 60
        src.emit("push-buffer", buf)
    src.emit("end-of-stream")
    aus = []
    while True:
        sample = sink.try_pull_sample(Gst.SECOND)
        if sample is None:
            break
        b = sample.get_buffer()
        ok, info = b.map(Gst.MapFlags.READ)
        if ok:
            aus.append(bytes(info.data)); b.unmap(info)
    pipe.set_state(Gst.State.NULL)
    with open(GOLD, "wb") as f:
        for au in aus:
            f.write(struct.pack("<I", len(au))); f.write(au)
    print(f"wrote {GOLD}: {len(aus)} access units")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--golden-only", action="store_true")
    a = ap.parse_args()
    if not a.golden_only:
        capture(a.frames)
    build_golden()
```

This is a one-time fixture generator, not part of CI. Commit the generated `.bin` fixtures so the test below runs anywhere with zero gst.

- [ ] **Step 2: Write the golden replay test**

Append to `tests/test_hevc_depayloader.cpp`:

```cpp
#include <cstdio>
TEST_CASE("golden: depayloader output matches gst reference NAL sequence", "[depay][golden]") {
    FILE* cap = std::fopen("tests/files/hevc_capture.bin", "rb");
    FILE* gold = std::fopen("tests/files/hevc_golden.bin", "rb");
    if (!cap || !gold) { if(cap)fclose(cap); if(gold)fclose(gold); SKIP("golden fixtures not present"); }

    // Replay capture through the depayloader, collecting AUs.
    std::vector<std::vector<uint8_t>> got;
    HevcDepayloader d([&](const uint8_t* p, size_t n){ got.emplace_back(p, p+n); });
    uint32_t plen;
    while (std::fread(&plen,4,1,cap)==1) {              // little-endian length
        std::vector<uint8_t> pkt(plen);
        if (plen && std::fread(pkt.data(),1,plen,cap)!=plen) break;
        if (pkt.size() < 12) continue;
        const bool marker = (pkt[1] & 0x80) != 0;
        const uint32_t ts = (uint32_t(pkt[4])<<24)|(uint32_t(pkt[5])<<16)|
                            (uint32_t(pkt[6])<<8)|pkt[7];
        d.on_payload(pkt.data()+12, pkt.size()-12, marker, ts);
    }
    // Read golden AUs.
    std::vector<std::vector<uint8_t>> want;
    uint32_t alen;
    while (std::fread(&alen,4,1,gold)==1) {
        std::vector<uint8_t> au(alen);
        if (alen && std::fread(au.data(),1,alen,gold)!=alen) break;
        want.emplace_back(std::move(au));
    }
    std::fclose(cap); std::fclose(gold);

    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); ++i) {
        // Compare NAL sequence (type+body), tolerating start-code length differences.
        REQUIRE(split_nals(got[i]) == split_nals(want[i]));
    }
}
```

- [ ] **Step 3: Run — with fixtures present, verify pass; without, verify SKIP**

Run: `./build-test/rtp_depay_tests "[golden]"`
Expected: PASS if `tests/files/hevc_*.bin` exist; otherwise reported as SKIPPED (not failed).

- [ ] **Step 4: Commit**

```bash
git add tools/gen_hevc_golden.py tests/test_hevc_depayloader.cpp tests/files/hevc_capture.bin tests/files/hevc_golden.bin
git commit -m "test(depay): golden parity vs gst reference (captured fixture)"
```

---

### Task 11: On-target validation checklist (manual, pre-flight)

**Files:** none (validation only). Record results in the PR description.

- [ ] **Step 1: Build + deploy to the GS test unit** per the GS deploy workflow (cross-build, deploy to `/tmp` first).
- [ ] **Step 2: Latency A/B** — with the latency probe enabled, capture `video.latency.*` + stutter facts on the new binary vs the current gst binary on the same link. Confirm latency is improved or not worse, and not noisier.
- [ ] **Step 3: IDR recovery** — induce packet loss (e.g. brief antenna detune); confirm the stream recovers (gap→IDR path fires, decode resumes).
- [ ] **Step 4: Restream** — enable restream to a phone/second screen; confirm video arrives and retargets when the client changes.
- [ ] **Step 5: DVR** — record a clip (raw H265), then play it back with seek / fast-forward / pause; confirm `GstFilePlayer` works.
- [ ] **Step 6: Soak** — run ≥30 min continuous; confirm no socket-drop stalls (watch `SO_RCVBUF` behavior under any decoder hiccup) and no leaks.
- [ ] **Step 7: Record outcomes** in the PR; only then deploy to the persistent `/usr` path.

---

## Self-Review

**Spec coverage check** (spec §→task):
- §6.1 HevcDepayloader (types, FU, AP, param-set, marker emit, malformed) → Tasks 1-5 ✓
- §4 marker-bit validated; §9 golden parity → Task 10 ✓; fuzz → Task 5 ✓
- §6.2 RtpVideoReceiver (recv loop, side-channels, single-thread, SO_RCVBUF, restream sendto, last-hop, gap→IDR, latency hook) → Task 7 ✓
- §6.3 GstFilePlayer (DVR, h265parse, transport controls) → Task 8 ✓
- §6.4 main.cpp wiring + playback routing → Task 9 ✓
- §7 H265-only deletions (reinit, mpp_type, decode-thread, detect_mp4_codec H264) + keep VideoCodec enum + preserve `gstreamer.received_bytes` → Tasks 6, 8, 9 ✓
- §8 CMake (new sources, drop gstnet/gio, test target) → Tasks 1, 7, 8 ✓
- §9 on-target A/B → Task 11 ✓
- §10 rollout → Task 11 ✓

**Known intermediate-build note:** Tasks 7-9 are a refactor sequence; the full `pixelpilot_tests` link is green only at Task 9 (it always includes `main.cpp`). Task 7 defers its CMake wiring to Task 8, so there is no duplicate-symbol window. Execute Tasks 7-8-9 as one reviewable unit if your gate requires a green build at every task boundary.

**Type consistency:** `NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>` is identical across `RtpVideoReceiver`, `GstFilePlayer`, and `main.cpp` ✓. `HevcDepayloader::FrameCallback = std::function<void(const uint8_t*, size_t)>` is internal to the depayloader; the receiver adapts it to `NEW_FRAME_CALLBACK` in its ctor ✓. The C-API (`idr_*`/`restream_*`) signatures are copied verbatim from `gstrtpreceiver.h` ✓.

**Capture/golden format:** `hevc_capture.bin` stores raw RTP packets (`[u32 len][packet]`, little-endian length); both the generator's gst feed and the Task 10 test parse the 12-byte RTP header themselves. Test and fixture hosts (aarch64 GS / x86 dev) are little-endian, matching the `struct.pack("<I")` writer.
