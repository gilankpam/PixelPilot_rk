# Video Stutter Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `VideoStutterWidget` (compact numeric line) and enable the existing `BarChartWidget` to render a moving stutter graph, both fed by a new GS-local `video.frame_interval_ms` fact published from the display thread.

**Architecture:** Two pure helper functions in a new header `src/video_stutter.hpp` carry the only non-trivial logic (`is_stutter` and `update_peak`), tested directly with Catch2. The widget in `src/osd.cpp` is a thin shell that pipes data through those helpers and into existing `RunningAverage` accumulators, mirroring the pattern of `VideoDecodeLatencyWidget`. The display thread publishes one new fact line. The graph reuses `BarChartWidget` with a small backward-compatible fixed-Y-axis enhancement.

**Tech Stack:** C++17, Catch2 (existing), nlohmann/json (existing), Cairo (existing), `RunningAverage` (existing in `src/osd.cpp`).

**Build & test commands:**
- Tests inside container (preferred): `./tools/container_run_test.sh '[VideoStutter]'`
- Tests locally (if deps installed): `cmake -B build -DBUILD_TESTS=ON && cmake --build build -j$(nproc) --target pixelpilot_tests && ./build/pixelpilot_tests '[VideoStutter]'`
- Full app build (verifies main.cpp + osd.cpp changes compile): `cmake -B build && cmake --build build -j$(nproc) --target pixelpilot`

---

## File Map

| File | Action | Purpose |
|---|---|---|
| `src/video_stutter.hpp` | **create** | Pure helpers: `is_stutter()`, `update_peak()`. Header-only. |
| `tests/test_video_stutter.cpp` | **create** | Catch2 unit tests for the two helpers. |
| `CMakeLists.txt` | **modify** | Add `tests/test_video_stutter.cpp` to `TEST_SOURCES`. |
| `src/osd.cpp` | **modify** | Add `VideoStutterWidget`; add JSON parser branch; extend `BarChartWidget` ctor + `draw()` + parser branch with optional `min_y`/`max_y`. |
| `src/main.cpp` | **modify** | In `__DISPLAY_THREAD__` publish `video.frame_interval_ms`. |

---

## Task 1: Pure helper `is_stutter()` — TDD

**Files:**
- Create: `tests/test_video_stutter.cpp`
- Create: `src/video_stutter.hpp`
- Modify: `CMakeLists.txt:250-254` (add new test source)

- [ ] **Step 1.1: Add test source to CMake**

In `CMakeLists.txt`, change the `TEST_SOURCES` block (around line 250) from:

```cmake
    set(TEST_SOURCES
      tests/test_osd.cpp
      src/main.h
      src/main.cpp
    )
```

to:

```cmake
    set(TEST_SOURCES
      tests/test_osd.cpp
      tests/test_video_stutter.cpp
      src/main.h
      src/main.cpp
    )
```

- [ ] **Step 1.2: Write the failing test**

Create `tests/test_video_stutter.cpp` with:

```cpp
#include <catch2/catch.hpp>
#include <deque>

#include "../src/video_stutter.hpp"

TEST_CASE("is_stutter returns false during warm-up (ring < 30 samples)", "[VideoStutter]") {
    std::deque<long> recent;
    // empty ring
    REQUIRE_FALSE(is_stutter(100, recent, 1.5));

    // 29 samples (still warm-up)
    for (int i = 0; i < 29; ++i) recent.push_back(16);
    REQUIRE_FALSE(is_stutter(500, recent, 1.5));
}

TEST_CASE("is_stutter fires when interval exceeds factor * median", "[VideoStutter]") {
    std::deque<long> recent;
    for (int i = 0; i < 60; ++i) recent.push_back(16);

    // median == 16, threshold = 24
    REQUIRE_FALSE(is_stutter(16, recent, 1.5));   // equal to median
    REQUIRE_FALSE(is_stutter(23, recent, 1.5));   // below threshold
    REQUIRE_FALSE(is_stutter(24, recent, 1.5));   // at threshold (strict >)
    REQUIRE(is_stutter(25, recent, 1.5));         // above threshold
    REQUIRE(is_stutter(100, recent, 1.5));        // way above
}

TEST_CASE("is_stutter adapts to varying stream FPS via median", "[VideoStutter]") {
    std::deque<long> recent;
    // 33ms intervals = ~30fps stream; median == 33, threshold = 49
    for (int i = 0; i < 60; ++i) recent.push_back(33);

    REQUIRE_FALSE(is_stutter(40, recent, 1.5));   // normal jitter
    REQUIRE(is_stutter(50, recent, 1.5));         // genuine stutter
}
```

