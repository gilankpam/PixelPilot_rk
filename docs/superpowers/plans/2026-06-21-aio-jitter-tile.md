# AIO JITTER Tile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the AIO Widget `LATENCY` tile with a `JITTER` tile showing RFC 3550 RTP interarrival jitter (ms).

**Architecture:** A new pure header `RtpJitterEstimator` computes RFC 3550 jitter from each frame's RTP timestamp vs GS arrival time. `latency_probe::on_rtp_buffer` feeds it per marker packet and publishes a new `video.rtp_jitter_ms` fact. The AIO Widget swaps its `SLOT_LATENCY` slot to subscribe to that fact and renders it through a new `aio::Metric::Jitter` colour band.

**Tech Stack:** C++17, Catch2 (v2 standalone runner for latency/jitter; v3 for aio_logic), CMake + Nix host-sim build.

## Global Constraints

- Clock-sync independence is mandatory: jitter math is a difference-of-differences so the GS↔drone clock offset cancels — never compare absolute timestamps across the two clocks.
- RTP video clock rate is **90 kHz** (H.264/H.265 convention); the constant `RTP_HZ = 90000.0`.
- `rtp_ts` is `uint32` and wraps; timestamp deltas must be computed as signed 32-bit differences.
- The AIO slot enum order is contractual with the default matcher list in the widget factory — any slot change must update both in lockstep.
- `video.latency.*` and `VideoStutterWidget` stay untouched; only the AIO subscription changes.
- Host tests run inside Nix. Latency/jitter unit + integration tests use the standalone g++ runner: `nix-shell --run tests/run_latency_tests.sh`. The `aio_logic_tests` target builds in `build-test/` (already configured with `USE_SIMULATOR=ON`): `nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target <t> -j4 && ./build-test/<t>"`.

---

### Task 1: `RtpJitterEstimator` pure logic unit

**Files:**
- Create: `src/rtp_jitter.hpp`
- Create: `tests/test_rtp_jitter.cpp`
- Modify: `tests/run_latency_tests.sh` (add the new test source to the g++ compile line)

**Interfaces:**
- Produces: `class RtpJitterEstimator` with
  - `double update(uint32_t ssrc, uint32_t rtp_ts, uint64_t gs_recv_us)` — feed one frame-marker arrival; returns current jitter in ms. Resets on SSRC change; first arrival after a reset returns `0.0`.
  - `double jitter_ms() const` — latest estimate.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_rtp_jitter.cpp`:

```cpp
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
```

Add `tests/test_rtp_jitter.cpp` to the g++ source list in `tests/run_latency_tests.sh` (insert after the `test_latency_probe_integration.cpp` line):

```bash
    "$HERE/test_latency_probe.cpp" \
    "$HERE/test_latency_probe_integration.cpp" \
    "$HERE/test_rtp_jitter.cpp" \
    "$HERE/osd_stub.cpp" \
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `nix-shell --run tests/run_latency_tests.sh 2>&1 | tail -20`
Expected: compile FAIL — `fatal error: rtp_jitter.hpp: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `src/rtp_jitter.hpp`:

```cpp
#ifndef RTP_JITTER_HPP
#define RTP_JITTER_HPP

#include <cstdint>

// RFC 3550 §6.4.1 interarrival jitter for an RTP video stream (90 kHz clock).
// Fed one frame-marker arrival at a time. Clock-sync independent: the estimate
// is built from a difference-of-differences, so the unknown sender/receiver
// clock offset cancels and no NTP-style sync is required.
class RtpJitterEstimator {
public:
    // Feed one frame-marker arrival.
    //   ssrc       : RTP SSRC (stream identity; a change resets state)
    //   rtp_ts     : sender RTP timestamp (90 kHz ticks, uint32, wraps)
    //   gs_recv_us : receiver arrival time (monotonic microseconds)
    // Returns the current smoothed jitter estimate in milliseconds. The first
    // arrival after construction or an SSRC change has no reference and
    // returns 0.0.
    double update(uint32_t ssrc, uint32_t rtp_ts, uint64_t gs_recv_us) {
        if (!have_prev_ || ssrc != ssrc_) {
            ssrc_         = ssrc;
            prev_rtp_ts_  = rtp_ts;
            prev_recv_us_ = gs_recv_us;
            j_us_         = 0.0;
            have_prev_    = true;
            return 0.0;
        }
        // Signed 32-bit tick delta handles uint32 wrap-around correctly.
        int32_t ts_delta_ticks = static_cast<int32_t>(rtp_ts - prev_rtp_ts_);
        double  s_delta_us = static_cast<double>(ts_delta_ticks) * (1e6 / RTP_HZ);
        double  r_delta_us = static_cast<double>(gs_recv_us - prev_recv_us_);
        double  d_us = r_delta_us - s_delta_us;
        if (d_us < 0) d_us = -d_us;
        j_us_ += (d_us - j_us_) / 16.0;

        prev_rtp_ts_  = rtp_ts;
        prev_recv_us_ = gs_recv_us;
        return jitter_ms();
    }

