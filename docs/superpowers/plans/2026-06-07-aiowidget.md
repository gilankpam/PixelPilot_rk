# AIOWidget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a single Cairo OSD widget (`AIOWidget`) that renders the full `design_handoff_gs_osd` racing-HUD overlay — bottom telemetry strip + top-right REC badge — in two color schemes (`white` mono / `accent` threshold palette), configured by `{ "name", "type", "color_scheme" }` alone.

**Architecture:** Pure, dependency-free logic (threshold bands, color resolution, link-quality %, signal-bar count, MHz→channel, timecode, per-antenna best-value aggregation) lives in a new cairo-free translation unit `src/osd_aio_logic.{hpp,cpp}` and is unit-tested fast on the host. The Cairo drawing + telemetry plumbing (`AIOWidget : public Widget`) lives in `src/osd.cpp` and calls into that logic; it compiles only in the GS cross-build and is verified by compile + on-device visual check. Telemetry uses the existing tagless-matcher → `setFact` mechanism; RSSI/SNR/freq are auto-discovered across all antennas (no JSON tags needed).

**Tech Stack:** C++17, Cairo (toy font API + fontconfig), nlohmann/json, Catch2, CMake. Spec: `docs/superpowers/specs/2026-06-07-aiowidget-design.md`.

---

## Deviations from the spec (read first)

Two spec assumptions don't match build reality. Both are deliberate, low-risk adjustments; flag to the user if they object.

1. **Testing location.** The spec said tests run under the `USE_SIMULATOR` host build. In fact `osd.cpp` is **not** compiled in the host sim build and the only target that builds it (`pixelpilot_tests`, `BUILD_TESTS=ON`) needs the aarch64 cross-toolchain (rockchip_mpp/drm/gst/EGL) and the host shell has neither cairo nor freetype. So: **pure logic is extracted to `src/osd_aio_logic.cpp` and tested via a new host Catch2 target `aio_logic_tests`** (fast, std-only). The Cairo widget code is verified by cross-build compile + on-device visual check (no host draw smoke test).

2. **Fonts.** The spec proposed loading Barlow Condensed via Cairo's FreeType backend. FreeType linkage in the cross-toolchain is unverified and adds build risk. Instead we use the **already-proven toy font API** (`cairo_select_font_face`, as the current OSD does with "Roboto") with Barlow Condensed **installed into the GS system font path** so fontconfig resolves it. Zero build-system changes. The FreeType route remains a documented fallback if per-weight fidelity proves insufficient.

---

## File structure

| File | Responsibility |
|---|---|
| `src/osd_aio_logic.hpp` (create) | Declarations of the pure logic in `namespace aio`: `Band`, `Metric`, `Scheme`, `Rgba`, `resolve_band`, `resolve_color`, `link_quality_pct`, `signal_bar_count`, `freq_to_channel`, `format_timecode`, `AntennaAggregator`. No cairo, no project types. |
| `src/osd_aio_logic.cpp` (create) | Definitions of the above. Depends only on the C++ stdlib. |
| `tests/test_aio_logic.cpp` (create) | Catch2 unit tests for every logic function, tagged `[aio]`. |
| `CMakeLists.txt` (modify) | Add `src/osd_aio_logic.cpp`/`.hpp` to `LIB_SOURCE_FILES`; add host `aio_logic_tests` target in the `USE_SIMULATOR`/Catch2 block. |
| `src/osd.cpp` (modify) | `#include "osd_aio_logic.hpp"`; add `AIOWidget` class (state, `setFact`, `draw`); add `type == "AIOWidget"` factory branch with x/y/facts defaulting + default tagless matcher injection. |
| `osd-aio.json` (create) | Ready-to-use example OSD config using AIOWidget (documentation/migration aid). |

## Conventions used by every task

- **Host logic build/test** (this dev box):
  - Configure once if `build-test/` missing: `nix-shell --run 'cmake -DUSE_SIMULATOR=ON -S . -B build-test'`
  - Build: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
  - Run all aio tests: `nix-shell --run './build-test/aio_logic_tests "[aio]"'`
- **Cross-build compile check** (for `src/osd.cpp` changes — the only way to compile the cairo code):
  ```sh
  printf '%s\n' \
    'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
    'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
    'export DEFCONFIG=radxa_zero3_defconfig' \
    './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' \
    | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix
  ```
  Built binary: `output/radxa_zero3_defconfig/build/pixelpilot-custom/pixelpilot` (under the sbc repo).
- **Deploy + run on GS** (`root@10.18.0.1`, flaky link — wrap ssh/scp in retries with `-o ConnectTimeout=10 -o ServerAliveInterval=3`): scp the new binary over `/usr/bin/pixelpilot`, then `kill <pixelpilot pid>` (the fpvd supervisor respawns it in ~1–2 s). Verify with `md5sum /proc/<pid>/exe`. Full recipe in `docs/superpowers/notes/install_persistent.sh` and the `gs-build-deploy-workflow-pixelpilot` memory.

---

## Task 1: Scaffold the pure-logic unit + host test target

**Files:**
- Create: `src/osd_aio_logic.hpp`
- Create: `src/osd_aio_logic.cpp`
- Create: `tests/test_aio_logic.cpp`
- Modify: `CMakeLists.txt` (LIB_SOURCE_FILES ~line 156; USE_SIMULATOR Catch2 block ~line 267)

- [ ] **Step 1: Create the header skeleton**

`src/osd_aio_logic.hpp`:
```cpp
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace aio {

enum class Band { Good, Warn, Crit, Neutral };
enum class Metric { Link, Bitrate, Latency, Rssi, Snr };
enum class Scheme { Accent, White };

struct Rgba {
    double r, g, b, a; // each 0..1
    bool operator==(const Rgba& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
};

// Returns the version string so we can prove the unit links. Replaced by real
// functions in later tasks.
const char* aio_logic_version();

} // namespace aio
```

- [ ] **Step 2: Create the source skeleton**

`src/osd_aio_logic.cpp`:
```cpp
#include "osd_aio_logic.hpp"

namespace aio {

const char* aio_logic_version() { return "aio-logic-1"; }

} // namespace aio
```

- [ ] **Step 3: Create the test file with one trivial test**

`tests/test_aio_logic.cpp`:
```cpp
#include <catch2/catch.hpp>
#include "../src/osd_aio_logic.hpp"

TEST_CASE("logic unit links", "[aio]") {
    REQUIRE(std::string(aio::aio_logic_version()) == "aio-logic-1");
}
```

- [ ] **Step 4: Wire the logic source into LIB_SOURCE_FILES**