- [ ] **Step 1.3: Run test to verify it fails (header doesn't exist)**

```
./tools/container_run_test.sh '[VideoStutter]'
```

Expected: build failure, `'video_stutter.hpp' file not found`.

- [ ] **Step 1.4: Create the header with the function**

Create `src/video_stutter.hpp`:

```cpp
#ifndef VIDEO_STUTTER_HPP
#define VIDEO_STUTTER_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

// Returns true if `interval_ms` represents a stutter relative to the recent
// frame intervals. A stutter is `interval > factor * median(recent)`.
//
// During warm-up (fewer than 30 samples in `recent`), always returns false to
// avoid spurious classifications when the median is unstable.
//
// `recent` is treated as read-only. Median is computed via std::nth_element on
// a local copy — O(n) and run at most ~60 times/sec, negligible cost.
inline bool is_stutter(long interval_ms,
                       const std::deque<long>& recent,
                       double factor = 1.5) {
    if (recent.size() < 30) return false;

    std::vector<long> scratch(recent.begin(), recent.end());
    auto mid = scratch.begin() + scratch.size() / 2;
    std::nth_element(scratch.begin(), mid, scratch.end());
    long median = *mid;

    // Strict greater-than: equal-to-threshold is not a stutter.
    return static_cast<double>(interval_ms) > factor * static_cast<double>(median);
}

#endif // VIDEO_STUTTER_HPP
```

- [ ] **Step 1.5: Run tests to verify they pass**

```
./tools/container_run_test.sh '[VideoStutter]'
```

Expected: `All tests passed (X assertions in 3 test cases)`. If the container script reports `FAILED`, inspect the captured output for assertion failures.

- [ ] **Step 1.6: Commit**

```
git add CMakeLists.txt src/video_stutter.hpp tests/test_video_stutter.cpp
git commit -m "feat(stutter): add is_stutter() pure helper + tests"
```

---

## Task 2: Pure helper `update_peak()` — TDD

**Files:**
- Modify: `tests/test_video_stutter.cpp`
- Modify: `src/video_stutter.hpp`

- [ ] **Step 2.1: Add failing tests for update_peak**

Append to `tests/test_video_stutter.cpp`:

```cpp
TEST_CASE("update_peak: stutter-classified frame promotes peak when larger", "[VideoStutter]") {
    long peak = 0;
    uint64_t peak_ts = 0;

    update_peak(peak, peak_ts, /*interval=*/47, /*is_stutter=*/true, /*now_ms=*/1000);
    REQUIRE(peak == 47);
    REQUIRE(peak_ts == 1000);

    // Smaller interval, stutter: should NOT lower peak
    update_peak(peak, peak_ts, 30, true, 1500);
    REQUIRE(peak == 47);
    REQUIRE(peak_ts == 1000);

    // Larger interval, stutter: should promote
    update_peak(peak, peak_ts, 80, true, 2000);
    REQUIRE(peak == 80);
    REQUIRE(peak_ts == 2000);
}

TEST_CASE("update_peak: non-stutter frames never promote peak", "[VideoStutter]") {
    long peak = 0;
    uint64_t peak_ts = 0;

    // Even a huge interval that isn't classified as stutter must not promote.
    update_peak(peak, peak_ts, 500, /*is_stutter=*/false, /*now_ms=*/1000);
    REQUIRE(peak == 0);
    REQUIRE(peak_ts == 0);
}

TEST_CASE("update_peak: expires after 10s and may then be replaced by smaller value", "[VideoStutter]") {
    long peak = 47;
    uint64_t peak_ts = 1000;

    // 9.9s later — not expired yet
    update_peak(peak, peak_ts, 20, true, 1000 + 9'900);
    REQUIRE(peak == 47); // 20 < 47 and not expired, no change

    // 10.001s after original peak_ts — expired; then 20 promotes.
    update_peak(peak, peak_ts, 20, true, 1000 + 10'001);
    REQUIRE(peak == 20);
    REQUIRE(peak_ts == 1000 + 10'001);
}

TEST_CASE("update_peak: expiry without a new stutter resets peak to 0", "[VideoStutter]") {
    long peak = 47;
    uint64_t peak_ts = 1000;

    // 11s later, non-stutter frame: peak must reset to 0.
    update_peak(peak, peak_ts, 16, /*is_stutter=*/false, /*now_ms=*/12'000);
    REQUIRE(peak == 0);
    REQUIRE(peak_ts == 0);
}
```

- [ ] **Step 2.2: Run tests to verify they fail (function doesn't exist)**

```
./tools/container_run_test.sh '[VideoStutter]'
```

Expected: build failure, `'update_peak' was not declared in this scope`.

- [ ] **Step 2.3: Add update_peak() to header**

Append to `src/video_stutter.hpp` (before the closing `#endif`):

```cpp
// Updates the decaying peak in-place. Two steps:
//   1) Expire: if peak is set and older than 10s, reset to (0, 0).
//   2) Promote: only if the new frame is classified as a stutter AND
//      its interval exceeds the current peak.
//
// The "only promote on stutter" rule keeps the ▲ readout meaningful — it
// shows the largest stutter-classified interval in the last 10s, not the
// natural max of normal jitter.
inline void update_peak(long& peak_ms,
                        uint64_t& peak_ts_ms,
                        long interval_ms,
                        bool is_stutter,
                        uint64_t now_ms) {
    constexpr uint64_t PEAK_TTL_MS = 10'000;

    if (peak_ms != 0 && now_ms - peak_ts_ms > PEAK_TTL_MS) {
        peak_ms = 0;
        peak_ts_ms = 0;
    }
    if (is_stutter && interval_ms > peak_ms) {
        peak_ms = interval_ms;
        peak_ts_ms = now_ms;
    }
}
```

- [ ] **Step 2.4: Run tests to verify they pass**

```
./tools/container_run_test.sh '[VideoStutter]'
```

Expected: `All tests passed` covering 7 test cases.

- [ ] **Step 2.5: Commit**

```
git add src/video_stutter.hpp tests/test_video_stutter.cpp
git commit -m "feat(stutter): add update_peak() pure helper + tests"
```

---

## Task 3: `VideoStutterWidget` class

**Files:**
- Modify: `src/osd.cpp` (add `#include`, add class after `VideoDecodeLatencyWidget` ~line 1294)

- [ ] **Step 3.1: Add include for the new header**

In `src/osd.cpp` the include block ends around line 32 (`#include <mutex>`). Add these two lines at the bottom of that block (both standard-header includes are idempotent if already pulled in transitively):

```cpp
#include <deque>
#include "video_stutter.hpp"
```

- [ ] **Step 3.2: Add the widget class right after `VideoDecodeLatencyWidget`**

Insert after the closing `};` of `VideoDecodeLatencyWidget` (around line 1293, immediately before `class GPSWidget`):

```cpp
class VideoStutterWidget: public IconTplTextWidget {
public:
    VideoStutterWidget(int pos_x, int pos_y, uint window_size_ms, uint bucket_size_ms,
                       cairo_surface_t *icon, std::string tpl, uint num_args) :
        IconTplTextWidget(pos_x, pos_y, icon, tpl, 3),
        avg_interval(window_size_ms, bucket_size_ms),
        stutter_events(window_size_ms, bucket_size_ms),
        peak_ms(0),
        peak_ts_ms(0) {
        assert(num_args == 1);
    }

    virtual void setFact(uint idx, Fact fact) {
        assert(idx == 0);
        long interval = static_cast<long>(fact.getUintValue());

        recent.push_back(interval);
        if (recent.size() > RING_CAP) recent.pop_front();

        avg_interval.add(interval);

        bool stutter = is_stutter(interval, recent, 1.5);
        if (stutter) stutter_events.add(1);

        auto now = std::chrono::steady_clock::now();
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch()).count();
        update_peak(peak_ms, peak_ts_ms, interval, stutter, now_ms);

        Stats avg_stats = avg_interval.get_stats_over_last_ms_result(1000);
        args[0] = Fact(FactMeta("interval_avg_ms"), (ulong)avg_stats.average);
        args[1] = Fact(FactMeta("stutter_per_s"),
                       (ulong)stutter_events.rate_per_second_over_last_ms(1000));
        args[2] = Fact(FactMeta("peak_ms"), (ulong)peak_ms);
    }

private:
    static constexpr size_t RING_CAP = 120;
    RunningAverage avg_interval;
    RunningAverage stutter_events;
    std::deque<long> recent;
    long peak_ms;
    uint64_t peak_ts_ms;
};
```

- [ ] **Step 3.3: Build the application to verify it compiles**

```
cmake -B build && cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build. No warnings about unused includes; no errors. The widget is defined but not yet wired to the config parser — that is the next task.

- [ ] **Step 3.4: Commit**

```
git add src/osd.cpp
git commit -m "feat(stutter): add VideoStutterWidget class"
```

---

## Task 4: JSON config-parser branch for `VideoStutterWidget`

**Files:**
- Modify: `src/osd.cpp:1722-1731` (add new branch after `VideoDecodeLatencyWidget`)

- [ ] **Step 4.1: Add the parser branch**

In `src/osd.cpp` find the existing parser branch for `VideoDecodeLatencyWidget` (around line 1722). It ends at line ~1731 with `}` and is immediately followed by `} else if(type == "BoxWidget") {`. Insert the following block on the line *between* them (it begins with `} else if`, so the prior `}` correctly closes the `VideoDecodeLatencyWidget` branch and the following `} else if(...BoxWidget)` closes this new one):

```cpp
            } else if(type == "VideoStutterWidget") {
                auto tpl = widget_j.at("template").template get<std::string>();
                auto icon_path = widget_j.at("icon_path").template get<std::filesystem::path>();
                uint window_size_s = widget_j.at("per_second_window_s").template get<uint>();
                uint bucket_size_ms = widget_j.at("per_second_bucket_ms").template get<uint>();
                cairo_surface_t *icon = openIcon(name, assets_dir, icon_path);
                if (icon == NULL) break;
                addWidget(new VideoStutterWidget(x, y, window_size_s * 1000, bucket_size_ms,
                                                 icon, tpl, (uint)matchers.size()),
                          matchers);
```

This block mirrors `VideoDecodeLatencyWidget`'s parser branch exactly — same JSON keys (`template`, `icon_path`, `per_second_window_s`, `per_second_bucket_ms`), same icon loading, same widget construction shape.

- [ ] **Step 4.2: Build and verify**

```
cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build.

- [ ] **Step 4.3: Commit**

```
git add src/osd.cpp
git commit -m "feat(stutter): wire VideoStutterWidget into OSD config parser"
```

---

## Task 5: Publish `video.frame_interval_ms` from the display thread

**Files:**
- Modify: `src/main.cpp:402-462` (`__DISPLAY_THREAD__`)

- [ ] **Step 5.1: Add interval tracking and publish line**

In `src/main.cpp`, change `__DISPLAY_THREAD__` so that:

1. A local `uint64_t last_commit_ms = 0;` is declared just inside the thread function (above the `while (!frm_eos)` loop at line 407).
2. After the existing `video.decode_and_handover_ms` publish at line 457, add an interval-publish block.

Concretely, replace the trailing section of the loop (lines 455–457):

```cpp
        osd_publish_uint_fact("video.displayed_frame", NULL, 0, 1);
        uint64_t decode_and_handover_display_ms=get_time_ms()-decoding_pts;
        osd_publish_uint_fact("video.decode_and_handover_ms", NULL, 0, decode_and_handover_display_ms);
```

with:

```cpp
        osd_publish_uint_fact("video.displayed_frame", NULL, 0, 1);
        uint64_t now_ms = get_time_ms();
        uint64_t decode_and_handover_display_ms = now_ms - decoding_pts;
        osd_publish_uint_fact("video.decode_and_handover_ms", NULL, 0, decode_and_handover_display_ms);
        if (last_commit_ms != 0) {
            uint64_t interval_ms = now_ms - last_commit_ms;
            osd_publish_uint_fact("video.frame_interval_ms", NULL, 0, interval_ms);
        }
        last_commit_ms = now_ms;
```

And add the `uint64_t last_commit_ms = 0;` declaration right after `pthread_setname_np(pthread_self(), "__DISPLAY");` (around line 405) so it persists across loop iterations:

```cpp
    int ret;
    pthread_setname_np(pthread_self(), "__DISPLAY");
    uint64_t last_commit_ms = 0;
```

Note: `get_time_ms()` is already in scope in this file (see line 425, 456). Skipping the publish on the first iteration (when `last_commit_ms == 0`) ensures no bogus huge interval is reported at startup. Reinitialising would require resetting `last_commit_ms` on resolution change — that is out of scope; an oversized first-after-reinit interval is harmless (it just briefly inflates `▲` until the 10s decay).

- [ ] **Step 5.2: Build and verify**

```
cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build.

- [ ] **Step 5.3: Commit**

```
git add src/main.cpp
git commit -m "feat(stutter): publish video.frame_interval_ms from display thread"
```

---

## Task 6: Extend `BarChartWidget` constructor with optional fixed Y-axis

**Files:**
- Modify: `src/osd.cpp:1019-1021` (constructor and member declarations around line 1009)

- [ ] **Step 6.1: Update constructor signature, initializer list, and member fields**

Find the `BarChartWidget` constructor at `src/osd.cpp:1019`:

```cpp
    BarChartWidget(int pos_x, int pos_y, uint w, uint h, uint window_s, uint num_buckets, BarChartWidget::StatsField stats_field):
        Widget(pos_x, pos_y, 0), w(w), h(h), window_ms(window_s * 1000), num_buckets(num_buckets), stats_field(stats_field),
        stats(window_s * 1000, window_s * 1000 / num_buckets) {};
```

Replace it with:

```cpp
    BarChartWidget(int pos_x, int pos_y, uint w, uint h, uint window_s, uint num_buckets,
                   BarChartWidget::StatsField stats_field,
                   long min_y = -1, long max_y = -1):
        Widget(pos_x, pos_y, 0), w(w), h(h), window_ms(window_s * 1000), num_buckets(num_buckets),
        stats_field(stats_field),
        fixed_y_active(min_y >= 0 && max_y >= 0 && max_y > min_y),
        fixed_min_y(min_y), fixed_max_y(max_y),
        stats(window_s * 1000, window_s * 1000 / num_buckets) {};
```

Then find the existing private member block at the bottom of the class (around `src/osd.cpp:1140-1144`):

```cpp
	uint w, h;
	uint window_ms, num_buckets;
	StatsField stats_field = STATS_SUM;
	RunningAverage stats;
};
```

Add three new member fields just above `RunningAverage stats;`:

```cpp
	uint w, h;
	uint window_ms, num_buckets;
	StatsField stats_field = STATS_SUM;
	bool fixed_y_active = false;
	long fixed_min_y = -1;
	long fixed_max_y = -1;
	RunningAverage stats;
};
```

The defaults match the auto-scale behavior; the constructor will overwrite them when fixed-Y mode is requested.

- [ ] **Step 6.2: Build and verify (no behavior change yet)**

```
cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build. The existing call site at `osd.cpp:1762` still compiles because the new parameters default to `-1` and `fixed_y_active` will be `false` for every existing usage.