    double jitter_ms() const { return j_us_ / 1000.0; }

private:
    bool     have_prev_    = false;
    uint32_t ssrc_         = 0;
    uint32_t prev_rtp_ts_  = 0;
    uint64_t prev_recv_us_ = 0;
    double   j_us_         = 0.0;   // RFC 3550 J, in microseconds
    static constexpr double RTP_HZ = 90000.0;
};

#endif // RTP_JITTER_HPP
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `nix-shell --run tests/run_latency_tests.sh 2>&1 | tail -20`
Expected: PASS — all assertions in `[rtp_jitter]` pass; existing latency tests still pass.

- [ ] **Step 5: Commit**

```bash
git add src/rtp_jitter.hpp tests/test_rtp_jitter.cpp tests/run_latency_tests.sh
git commit -m "feat(jitter): add RtpJitterEstimator (RFC 3550 interarrival jitter)"
```

---

### Task 2: Publish `video.rtp_jitter_ms` from `latency_probe`

**Files:**
- Modify: `src/latency_probe.cpp` (include header; add `ProbeState` member; emit in `on_rtp_buffer`)
- Modify: `tests/test_latency_probe_integration.cpp` (add a jitter integration test reusing the loopback fixture)

**Interfaces:**
- Consumes: `RtpJitterEstimator` from Task 1; existing `ProbeState`, `g_pub_u_override`, `publish_uint_real`, `lp::start/stop`, `lp::on_rtp_buffer`, `lp::set_publish_overrides_for_test`, `FakeWaybeam` fixture.
- Produces: a new published fact **`video.rtp_jitter_ms`** (uint, ms), emitted once per frame-marker RTP packet while the probe is active.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_latency_probe_integration.cpp` (after the existing integration test, before any trailing namespace close — it reuses the file's `FakeWaybeam`, includes, and `lp` alias):

```cpp
TEST_CASE("latency_probe: publishes video.rtp_jitter_ms per marker",
          "[latency_probe][integration][rtp_jitter]") {
    FakeWaybeam fw;
    REQUIRE(fw.start_listening());
    fw.th = std::thread([&]{ fw.run(); });

    std::mutex captured_mu;
    std::vector<std::pair<std::string,uint64_t>> uint_facts;
    lp::set_publish_overrides_for_test(
        [&](const char* n, uint64_t v){
            std::lock_guard<std::mutex> lk(captured_mu); uint_facts.emplace_back(n,v);
        },
        [&](const char*, int64_t){});

    REQUIRE(lp::start("127.0.0.1", fw.port));

    // Helper: feed one marker packet with explicit rtp_ts + arrival time.
    auto feed = [](uint32_t rtp_ts, uint32_t ssrc, uint64_t recv_us){
        uint8_t hdr[12] = {};
        hdr[0] = 0x80;                       // V=2
        hdr[1] = 0x80;                       // marker=1
        hdr[4]=(rtp_ts>>24)&0xff; hdr[5]=(rtp_ts>>16)&0xff;
        hdr[6]=(rtp_ts>>8)&0xff;  hdr[7]= rtp_ts &0xff;
        hdr[8]=(ssrc>>24)&0xff; hdr[9]=(ssrc>>16)&0xff;
        hdr[10]=(ssrc>>8)&0xff; hdr[11]= ssrc &0xff;
        lp::on_rtp_buffer(hdr, sizeof(hdr), recv_us);
    };

    // 5 evenly spaced frames, then one 30 ms late.
    uint32_t ts = 1000; uint64_t rx = 1'000'000; const uint32_t ssrc = 7;
    for (int i = 0; i < 5; ++i) { feed(ts, ssrc, rx); ts += 1500; rx += 16667; }
    feed(ts, ssrc, rx + 30000);   // late arrival

    lp::stop();
    lp::set_publish_overrides_for_test(nullptr, nullptr);
    fw.stop = true;
    if (fw.th.joinable()) fw.th.join();
    close(fw.fd);

    std::lock_guard<std::mutex> lk(captured_mu);
    size_t jitter_pubs = 0;
    uint64_t last_jitter = 0;
    for (auto& [n, v] : uint_facts)
        if (n == "video.rtp_jitter_ms") { jitter_pubs++; last_jitter = v; }
    REQUIRE(jitter_pubs >= 5);          // one per marker (first returns 0)
    REQUIRE(last_jitter >= 1);          // late frame pushed the estimate up
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run tests/run_latency_tests.sh 2>&1 | tail -20`
Expected: FAIL — `jitter_pubs >= 5` fails (no `video.rtp_jitter_ms` ever published).

- [ ] **Step 3: Write the implementation**

In `src/latency_probe.cpp`:

(a) Add the include near the other project includes at the top of the file:

```cpp
#include "rtp_jitter.hpp"
```

(b) Add a `<cmath>` include if not already present (for `std::llround`).

(c) Add a member to `struct ProbeState` (around line 32–38, next to `FrameMatcher matcher;`):

```cpp
    RtpJitterEstimator jitter;
```

(d) In `on_rtp_buffer` (line 230), after the existing `on_marker_arrival` call, emit the jitter fact:

```cpp
    s->matcher.on_marker_arrival(h.ssrc, h.timestamp, gs_recv_us, gs_recv_us);

    double jms = s->jitter.update(h.ssrc, h.timestamp, gs_recv_us);
    PublishUintFn pu = g_pub_u_override ? g_pub_u_override : publish_uint_real;
    pu("video.rtp_jitter_ms", static_cast<uint64_t>(std::llround(jms)));
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix-shell --run tests/run_latency_tests.sh 2>&1 | tail -20`
Expected: PASS — `[rtp_jitter]` integration case passes; all existing latency tests still pass.

- [ ] **Step 5: Commit**

```bash
git add src/latency_probe.cpp tests/test_latency_probe_integration.cpp
git commit -m "feat(jitter): publish video.rtp_jitter_ms from latency_probe"
```

---

### Task 3: Add `aio::Metric::Jitter` colour band

**Files:**
- Modify: `src/osd_aio_logic.hpp:10` (enum)
- Modify: `src/osd_aio_logic.cpp` (`resolve_band`)
- Modify: `tests/test_aio_logic.cpp` (boundary cases)

**Interfaces:**
- Produces: `aio::Metric::Jitter` enum value and its `resolve_band` mapping — Good ≤ 10 ms, Warn ≤ 25 ms, Crit > 25 ms (lower is better).

- [ ] **Step 1: Write the failing test**

In `tests/test_aio_logic.cpp`, inside the `TEST_CASE("resolve_band boundaries", "[aio]")` block, append:

```cpp
    // JITTER ms: <=10 good, 11..25 warn, >25 crit (lower is better)
    REQUIRE(resolve_band(Metric::Jitter, 10) == Band::Good);
    REQUIRE(resolve_band(Metric::Jitter, 11) == Band::Warn);
    REQUIRE(resolve_band(Metric::Jitter, 25) == Band::Warn);
    REQUIRE(resolve_band(Metric::Jitter, 26) == Band::Crit);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target aio_logic_tests -j4 && ./build-test/aio_logic_tests '[aio]'" 2>&1 | tail -20`
Expected: compile FAIL — `Jitter` is not a member of `aio::Metric`.

- [ ] **Step 3: Write the implementation**

In `src/osd_aio_logic.hpp:10`, add `Jitter` to the enum:

```cpp
enum class Metric { Link, Bitrate, Latency, Rssi, Snr, Jitter };
```

In `src/osd_aio_logic.cpp`, add a case to `resolve_band` (after the `Metric::Snr` case, before the closing `}`):

```cpp
    case Metric::Jitter: // lower is better
        if (v <= 10) return Band::Good;
        if (v <= 25) return Band::Warn;
        return Band::Crit;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `nix-shell shell-sim.nix --run "cmake --build build-test --target aio_logic_tests -j4 && ./build-test/aio_logic_tests '[aio]'" 2>&1 | tail -20`
Expected: PASS — all `[aio]` assertions pass.

- [ ] **Step 5: Commit**

```bash
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(jitter): add aio::Metric::Jitter band (10/25 ms)"
```

---

### Task 4: Swap the AIO `LATENCY` tile to `JITTER`

**Files:**
- Modify: `src/osd.cpp` (slot enum, `setFact` comment, `draw_strip` tile, default matcher list)

**Interfaces:**
- Consumes: `video.rtp_jitter_ms` fact (Task 2), `aio::Metric::Jitter` (Task 3).
- Produces: AIO strip renders a `JITTER … ms` tile in the slot formerly occupied by `LATENCY`.

There is no unit test for the cairo `draw_strip` output; the gate is a clean host-sim compile plus the logic tests from Tasks 1–3. All four edits below must land together (the enum order is contractual with the matcher list).

- [ ] **Step 1: Rename the slot enum**

In `src/osd.cpp:1413`, change:

```cpp
        SLOT_LATENCY,       // video.latency.total_ms  (uint)
```
to:
```cpp
        SLOT_JITTER,        // video.rtp_jitter_ms     (uint)
```

- [ ] **Step 2: Update the `setFact` default-case comment**

In `src/osd.cpp:1472`, change the comment to reflect the rename:

```cpp
        default: // SLOT_VIDEO_RES, SLOT_VIDEO_FPS, SLOT_JITTER
            args[idx] = fact;
            break;
```

(No behavioural change — the jitter value is already smoothed, so storing the latest value is correct.)

- [ ] **Step 3: Replace the LATENCY tile in `draw_strip`**

In `src/osd.cpp`, change the latency read (line 1673):

```cpp
        long lat = (long)arg_u(SLOT_LATENCY);
```
to:
```cpp
        long jit = (long)arg_u(SLOT_JITTER);
```

and the tile push (lines 1692–1693):

```cpp
        right.push_back(metric_tile("LATENCY", std::to_string(lat), "ms",
                                    aio::resolve_band(aio::Metric::Latency, (double)lat), "888"));
```
to:
```cpp
        right.push_back(metric_tile("JITTER", std::to_string(jit), "ms",
                                    aio::resolve_band(aio::Metric::Jitter, (double)jit), "888"));
```

(Like the prior LATENCY tile, this renders the number unconditionally — `0` when the probe is inactive and no fact has arrived. This matches the replaced tile's behaviour.)

- [ ] **Step 4: Swap the default matcher in the widget factory**

In `src/osd.cpp:2358`, change:

```cpp
					matchers.push_back(FactMatcher("video.latency.total_ms"));        // SLOT_LATENCY
```
to:
```cpp
					matchers.push_back(FactMatcher("video.rtp_jitter_ms"));           // SLOT_JITTER
```

- [ ] **Step 5: Verify it compiles (host-sim gate)**

Run: `nix-shell shell-sim.nix --run "cmake -S . -B build-sim -DUSE_SIMULATOR=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && cmake --build build-sim --target pixelpilot -j4" 2>&1 | tail -15`
Expected: build succeeds, no references to `SLOT_LATENCY` / `Metric::Latency` left in the AIO path.

- [ ] **Step 6: Run the full host test suites for regressions**

Run:
```bash
nix-shell --run tests/run_latency_tests.sh 2>&1 | tail -5
nix-shell shell-sim.nix --run "cmake --build build-test --target aio_logic_tests -j4 && ./build-test/aio_logic_tests" 2>&1 | tail -5
```
Expected: both PASS.

- [ ] **Step 7: Commit**

```bash
git add src/osd.cpp
git commit -m "feat(jitter): replace AIO LATENCY tile with RTP JITTER"
```

---

## Self-Review notes

- **Spec coverage:** Task 1 = `RtpJitterEstimator` + RFC 3550 math + wrap/SSRC handling; Task 2 = `on_rtp_buffer` wiring + `video.rtp_jitter_ms` fact; Task 3 = `aio::Metric::Jitter` band (10/25 ms); Task 4 = slot rename, matcher swap, draw, `setFact`. Non-goals (latency untouched, VideoStutterWidget untouched) are preserved — no task modifies them.
- **Probe dependency** (spec note): jitter publishes only while `latency_probe` is active. Out of scope to decouple; the tile shows `0` when inactive (matches prior LATENCY behaviour).
- **Type consistency:** `update(uint32_t, uint32_t, uint64_t) -> double` and `jitter_ms() -> double` are used identically in Tasks 1 and 2; `Metric::Jitter` is defined in Task 3 and consumed in Task 4; `video.rtp_jitter_ms` string is identical across Tasks 2 and 4.