In `CMakeLists.txt`, find (~line 156):
```cmake
        src/osd.hpp
        src/osd.cpp
```
Change to:
```cmake
        src/osd.hpp
        src/osd.cpp
        src/osd_aio_logic.hpp
        src/osd_aio_logic.cpp
```

- [ ] **Step 5: Add the host test target**

In `CMakeLists.txt`, inside the `if(Catch2_FOUND)` block in the `USE_SIMULATOR` branch, immediately before the `endif()` at ~line 269 (right after the `gs_enum_tests` `target_link_libraries`), add:
```cmake
    add_executable(aio_logic_tests
      src/osd_aio_logic.cpp
      tests/test_aio_logic.cpp)
    target_include_directories(aio_logic_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(aio_logic_tests Catch2::Catch2WithMain)
```

- [ ] **Step 6: Configure (if needed) and build the test target**

Run:
```sh
nix-shell --run 'cmake -DUSE_SIMULATOR=ON -S . -B build-test' 2>&1 | tail -5
nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'
```
Expected: configures without error; `aio_logic_tests` links successfully.

- [ ] **Step 7: Run the test**

Run: `nix-shell --run './build-test/aio_logic_tests "[aio]"'`
Expected: `All tests passed (1 assertion in 1 test case)`.

- [ ] **Step 8: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp CMakeLists.txt
git commit -m "feat(osd): scaffold AIOWidget pure-logic unit + host test target"
```

---

## Task 2: `resolve_band` — threshold bands

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("resolve_band boundaries", "[aio]") {
    using aio::Band; using aio::Metric; using aio::resolve_band;

    // LINK %: >=70 good, 40..69 warn, <40 crit
    REQUIRE(resolve_band(Metric::Link, 70) == Band::Good);
    REQUIRE(resolve_band(Metric::Link, 69) == Band::Warn);
    REQUIRE(resolve_band(Metric::Link, 40) == Band::Warn);
    REQUIRE(resolve_band(Metric::Link, 39) == Band::Crit);

    // BITRATE: >=15 good, 8..14.9 warn, <8 crit
    REQUIRE(resolve_band(Metric::Bitrate, 15) == Band::Good);
    REQUIRE(resolve_band(Metric::Bitrate, 14.9) == Band::Warn);
    REQUIRE(resolve_band(Metric::Bitrate, 8) == Band::Warn);
    REQUIRE(resolve_band(Metric::Bitrate, 7.9) == Band::Crit);

    // LATENCY: <=50 good, 51..100 warn, >100 crit (lower is better)
    REQUIRE(resolve_band(Metric::Latency, 50) == Band::Good);
    REQUIRE(resolve_band(Metric::Latency, 51) == Band::Warn);
    REQUIRE(resolve_band(Metric::Latency, 100) == Band::Warn);
    REQUIRE(resolve_band(Metric::Latency, 101) == Band::Crit);

    // RSSI: >=-70 good, -71..-80 warn, <-80 crit (higher is better)
    REQUIRE(resolve_band(Metric::Rssi, -70) == Band::Good);
    REQUIRE(resolve_band(Metric::Rssi, -71) == Band::Warn);
    REQUIRE(resolve_band(Metric::Rssi, -80) == Band::Warn);
    REQUIRE(resolve_band(Metric::Rssi, -81) == Band::Crit);

    // SNR: >=12 good, 6..11 warn, <6 crit
    REQUIRE(resolve_band(Metric::Snr, 12) == Band::Good);
    REQUIRE(resolve_band(Metric::Snr, 11) == Band::Warn);
    REQUIRE(resolve_band(Metric::Snr, 6) == Band::Warn);
    REQUIRE(resolve_band(Metric::Snr, 5) == Band::Crit);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — compile error, `resolve_band` not declared.

- [ ] **Step 3: Declare the function**

In `src/osd_aio_logic.hpp`, add before the closing `} // namespace aio`:
```cpp
// Threshold band for a metric value (units as displayed: Mb/s, ms, dBm, dB, %).
Band resolve_band(Metric m, double value);
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, add before the closing `} // namespace aio`:
```cpp
Band resolve_band(Metric m, double v) {
    switch (m) {
    case Metric::Link:
        if (v >= 70) return Band::Good;
        if (v >= 40) return Band::Warn;
        return Band::Crit;
    case Metric::Bitrate:
        if (v >= 15) return Band::Good;
        if (v >= 8)  return Band::Warn;
        return Band::Crit;
    case Metric::Latency: // lower is better
        if (v <= 50)  return Band::Good;
        if (v <= 100) return Band::Warn;
        return Band::Crit;
    case Metric::Rssi: // higher (less negative) is better
        if (v >= -70) return Band::Good;
        if (v >= -80) return Band::Warn;
        return Band::Crit;
    case Metric::Snr:
        if (v >= 12) return Band::Good;
        if (v >= 6)  return Band::Warn;
        return Band::Crit;
    }
    return Band::Neutral;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS, all assertions.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget resolve_band threshold tables"
```

---