- [ ] **Step 6.3: Commit**

```
git add src/osd.cpp
git commit -m "refactor(BarChartWidget): add optional fixed Y-axis ctor params (no behavior change)"
```

---

## Task 7: Use the fixed Y-axis in `BarChartWidget::draw()`

**Files:**
- Modify: `src/osd.cpp:1034-1085` (`BarChartWidget::draw`)

- [ ] **Step 7.1: Replace min/max derivation and bar-height computation**

In `BarChartWidget::draw()` find the block at `src/osd.cpp:1047-1063`:

```cpp
        std::vector<double> stats = select_stats(all_stats);
        double min = *std::min_element(stats.begin(), stats.end());
        double max = *std::max_element(stats.begin(), stats.end());

        // legend
        cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
        cairo_move_to(cr, x + 2, y + 15);
        cairo_show_text(cr, shorten(max).c_str());

        cairo_move_to(cr, x + 2, y + h);
        cairo_show_text(cr, shorten(min).c_str());

        // bars
        cairo_set_source_rgba(cr, 200.0, 200.0, 200.0, 0.8);

        double scale = max - min;
```

Replace with:

```cpp
        std::vector<double> stats = select_stats(all_stats);
        double min;
        double max;
        if (fixed_y_active) {
            min = static_cast<double>(fixed_min_y);
            max = static_cast<double>(fixed_max_y);
        } else {
            min = *std::min_element(stats.begin(), stats.end());
            max = *std::max_element(stats.begin(), stats.end());
        }

        // legend
        cairo_set_source_rgba(cr, 255.0, 255.0, 255.0, 1);
        cairo_move_to(cr, x + 2, y + 15);
        cairo_show_text(cr, shorten(static_cast<long>(max)).c_str());

        cairo_move_to(cr, x + 2, y + h);
        cairo_show_text(cr, shorten(static_cast<long>(min)).c_str());

        // bars
        cairo_set_source_rgba(cr, 200.0, 200.0, 200.0, 0.8);

        double scale = max - min;
```

Then find the bar-rendering loop a few lines below (`src/osd.cpp:1074-1084`):

```cpp
        for (auto val : stats) {
            double normalized = val - min;
            double bar_h = -1.0 * (normalized * (h - 10)) / scale;
            // h -> max-min
            // ? -> normalized
            SPDLOG_TRACE("val {}, cairo_rectangle(cr, {}, {}, {}, {})",
                         val, bar_x, y + h, bar_w, bar_h);
            cairo_rectangle(cr, bar_x, y + h, bar_w, bar_h - 2);
            cairo_fill(cr);
            bar_x += (bar_pad + bar_w);
        }
```

Replace with:

```cpp
        for (auto val : stats) {
            // Clamp to [min, max] when fixed-Y is active; in auto mode this is a no-op
            // because min/max were derived from these same values.
            double clamped = std::max(min, std::min(max, val));
            double normalized = clamped - min;
            double bar_h = scale > 0 ? -1.0 * (normalized * (h - 10)) / scale : 0.0;
            SPDLOG_TRACE("val {}, cairo_rectangle(cr, {}, {}, {}, {})",
                         val, bar_x, y + h, bar_w, bar_h);
            cairo_rectangle(cr, bar_x, y + h, bar_w, bar_h - 2);
            cairo_fill(cr);
            bar_x += (bar_pad + bar_w);
        }
```