## Task 3: `resolve_color` — band + scheme → RGBA

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("resolve_color schemes", "[aio]") {
    using aio::Band; using aio::Scheme; using aio::Rgba; using aio::resolve_color;
    const Rgba white{1, 1, 1, 1};
    const Rgba green{0x1f / 255.0, 0xe0 / 255.0, 0x84 / 255.0, 1};
    const Rgba amber{0xff / 255.0, 0xb3 / 255.0, 0x00 / 255.0, 1};
    const Rgba red  {0xff / 255.0, 0x2e / 255.0, 0x3e / 255.0, 1};

    // White scheme: everything white regardless of band.
    REQUIRE(resolve_color(Band::Good, Scheme::White, false) == white);
    REQUIRE(resolve_color(Band::Crit, Scheme::White, false) == white);
    REQUIRE(resolve_color(Band::Neutral, Scheme::White, true) == white);

    // Accent scheme: threshold palette for metrics, white for neutral tiles.
    REQUIRE(resolve_color(Band::Good, Scheme::Accent, false) == green);
    REQUIRE(resolve_color(Band::Warn, Scheme::Accent, false) == amber);
    REQUIRE(resolve_color(Band::Crit, Scheme::Accent, false) == red);
    REQUIRE(resolve_color(Band::Neutral, Scheme::Accent, true) == white);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `resolve_color` not declared.

- [ ] **Step 3: Declare the function**

In `src/osd_aio_logic.hpp`, add before the closing namespace brace:
```cpp
// Final value/threshold color. is_neutral marks informational tiles (VIDEO, WIFI CH)
// which are always white. White scheme returns white for every band.
Rgba resolve_color(Band band, Scheme scheme, bool is_neutral);
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, add before the closing namespace brace:
```cpp
Rgba resolve_color(Band band, Scheme scheme, bool is_neutral) {
    const Rgba white{1, 1, 1, 1};
    if (scheme == Scheme::White || is_neutral || band == Band::Neutral) return white;
    switch (band) {
    case Band::Good: return Rgba{0x1f / 255.0, 0xe0 / 255.0, 0x84 / 255.0, 1};
    case Band::Warn: return Rgba{0xff / 255.0, 0xb3 / 255.0, 0x00 / 255.0, 1};
    case Band::Crit: return Rgba{0xff / 255.0, 0x2e / 255.0, 0x3e / 255.0, 1};
    default:         return white;
    }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget resolve_color for white/accent schemes"
```

---

## Task 4: `link_quality_pct` + `signal_bar_count`

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("link_quality_pct", "[aio]") {
    using aio::link_quality_pct;
    REQUIRE(link_quality_pct(0, 0) == 0);       // no traffic -> 0%
    REQUIRE(link_quality_pct(100, 0) == 100);   // perfect
    REQUIRE(link_quality_pct(100, 100) == 0);   // all lost
    REQUIRE(link_quality_pct(100, 8) == 92);    // nominal sample
    REQUIRE(link_quality_pct(100, 200) == 0);   // clamp: lost > all
}

TEST_CASE("signal_bar_count", "[aio]") {
    using aio::signal_bar_count;
    REQUIRE(signal_bar_count(0) == 0);
    REQUIRE(signal_bar_count(100) == 5);
    REQUIRE(signal_bar_count(92) == 5);   // round(4.6)
    REQUIRE(signal_bar_count(41) == 2);   // round(2.05)
    REQUIRE(signal_bar_count(50) == 3);   // round(2.5)
    REQUIRE(signal_bar_count(-10) == 0);  // clamp low
    REQUIRE(signal_bar_count(150) == 5);  // clamp high
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — functions not declared.

- [ ] **Step 3: Declare the functions**

In `src/osd_aio_logic.hpp`, add before the closing namespace brace:
```cpp
// Link quality 0..100 from packet counters over a window.
int link_quality_pct(long pkt_all, long pkt_lost);

// Filled signal-bar count 0..5 from link quality %.
int signal_bar_count(int lq_pct);
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, ensure `#include <cmath>` is present at the top (add it under the existing include if missing), then add before the closing namespace brace:
```cpp
int link_quality_pct(long all, long lost) {
    if (all <= 0) return 0;
    long good = all - lost;
    if (good < 0) good = 0;
    long pct = std::lround(100.0 * static_cast<double>(good) / static_cast<double>(all));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return static_cast<int>(pct);
}

int signal_bar_count(int lq_pct) {
    int n = static_cast<int>(std::lround(lq_pct / 100.0 * 5.0));
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    return n;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget link_quality_pct + signal_bar_count"
```

---

## Task 5: `freq_to_channel`

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("freq_to_channel", "[aio]") {
    using aio::freq_to_channel;
    REQUIRE(freq_to_channel(5745) == std::optional<int>(149));
    REQUIRE(freq_to_channel(5180) == std::optional<int>(36));
    REQUIRE(freq_to_channel(2412) == std::optional<int>(1));
    REQUIRE(freq_to_channel(2472) == std::optional<int>(13));
    REQUIRE(freq_to_channel(2484) == std::optional<int>(14));
    REQUIRE(freq_to_channel(3000) == std::nullopt); // out of band -> caller shows raw MHz
    REQUIRE(freq_to_channel(5183) == std::nullopt); // not on a 5 MHz grid
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `freq_to_channel` not declared.

- [ ] **Step 3: Declare the function**

In `src/osd_aio_logic.hpp`, add before the closing namespace brace:
```cpp
// MHz -> WiFi channel number; nullopt if outside known 2.4/5 GHz grids.
std::optional<int> freq_to_channel(int freq_mhz);
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, add before the closing namespace brace:
```cpp
std::optional<int> freq_to_channel(int f) {
    if (f == 2484) return 14;                                  // 2.4 GHz ch 14
    if (f >= 2412 && f <= 2472 && (f - 2407) % 5 == 0)
        return (f - 2407) / 5;                                 // 2.4 GHz ch 1..13
    if (f >= 5150 && f <= 5895 && (f - 5000) % 5 == 0)
        return (f - 5000) / 5;                                 // 5 GHz
    return std::nullopt;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget freq_to_channel conversion"
```

---

## Task 6: `format_timecode`

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("format_timecode", "[aio]") {
    using aio::format_timecode;
    REQUIRE(format_timecode(0) == "00:00:00");
    REQUIRE(format_timecode(8) == "00:00:08");
    REQUIRE(format_timecode(872) == "00:14:32");   // handoff REC sample
    REQUIRE(format_timecode(3661) == "01:01:01");
    REQUIRE(format_timecode(-5) == "00:00:00");     // clamp negatives
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `format_timecode` not declared.

- [ ] **Step 3: Declare the function**

In `src/osd_aio_logic.hpp`, add before the closing namespace brace:
```cpp
// Elapsed seconds -> "HH:MM:SS" (zero-padded; negatives clamp to zero).
std::string format_timecode(long elapsed_s);
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, ensure `#include <cstdio>` is present at the top, then add before the closing namespace brace:
```cpp
std::string format_timecode(long s) {
    if (s < 0) s = 0;
    long h = s / 3600;
    long m = (s % 3600) / 60;
    long sec = s % 60;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", h, m, sec);
    return std::string(buf);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget format_timecode"
```

---

## Task 7: `AntennaAggregator` — per-antenna best value with staleness

**Files:**
- Modify: `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("AntennaAggregator best + staleness", "[aio]") {
    aio::AntennaAggregator agg(2500); // 2500 ms stale window

    REQUIRE(agg.best(0) == std::nullopt);          // empty
    agg.update("0", -70, 1000);
    agg.update("1", -62, 1000);
    agg.update("256", -85, 1000);
    REQUIRE(agg.best(1000) == std::optional<long>(-62)); // max across antennas
    REQUIRE(agg.live_count(1000) == 3u);

    // Antenna "1" goes stale (no refresh); others refreshed at 4000ms.
    agg.update("0", -71, 4000);
    agg.update("256", -88, 4000);
    REQUIRE(agg.best(4000) == std::optional<long>(-71)); // "1" (-62) evicted as stale
    REQUIRE(agg.live_count(4000) == 2u);

    // Everything stale -> nullopt.
    REQUIRE(agg.best(10000) == std::nullopt);
    REQUIRE(agg.live_count(10000) == 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `AntennaAggregator` not declared.

- [ ] **Step 3: Declare the class**

In `src/osd_aio_logic.hpp`, add before the closing namespace brace:
```cpp
// Tracks the latest value per antenna id and reports the best (max) across
// antennas seen within `stale_ms`. Pure: the caller supplies the clock (now_ms).
class AntennaAggregator {
public:
    explicit AntennaAggregator(long stale_ms = 2500) : stale_ms_(stale_ms) {}
    void update(const std::string& ant_id, long value, long now_ms);
    std::optional<long> best(long now_ms) const; // max over live antennas
    std::size_t live_count(long now_ms) const;
private:
    struct Entry { long value; long last_ms; };
    std::map<std::string, Entry> entries_;
    long stale_ms_;
};
```

- [ ] **Step 4: Implement**

In `src/osd_aio_logic.cpp`, add before the closing namespace brace:
```cpp
void AntennaAggregator::update(const std::string& ant_id, long value, long now_ms) {
    entries_[ant_id] = Entry{value, now_ms};
}

std::optional<long> AntennaAggregator::best(long now_ms) const {
    std::optional<long> result;
    for (const auto& [id, e] : entries_) {
        if (now_ms - e.last_ms > stale_ms_) continue; // stale
        if (!result || e.value > *result) result = e.value;
    }
    return result;
}

std::size_t AntennaAggregator::live_count(long now_ms) const {
    std::size_t n = 0;
    for (const auto& [id, e] : entries_)
        if (now_ms - e.last_ms <= stale_ms_) ++n;
    return n;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS — the full `[aio]` suite (7 test cases) green.

- [ ] **Step 6: Commit**

```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget AntennaAggregator with staleness eviction"
```

---

## Task 8: `AIOWidget` class — state, clock, and `setFact` telemetry plumbing

This task adds the widget to `osd.cpp` with full telemetry handling but a stub `draw()`. It compiles in the cross-build only. No host unit test (cairo/target deps); verification is the cross-build compile.

**Files:**
- Modify: `src/osd.cpp` (add include near other includes; add class after `VideoStutterWidget`, before `GPSWidget` ~line 1398)

- [ ] **Step 1: Include the logic header**

In `src/osd.cpp`, near the top with the other project includes (after `#include <cairo.h>` at line 42), add:
```cpp
#include "osd_aio_logic.hpp"
```

- [ ] **Step 2: Add the AIOWidget class (state + clock + setFact, stub draw)**

In `src/osd.cpp`, insert before `class GPSWidget` (~line 1398):
```cpp
class AIOWidget : public Widget {
public:
    // Fixed slot order. The factory registers one tagless matcher per slot in
    // exactly this order, so setFact's idx maps directly to a Slot.
    enum Slot {
        SLOT_VIDEO_H = 0,  // video.height            (uint)
        SLOT_VIDEO_FPS,    // video.displayed_frame   (uint, -> per-second)
        SLOT_FREQ,         // wfbcli.rx.ant_stats.freq(uint, per-antenna)
        SLOT_PKT_ALL,      // wfbcli.rx.packets.all.delta   (uint)
        SLOT_PKT_LOST,     // wfbcli.rx.packets.lost.delta  (uint)
        SLOT_PKT_FEC,      // wfbcli.rx.packets.fec_rec.delta (uint, reserved)
        SLOT_BITRATE,      // gstreamer.received_bytes(uint, -> Mb/s)
        SLOT_LATENCY,      // video.latency.total_ms  (uint)
        SLOT_RSSI,         // wfbcli.rx.ant_stats.rssi_avg (int, per-antenna)
        SLOT_SNR,          // wfbcli.rx.ant_stats.snr_avg  (int, per-antenna)
        SLOT_REC,          // dvr.recording           (bool)
        SLOT_COUNT
    };

    AIOWidget(int pos_x, int pos_y, aio::Scheme scheme)
        : Widget(pos_x, pos_y, SLOT_COUNT), scheme(scheme),
          fps(2000, 200), bps(2000, 100),
          pkt_all(2000, 200), pkt_lost(2000, 200) {}

    void setFact(uint idx, Fact fact) override {
        long now = now_ms();
        switch (idx) {
        case SLOT_VIDEO_FPS:
            fps.add(static_cast<long>(fact.getUintValue()));
            args[idx] = Fact(FactMeta("aio.fps"),
                             (ulong)fps.rate_per_second_over_last_ms(1000));
            break;
        case SLOT_BITRATE:
            bps.add(static_cast<long>(fact.getUintValue()));
            // 125000 = 1e6 / 8  -> megabits
            args[idx] = Fact(FactMeta("aio.mbps"),
                             bps.rate_per_second_over_last_ms(1000) / 125000.0);
            break;
        case SLOT_PKT_ALL:
            pkt_all.add(static_cast<long>(fact.getUintValue()));
            args[idx] = fact;
            break;
        case SLOT_PKT_LOST:
            pkt_lost.add(static_cast<long>(fact.getUintValue()));
            args[idx] = fact;
            break;
        case SLOT_RSSI:
            if (accept_link(fact)) rssi_agg.update(ant_id_of(fact), (long)fact, now);
            args[idx] = fact;
            break;
        case SLOT_SNR:
            if (accept_link(fact)) snr_agg.update(ant_id_of(fact), (long)fact, now);
            args[idx] = fact;
            break;
        case SLOT_FREQ:
            if (accept_link(fact)) last_freq = static_cast<long>(fact.getUintValue());
            args[idx] = fact;
            break;
        case SLOT_REC: {
            bool rec = fact.isDefined() && fact.getBoolValue();
            if (rec && !recording) rec_start_ms = now; // false->true transition
            recording = rec;
            args[idx] = fact;
            break;
        }
        default: // SLOT_VIDEO_H, SLOT_LATENCY, SLOT_PKT_FEC
            args[idx] = fact;
            break;
        }
    }

    void draw(cairo_t *cr) override { /* implemented in Task 9 */ }

protected:
    static long now_ms() {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
    }
    // Aggregate only the video link: accept facts whose "id" tag contains
    // "video", or that carry no "id" tag at all.
    static bool accept_link(const Fact& f) {
        auto tags = f.getTags();
        auto it = tags.find("id");
        if (it == tags.end()) return true;
        return it->second.find("video") != std::string::npos;
    }
    static std::string ant_id_of(const Fact& f) {
        auto tags = f.getTags();
        auto it = tags.find("ant_id");
        return it != tags.end() ? it->second : std::string("0");
    }

    aio::Scheme scheme;
    RunningAverage fps, bps, pkt_all, pkt_lost;
    aio::AntennaAggregator rssi_agg{2500}, snr_agg{2500};
    long last_freq = -1;
    bool recording = false;
    long rec_start_ms = 0;
    long blink_last_ms = 0;
    bool blink_on = true;
};
```

- [ ] **Step 3: Cross-build compile check**

Run the cross-build command from "Conventions" (the `printf ... | nix-shell` block).
Expected: `PPBUILD_DONE rc=0` (compiles clean; `AIOWidget` is defined but not yet referenced by the factory, so unused-class is fine).

- [ ] **Step 4: Commit**

```sh
git add src/osd.cpp
git commit -m "feat(osd): AIOWidget class with telemetry plumbing (stub draw)"
```

---

## Task 9: `AIOWidget::draw` — full overlay rendering

Replaces the stub `draw()` with the complete Cairo rendering: gradient rail, two metric-tile groups, signal bars, and the REC badge, all scaled by `surface_height / 1080` and anchored to screen edges. Uses the Task 2–7 logic.

**Files:**
- Modify: `src/osd.cpp` (replace `AIOWidget::draw` body; add private draw helpers)

- [ ] **Step 1: Replace the stub draw and add helpers**

In `src/osd.cpp`, replace `void draw(cairo_t *cr) override { /* implemented in Task 9 */ }` with:
```cpp
    void draw(cairo_t *cr) override {
        cairo_surface_t *target = cairo_get_target(cr);
        const double W = cairo_image_surface_get_width(target);
        const double H = cairo_image_surface_get_height(target);
        const double s = H / 1080.0; // uniform scale; 16:9 stays undistorted

        cairo_save(cr);
        draw_gradient_rail(cr, W, H, s);
        draw_strip(cr, W, H, s);
        draw_rec_badge(cr, W, H, s);
        cairo_restore(cr);
    }

private:
    // ---- pixel snapping helpers -------------------------------------------
    static double px(double v, double min_px) { // scaled, integer-snapped, floored
        double r = std::round(v);
        return r < min_px ? min_px : r;
    }
    static void set_rgba(cairo_t *cr, aio::Rgba c) {
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    }
    // ---- fonts (toy API + fontconfig-resolved Barlow Condensed) -----------
    static void font_value(cairo_t *cr, double size) { // 800 italic
        cairo_select_font_face(cr, "Barlow Condensed",
                               CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, size);
    }
    static void font_label(cairo_t *cr, double size) { // 600
        cairo_select_font_face(cr, "Barlow Condensed",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, size);
    }
    static void font_unit(cairo_t *cr, double size) {  // 700
        cairo_select_font_face(cr, "Barlow Condensed",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, size);
    }
    static void font_rec(cairo_t *cr, double size) {   // 800 italic
        cairo_select_font_face(cr, "Barlow Condensed",
                               CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, size);
    }
    // Text with a 1px dark shadow behind it (legibility over video).
    void text_shadow(cairo_t *cr, double x, double y, const std::string &t,
                     aio::Rgba col, double s) {
        double off = px(1 * s, 1);
        cairo_move_to(cr, x + off, y + off);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_show_text(cr, t.c_str());
        cairo_move_to(cr, x, y);
        set_rgba(cr, col);
        cairo_show_text(cr, t.c_str());
    }

    // A resolved tile ready to render.
    struct Tile {
        std::string label;
        std::string value;
        std::string unit;   // may be empty
        aio::Rgba value_col;
        aio::Rgba rail_col;
    };

    aio::Rgba neutral_rail() const { return aio::Rgba{1, 1, 1, 0.5}; }
    aio::Rgba label_col() const    { return aio::Rgba{1, 1, 1, 0.62}; }
    aio::Rgba unit_col() const     { return aio::Rgba{1, 1, 1, 0.66}; }

    // Build a metric tile (threshold-colored unless neutral).
    Tile metric_tile(const std::string &label, const std::string &value,
                     const std::string &unit, aio::Metric m, double v) {
        aio::Band band = aio::resolve_band(m, v);
        aio::Rgba col = aio::resolve_color(band, scheme, false);
        aio::Rgba rail = (scheme == aio::Scheme::Accent)
                             ? col : aio::Rgba{1, 1, 1, 1};
        return Tile{label, value, unit, col, rail};
    }
    Tile neutral_tile(const std::string &label, const std::string &value,
                      const std::string &unit) {
        return Tile{label, value, unit,
                    aio::Rgba{1, 1, 1, 1}, neutral_rail()};
    }

    // Returns the tile's drawn width so the caller can advance x.
    double draw_tile(cairo_t *cr, double x, double baseline, const Tile &t, double s) {
        const double pad   = px(26 * s, 2);
        const double rail_w = px(30 * s, 4);
        const double rail_h = px(4 * s, 2);
        const double gap    = px(2 * s, 1);
        const double label_sz = 14 * s;
        const double value_sz = 46 * s;
        const double unit_sz  = 16 * s;

        double cx = x + pad;
        // Accent rail (top of the tile). Place it above the label.
        double rail_y = baseline - value_sz - label_sz - gap * 2 - rail_h;
        set_rgba(cr, t.rail_col);
        rounded_rect(cr, cx, rail_y, rail_w, rail_h, px(2 * s, 1));
        cairo_fill(cr);

        // Label.
        font_label(cr, label_sz);
        double label_y = rail_y + rail_h + gap + label_sz;
        text_shadow(cr, cx, label_y, t.label, label_col(), s);

        // Value.
        font_value(cr, value_sz);
        double value_y = baseline;
        text_shadow(cr, cx, value_y, t.value, t.value_col, s);
        cairo_text_extents_t ve;
        cairo_text_extents(cr, t.value.c_str(), &ve);
        double after_value = cx + ve.x_advance + px(6 * s, 1);

        // Unit (optional), baseline-aligned with the value.
        double tile_right = cx + ve.x_advance;
        if (!t.unit.empty()) {
            font_unit(cr, unit_sz);
            text_shadow(cr, after_value, value_y, t.unit, unit_col(), s);
            cairo_text_extents_t ue;
            cairo_text_extents(cr, t.unit.c_str(), &ue);
            tile_right = after_value + ue.x_advance;
        }
        // Tile width = max(rail extent, value/unit extent) + trailing pad.
        double content_right = std::max(cx + rail_w, tile_right);
        return (content_right - x) + pad;
    }

    static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                             double r) {
        if (r * 2 > h) r = h / 2;
        if (r * 2 > w) r = w / 2;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
        cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
    }

    void draw_gradient_rail(cairo_t *cr, double W, double H, double s) {
        double rail_h = px(150 * s, 4);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, H - rail_h, 0, H);
        cairo_pattern_add_color_stop_rgba(g, 0.00, 10/255.0, 11/255.0, 14/255.0, 0.00);
        cairo_pattern_add_color_stop_rgba(g, 0.55, 10/255.0, 11/255.0, 14/255.0, 0.10);
        cairo_pattern_add_color_stop_rgba(g, 1.00, 10/255.0, 11/255.0, 14/255.0, 0.34);
        cairo_rectangle(cr, 0, H - rail_h, W, rail_h);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    void draw_signal_bars(cairo_t *cr, double x, double baseline, int filled,
                          aio::Rgba on_col, double s) {
        const int N = 5;
        const double bw  = px(8 * s, 3);
        const double gap = px(5 * s, 1);
        const double maxh = px(42 * s, 6);
        const double rad = px(2 * s, 1);
        for (int i = 0; i < N; ++i) {
            double frac = 0.32 + (1.0 - 0.32) * (i / double(N - 1));
            double h = px(maxh * frac, 2);
            double bx = x + i * (bw + gap);
            double by = baseline - h;
            aio::Rgba c = (i < filled) ? on_col : aio::Rgba{1, 1, 1, 0.26};
            set_rgba(cr, c);
            rounded_rect(cr, bx, by, bw, h, rad);
            cairo_fill(cr);
        }
    }

    void draw_strip(cairo_t *cr, double W, double H, double s) {
        const double pad_x = px(46 * s, 2);
        const double pad_b = px(26 * s, 2);
        const double baseline = H - pad_b;

        // ---- Left group: VIDEO, WIFI CH -----------------------------------
        ulong vh = arg_u(SLOT_VIDEO_H);
        ulong vfps = arg_u(SLOT_VIDEO_FPS);
        std::string video = vh ? (std::to_string(vh) + "p" + std::to_string(vfps))
                               : std::string("--");
        std::string chan = "--";
        if (last_freq > 0) {
            auto c = aio::freq_to_channel((int)last_freq);
            chan = c ? std::to_string(*c) : (std::to_string(last_freq));
        }
        double x = pad_x;
        x += draw_tile(cr, x, baseline, neutral_tile("VIDEO", video, ""), s);
        x += draw_tile(cr, x, baseline, neutral_tile("WIFI CH", chan, ""), s);

        // ---- Right group: signal bars + 5 metrics (right-anchored) --------
        int lq = aio::link_quality_pct(window_sum(pkt_all), window_sum(pkt_lost));
        aio::Rgba link_col = aio::resolve_color(
            aio::resolve_band(aio::Metric::Link, lq), scheme, false);

        double br = arg_d(SLOT_BITRATE);
        long lat = (long)arg_u(SLOT_LATENCY);
        auto rssi = rssi_agg.best(now_ms());
        auto snr  = snr_agg.best(now_ms());

        std::vector<Tile> right;
        right.push_back(metric_tile("LINK", std::to_string(lq), "%",
                                    aio::Metric::Link, lq));
        right.push_back(metric_tile("BITRATE", fmt1(br), "Mb/s",
                                    aio::Metric::Bitrate, br));
        right.push_back(metric_tile("LATENCY", std::to_string(lat), "ms",
                                    aio::Metric::Latency, (double)lat));
        right.push_back(metric_tile("RSSI",
                                    rssi ? std::to_string(*rssi) : "--", "dBm",
                                    aio::Metric::Rssi, rssi ? (double)*rssi : 0));
        right.push_back(metric_tile("SNR",
                                    snr ? std::to_string(*snr) : "--", "dB",
                                    aio::Metric::Snr, snr ? (double)*snr : 0));

        // Lay the right group out from the right edge: measure, then place.
        // Signal bars sit just left of LINK.
        const double bars_w = 5 * px(8 * s, 3) + 4 * px(5 * s, 1) + px(26 * s, 2);
        // Pre-measure tile widths by drawing to a recording surface is overkill;
        // instead draw left-to-right starting at a computed origin equal to
        // (W - pad_x - total_right_width). Compute total width first.
        double total = bars_w;
        for (auto &t : right) total += measure_tile(cr, t, s);
        double rx = W - pad_x - total;
        draw_signal_bars(cr, rx + px(26 * s, 2) * 0.0, baseline,
                         aio::signal_bar_count(lq), link_col, s);
        rx += bars_w;
        for (auto &t : right) rx += draw_tile(cr, rx, baseline, t, s);
    }

    // Measure a tile's width without committing visible state changes.
    double measure_tile(cairo_t *cr, const Tile &t, double s) {
        const double pad = px(26 * s, 2);
        const double rail_w = px(30 * s, 4);
        font_value(cr, 46 * s);
        cairo_text_extents_t ve; cairo_text_extents(cr, t.value.c_str(), &ve);
        double right = ve.x_advance;
        if (!t.unit.empty()) {
            font_unit(cr, 16 * s);
            cairo_text_extents_t ue; cairo_text_extents(cr, t.unit.c_str(), &ue);
            right += px(6 * s, 1) + ue.x_advance;
        }
        double content = std::max(rail_w, right);
        return pad + content + pad;
    }

    void draw_rec_badge(cairo_t *cr, double W, double H, double s) {
        if (!recording) return;
        long now = now_ms();
        if (now - blink_last_ms >= 1100) { blink_on = !blink_on; blink_last_ms = now; }

        const double top = px(38 * s, 1);
        const double right = px(48 * s, 1);
        const double gap = px(12 * s, 1);
        const double dot = px(14 * s, 4);
        const double rec_sz = 26 * s;

        // Lay out right-anchored: [dot] REC [timecode]
        std::string tc = aio::format_timecode((now - rec_start_ms) / 1000);
        font_rec(cr, rec_sz);
        cairo_text_extents_t re; cairo_text_extents(cr, "REC", &re);
        cairo_text_extents_t te; cairo_text_extents(cr, tc.c_str(), &te);
        double total = dot + gap + re.x_advance + px(10 * s, 1) + te.x_advance;
        double x = W - right - total;
        double cy = top + rec_sz; // baseline-ish

        // Dot (red even in white scheme; hard blink).
        if (blink_on) cairo_set_source_rgba(cr, 0xff/255.0, 0x2e/255.0, 0x3e/255.0, 1);
        else          cairo_set_source_rgba(cr, 0xff/255.0, 0x2e/255.0, 0x3e/255.0, 0.28);
        rounded_rect(cr, x, cy - dot, dot, dot, px(3 * s, 1));
        cairo_fill(cr);
        x += dot + gap;

        font_rec(cr, rec_sz);
        text_shadow(cr, x, cy, "REC", aio::Rgba{1, 1, 1, 1}, s);
        x += re.x_advance + px(10 * s, 1);
        font_unit(cr, rec_sz);
        text_shadow(cr, x, cy, tc, aio::Rgba{1, 1, 1, 0.92}, s);
    }

    // ---- small arg accessors ----
    ulong arg_u(int idx) {
        return args[idx].isDefined() ? (ulong)args[idx] : 0ul;
    }
    double arg_d(int idx) {
        return args[idx].isDefined() ? (double)args[idx] : 0.0;
    }
    long window_sum(const RunningAverage &ra) {
        return ra.get_stats_over_last_ms_result(1000).sum;
    }
    static std::string fmt1(double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", v); return std::string(b);
    }
```

Note: `window_sum` calls `get_stats_over_last_ms_result` which is `const` on `RunningAverage` (src/osd.cpp:708). `arg_u`/`arg_d` use the `Fact` conversion operators (src/osd.cpp:381–418). Ensure `#include <vector>`, `<algorithm>`, and `<cmath>` are available in osd.cpp (cairo + existing includes already pull most; add any missing at the top).

- [ ] **Step 2: Cross-build compile check**

Run the cross-build command. Expected: `PPBUILD_DONE rc=0`. Fix any compile errors (most likely: missing `<vector>`/`<algorithm>` include, or a `const`-ness mismatch on `RunningAverage` accessors — add `#include` or adjust accordingly).

- [ ] **Step 3: Commit**

```sh
git add src/osd.cpp
git commit -m "feat(osd): AIOWidget::draw full overlay (strip, bars, gradient, REC)"
```

---

## Task 10: Factory registration with `{name, type, color_scheme}` defaulting

Wires `type == "AIOWidget"` into `Osd::loadConfig` so it works with no `x`/`y`/`facts`, injecting the default tagless matcher set in Slot order.

**Files:**
- Modify: `src/osd.cpp` — `Osd::loadConfig` (the required-keys guard ~line 1744; the factory chain ~line 1916, before the final `else`)

- [ ] **Step 1: Relax the required-keys guard for AIOWidget**

In `src/osd.cpp`, the loop body reads `x`, `y`, `facts` via `.at()` (lines 1751–1754), which throw if absent. Restructure the per-widget head so AIOWidget defaults them. Replace lines 1749–1773 (from `auto name = ...` through the matcher-building loop) with:
```cpp
                auto name = widget_j.at("name").template get<std::string>();
                auto type = widget_j.at("type").template get<std::string>();
                int x = widget_j.contains("x") ? widget_j.at("x").template get<int>() : 0;
                int y = widget_j.contains("y") ? widget_j.at("y").template get<int>() : 0;
                std::vector<FactMatcher> matchers;
                if (widget_j.contains("facts")) {
                    for(json matcher_j : widget_j.at("facts")) {
                        auto matcher_name = matcher_j.at("name").template get<std::string>();
                        FactTags tags;
                        if (matcher_j.contains("tags")) {
                            for (auto& [key, value] : matcher_j.at("tags").items()) {
                                tags.insert({key, value});
                            }
                        }
                        if (matcher_j.contains("convert")) {
                            auto expression_str = matcher_j.at("convert").template get<std::string>();
                            try {
                                matchers.push_back(FactMatcher(matcher_name, tags, expression_str));
                            } catch (const ExpressionException& e) {
                                spdlog::error("Invalid convert expression {}: {}",
                                              expression_str, e.what());
                            }
                        } else {
                            matchers.push_back(FactMatcher(matcher_name, tags));
                        }
                    }
                }
```
(The only changes vs. the original: `x`/`y` default to 0 when absent, and the `facts` loop is guarded by `widget_j.contains("facts")` instead of an unconditional `.at("facts")`.)

- [ ] **Step 2: Add the AIOWidget factory branch with default matchers**

In `src/osd.cpp`, immediately before the final `} else {  spdlog::warn(... unknown type ...)` (~line 1918), add:
```cpp
            } else if (type == "AIOWidget") {
                aio::Scheme scheme = aio::Scheme::Accent;
                if (widget_j.contains("color_scheme")) {
                    auto cs = widget_j.at("color_scheme").template get<std::string>();
                    if (cs == "white") scheme = aio::Scheme::White;
                    else if (cs != "accent")
                        spdlog::warn("AIOWidget '{}': unknown color_scheme '{}', using accent",
                                     name, cs);
                }
                // Inject default tagless matchers in Slot order when the user
                // did not specify any facts.
                if (matchers.empty()) {
                    matchers.push_back(FactMatcher("video.height"));
                    matchers.push_back(FactMatcher("video.displayed_frame"));
                    matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.freq"));
                    matchers.push_back(FactMatcher("wfbcli.rx.packets.all.delta"));
                    matchers.push_back(FactMatcher("wfbcli.rx.packets.lost.delta"));
                    matchers.push_back(FactMatcher("wfbcli.rx.packets.fec_rec.delta"));
                    matchers.push_back(FactMatcher("gstreamer.received_bytes"));
                    matchers.push_back(FactMatcher("video.latency.total_ms"));
                    matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.rssi_avg"));
                    matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.snr_avg"));
                    matchers.push_back(FactMatcher("dvr.recording"));
                }
                addWidget(new AIOWidget(x, y, scheme), matchers);
```

- [ ] **Step 3: Cross-build compile check**

Run the cross-build command. Expected: `PPBUILD_DONE rc=0`.

- [ ] **Step 4: Commit**

```sh
git add src/osd.cpp
git commit -m "feat(osd): register AIOWidget with {name,type,color_scheme} defaulting"
```

---

## Task 11: Example config + font asset + docs

**Files:**
- Create: `osd-aio.json`
- Modify: `README.md` (add an AIOWidget section)

- [ ] **Step 1: Create the example OSD config**

`osd-aio.json`:
```jsonc
{
    "format": "0.0.1",
    "assets_dir": "/usr/share/pixelpilot/",
    "widgets": [
        { "name": "All-in-one link OSD", "type": "AIOWidget", "color_scheme": "accent" },

        { "name": "msposd", "type": "ExternalSurfaceWidget",
          "x": 0, "y": 0, "width": 0, "height": 0, "facts": [] }
    ]
}
```

- [ ] **Step 2: Document usage in README**

Add to `README.md` (under the OSD/widgets documentation) a short section:
```markdown
### AIOWidget (all-in-one link OSD)

A single widget that renders the full ground-station link overlay (bottom
telemetry strip + top-right REC badge) defined in `design_handoff_gs_osd/`.

Minimal config (accent threshold palette):

    { "name": "All-in-one link OSD", "type": "AIOWidget" }

Monochrome:

    { "name": "All-in-one link OSD", "type": "AIOWidget", "color_scheme": "white" }

It auto-discovers all telemetry (RSSI/SNR/freq across every antenna, link
quality from packet stats) — no `facts`/`x`/`y` needed. See
`docs/superpowers/specs/2026-06-07-aiowidget-design.md`. Requires the Barlow
Condensed font installed on the ground station (see below); without it the OSD
falls back to the default system font.
```

- [ ] **Step 3: Commit**

```sh
git add osd-aio.json README.md
git commit -m "docs(osd): AIOWidget example config + README usage"
```

---

## Task 12: Install Barlow Condensed font on the GS

The OSD resolves "Barlow Condensed" through fontconfig (same toy-API path the current OSD uses for "Roboto"). Install the TTF on the ground station so it resolves.

**Files:** none in-repo (asset is installed on the GS rootfs).

- [ ] **Step 1: Obtain the font**

Download Barlow Condensed (SIL OFL, free) — `BarlowCondensed-SemiBold.ttf` and `BarlowCondensed-ExtraBold.ttf` (and `-SemiBoldItalic`/`-ExtraBoldItalic` if available) from Google Fonts (`https://fonts.google.com/specimen/Barlow+Condensed`). All report family name "Barlow Condensed" with differing weights, so the toy-API weight selection picks the closest face.

- [ ] **Step 2: Copy to the GS font path**

```sh
ssh -o ConnectTimeout=10 root@10.18.0.1 'mkdir -p /usr/share/fonts/barlow'
scp -o ConnectTimeout=10 BarlowCondensed-*.ttf root@10.18.0.1:/usr/share/fonts/barlow/
ssh -o ConnectTimeout=10 root@10.18.0.1 'fc-cache -f /usr/share/fonts/barlow 2>/dev/null; fc-list | grep -i barlow'
```
Expected: `fc-list` prints the installed Barlow Condensed faces. If `fc-cache`/`fc-list` are absent on the GS, fontconfig still scans `/usr/share/fonts` at startup — the copy alone suffices.

- [ ] **Step 3: Note**

`/usr/share` is on the persistent overlay (survives reboot) per the deploy memory, so this is a one-time install. (Persisting the font into the buildroot image is a separate change in the `sbc-groundstations` repo and is out of scope for this plan — record it as a follow-up.)

---

## Task 13: On-device verification (both schemes + 720p)

**Files:** none (manual verification).

- [ ] **Step 1: Deploy the new binary**

Cross-build (Task convention), then deploy:
```sh
scp -o ConnectTimeout=10 output/radxa_zero3_defconfig/build/pixelpilot-custom/pixelpilot \
    root@10.18.0.1:/usr/bin/pixelpilot
ssh -o ConnectTimeout=10 root@10.18.0.1 \
    'pid=$(pidof pixelpilot); kill $pid; sleep 2; md5sum /proc/$(pidof pixelpilot)/exe; md5sum /usr/bin/pixelpilot'
```
Expected: the two md5sums match (the supervisor respawned the new binary).

- [ ] **Step 2: Point the OSD at the AIOWidget config**

Install `osd-aio.json` as the active OSD config on the GS (path per the launch args in `/etc/fpvd/config.json`; commonly `/usr/share/pixelpilot/osd.json` or an `--osd-config` arg). Back up the existing config first:
```sh
ssh -o ConnectTimeout=10 root@10.18.0.1 \
    'cp /usr/share/pixelpilot/osd.json /usr/share/pixelpilot/osd.json.bak 2>/dev/null'
scp -o ConnectTimeout=10 osd-aio.json root@10.18.0.1:/usr/share/pixelpilot/osd.json
ssh -o ConnectTimeout=10 root@10.18.0.1 'kill $(pidof pixelpilot)'
```

- [ ] **Step 3: Verify accent scheme with a live link**

With video flowing, confirm on the goggle/display:
- Bottom strip shows VIDEO (`<h>p<fps>`), WIFI CH (channel number), signal bars, LINK %, BITRATE, LATENCY, RSSI, SNR.
- Threshold colors apply (green/amber/red) and track changing link conditions.
- RSSI/SNR show a sensible best-antenna value; degrade the link and watch them recolor.
- REC badge appears only while recording; dot blinks ~1.1s; timecode counts up `HH:MM:SS`.

- [ ] **Step 4: Verify white scheme**

Edit the GS `osd.json` to `"color_scheme": "white"`, restart pixelpilot (`kill $(pidof pixelpilot)`). Confirm everything renders monochrome white **except** the REC dot stays red.

- [ ] **Step 5: Verify 720p panel scaling (if a 720p panel/output is available)**

Confirm the overlay scales down proportionally and stays crisp (no fuzzy 1px rails/bars), anchored to the bottom and top-right.

- [ ] **Step 6: Restore (if this was a temporary test)**

If reverting: `ssh root@10.18.0.1 'cp /usr/share/pixelpilot/osd.json.bak /usr/share/pixelpilot/osd.json; cp /usr/bin/pixelpilot.stock /usr/bin/pixelpilot; kill $(pidof pixelpilot)'`.

- [ ] **Step 7: Final commit (if any tweaks were needed)**

Commit any code adjustments discovered during device verification (e.g. font weight/size tuning, baseline offsets), each with a clear message.

---

## Spec coverage check

- Cairo `AIOWidget : Widget` in osd.cpp, factory branch → Tasks 8–10. ✓
- Self-anchoring (strip bottom, badge top-right), surface-size driven → Task 9. ✓
- Internal decomposition (band/color/tile/bars/rec/gradient/shadow helpers) → Tasks 2–3, 9. ✓
- Data model + fact mapping (VIDEO/CH/LINK/BITRATE/LATENCY/RSSI/SNR/REC) → Tasks 8–9. ✓
- LINK % from packet stats; signal-bar fill from LINK → Tasks 4, 9. ✓
- Auto antenna aggregation (tagless matchers, max-of-live, staleness, video-id filter) → Tasks 7, 8, 10. ✓
- Threshold tables verbatim → Task 2. ✓
- Two color schemes (white mono / accent palette; REC dot red exception) → Tasks 3, 9. ✓
- Uniform 16:9 scaling incl. 720p, pixel-snapping floors → Task 9, verified Task 13. ✓
- Fonts (Barlow Condensed; graceful fallback) → Tasks 9, 12 (toy-API+fontconfig per Deviation 2). ✓
- Text shadow → Task 9. ✓
- Minimal `{name, type, color_scheme}` config; advanced overrides retained → Task 10. ✓
- Coexistence/migration example → Task 11. ✓
- Tests for band/color/link/channel/timecode/aggregation → Tasks 2–7 (host target per Deviation 1). ✓
- Draw smoke test → replaced by cross-build compile + on-device verification (Deviation 1), Tasks 9, 13.
- `osd.json` example → Task 11. ✓
```