The `scale > 0` guard prevents division-by-zero in the previously-unreachable case where `min == max` (now reachable in auto mode if all buckets are identical, and theoretically reachable in fixed mode if a downstream caller passes `min_y == max_y` — already filtered out by `fixed_y_active`, but the guard is cheap).

- [ ] **Step 7.2: Build and verify**

```
cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build. Existing `BarChartWidget` configs continue to behave as before (auto-scale) since `fixed_y_active` defaults to `false`.

- [ ] **Step 7.3: Commit**

```
git add src/osd.cpp
git commit -m "feat(BarChartWidget): honor fixed Y-axis when configured"
```

---

## Task 8: JSON parser support for `BarChartWidget` `min_y`/`max_y`

**Files:**
- Modify: `src/osd.cpp:1741-1763` (`BarChartWidget` parser branch)

- [ ] **Step 8.1: Read optional fields and pass to constructor**

Find the `BarChartWidget` branch at `src/osd.cpp:1741-1763`. Replace its final `addWidget(...)` call (currently `addWidget(new BarChartWidget(x, y, width, height, window_s, num_buckets, stats_kind), matchers);`) with:

```cpp
                long min_y = -1;
                long max_y = -1;
                if (widget_j.contains("min_y")) {
                    min_y = widget_j.at("min_y").template get<long>();
                }
                if (widget_j.contains("max_y")) {
                    max_y = widget_j.at("max_y").template get<long>();
                }
                addWidget(new BarChartWidget(x, y, width, height, window_s, num_buckets,
                                             stats_kind, min_y, max_y),
                          matchers);
```

The two fields are optional; existing configs without them continue to work.

- [ ] **Step 8.2: Build and verify**

```
cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build.

- [ ] **Step 8.3: Commit**

```
git add src/osd.cpp
git commit -m "feat(BarChartWidget): expose min_y/max_y in OSD config"
```

---

## Task 9: Full test sweep + manual verification

**Files:** none (verification only)

- [ ] **Step 9.1: Run the full unit-test suite**

```
./tools/container_run_test.sh
```

Expected: all existing tests still pass plus the 7 new `[VideoStutter]` tests. No `FAILED` in output.

- [ ] **Step 9.2: Build the final binary**

```
cmake -B build && cmake --build build -j$(nproc) --target pixelpilot
```

Expected: clean build with no warnings related to changed files.

- [ ] **Step 9.3: Manual smoke check — copy these JSON snippets into a local OSD config**

For the pilot to enable the new readouts, add to the `widgets` array of an OSD JSON config (e.g. a copy of `config_osd.json`):

```json
{
    "name": "Video stutter (numeric)",
    "type": "VideoStutterWidget",
    "x": -250,
    "y": 90,
    "icon_path": "framerate.png",
    "template": "STUT %u ms · %u/s ▲%u",
    "per_second_window_s": 1,
    "per_second_bucket_ms": 100,
    "facts": [
        { "name": "video.frame_interval_ms" }
    ]
},
{
    "name": "Video stutter (graph)",
    "type": "BarChartWidget",
    "x": -250,
    "y": 110,
    "width": 200,
    "height": 60,
    "window_s": 10,
    "num_buckets": 20,
    "stats_kind": "max",
    "min_y": 0,
    "max_y": 100,
    "facts": [
        { "name": "video.frame_interval_ms" }
    ]
}
```

(Pick `x`/`y` positions that suit the actual layout.)

- [ ] **Step 9.4: Confirm in the field (out of band)**

Launch `pixelpilot` on the GS with the modified OSD config and a live RTP source. Confirm:
- The numeric widget shows `STUT N ms · 0/s ▲0` during calm flight, with N ≈ frame interval (~16 at 60fps).
- Briefly disrupting the RTP source (unplug antenna, or `gst-launch ... ! identity sleep-time=200000`) causes the `/s` count to rise and `▲` to jump to the spike magnitude.
- 10s after the disruption, `▲` returns to `0`.
- The bar chart shows mostly low bars during calm flight (since `max_y=100`, a healthy 16ms bar is ~16% tall) and a tall bar that walks left over 10s after a spike.

This step requires the physical GS hardware; flag in the PR description that it has not been performed if you cannot run it.

---

## Self-Review Notes

- **Spec coverage check:**
  - Display format `STUT 18 ms · 3/s ▲47` → Task 3 template + Task 9.3 JSON.
  - 1.5× threshold, ≥30 sample warm-up, 120-sample ring → Task 1 (`is_stutter` impl) + Task 3 widget (`RING_CAP = 120`).
  - Peak only on stutter, 10s decay → Task 2 (`update_peak` impl) + tests.
  - `video.frame_interval_ms` fact published after `drmModeAtomicCommit` → Task 5.
  - Skip first interval after startup/reinit → Task 5 (`last_commit_ms != 0` guard); reinit case explicitly noted as out of scope with rationale.
  - `RunningAverage(1000ms, 100ms)` for avg + stutter rate → Task 3 (`per_second_window_s=1`, `per_second_bucket_ms=100` in JSON).
  - `BarChartWidget` fixed Y-axis with `min_y`/`max_y`, both required for activation, clamping → Tasks 6, 7, 8.
  - Recommended graph config (200x60, 10s, 20 buckets, max, 0..100) → Task 9.3 JSON.
  - Cross-clock constraint (CLOCK_MONOTONIC only) → satisfied because both `get_time_ms()` (main.cpp) and `std::chrono::steady_clock` (osd.cpp) are GS-local monotonic clocks.

- **Out-of-scope items confirmed absent from plan:** no GStreamer `rtpjitterbuffer` stats hookup, no drop counter, no color thresholds, no CLI flag, no RTP-PTS plumbing. Good.
