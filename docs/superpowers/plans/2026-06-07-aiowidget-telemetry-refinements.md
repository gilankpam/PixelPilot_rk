# AIOWidget Telemetry & Layout Refinements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Source the AIOWidget VIDEO tile from the fpvd air config (static `1080p60`, auto-updating on gsmenu change), add a live FPS tile beside LATENCY (deviation-colored vs configured fps), make the layout fixed so changing numbers never shift the OSD, and repoint the signal bars to RSSI strength.

**Architecture:** Three new pure helpers in the cairo-free `osd_aio_logic` unit (host-tested); one new in-process "air-info bridge" that republishes the configured air resolution/fps as OSD facts (reusing gsmenu's existing fpvd polling, public APIs only); and a rewire of `AIOWidget` in `osd.cpp` (slot remap + live-fps slot + fixed-width tiles + RSSI bars). Builds on the already-shipped AIOWidget.

**Tech Stack:** C/C++17, Cairo, fpvd (curl, in `settings_fpvd.c`), nlohmann/json, Catch2, CMake. Spec: `docs/superpowers/specs/2026-06-07-aiowidget-telemetry-refinements-design.md`.

---

## Conventions (every task uses these)

- **Host logic build/test** (this dev box; for the `osd_aio_logic` tasks):
  - Build: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
  - Run: `nix-shell --run './build-test/aio_logic_tests "[aio]"'`
  - (If `build-test/` is missing: `nix-shell --run 'cmake -DUSE_SIMULATOR=ON -S . -B build-test'` first.)
- **Cross-build compile check** (for `src/osd.cpp` and the bridge — the only way to compile them; INCREMENTAL, so fast). Run via Bash with `timeout` 600000, no `cd` (the command cd's internally):
  ```sh
  printf '%s\n' \
    'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
    'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
    'export DEFCONFIG=radxa_zero3_defconfig' \
    './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' \
    | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix
  ```
  Success = output contains `PPBUILD_DONE rc=0`. Built binary: `output/radxa_zero3_defconfig/build/pixelpilot-custom/pixelpilot` under the sbc repo.
- **Catch2 v3 header** is already in `tests/test_aio_logic.cpp` (`#include <catch2/catch_test_macros.hpp>`). Just append TEST_CASEs.

## File structure

| File | Change |
|---|---|
| `src/osd_aio_logic.hpp` / `.cpp` | add `format_video_mode`, `rssi_to_bars`, `fps_band`; remove `signal_bar_count` |
| `tests/test_aio_logic.cpp` | add tests for the 3 new fns; remove the `signal_bar_count` test |
| `src/gsmenu/osd_air_bridge.h` / `.c` | **new** — publishes `air.video.resolution` (str) + `air.video.fps` (int) facts from the settings snapshot |
| `src/main.cpp` | include the bridge header; call `pp_osd_air_bridge_init()` after `pp_settings_register_fpvd()` (line 1434) |
| `CMakeLists.txt` | add the bridge `.c`/`.h` to the device (`list(APPEND SOURCE_FILES …)`) block |
| `src/osd.cpp` | `AIOWidget`: Slot remap (+`SLOT_FPS_LIVE`), `setFact`, factory default matchers, `arg_s`, draw (VIDEO via `format_video_mode`, FPS tile, RSSI bars, fixed-width tiles) |

---

## Task 1: `format_video_mode` (host TDD)

**Files:** `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("format_video_mode", "[aio]") {
    using aio::format_video_mode;
    REQUIRE(format_video_mode("1920x1080", 60) == "1080p60");
    REQUIRE(format_video_mode("1280x720", 120) == "720p120");
    REQUIRE(format_video_mode("960x540", 60) == "540p60");
    REQUIRE(format_video_mode("1920x1080", 0) == "1080p");   // fps<=0 -> omit
    REQUIRE(format_video_mode("foo", 60) == "foo");          // not WxH -> raw
    REQUIRE(format_video_mode("", 60) == "");                // empty -> empty
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `format_video_mode` not declared.

- [ ] **Step 3: Declare** — in `src/osd_aio_logic.hpp`, before the closing `} // namespace aio`:
```cpp
// "1920x1080", 60 -> "1080p60". Parses the height after 'x'; if the string isn't
// WxH with a numeric height, returns it unchanged; if fps <= 0 the fps suffix is
// omitted; empty input -> "".
std::string format_video_mode(const std::string& resolution, int fps);
```

- [ ] **Step 4: Implement** — in `src/osd_aio_logic.cpp`, before the closing `} // namespace aio`:
```cpp
std::string format_video_mode(const std::string& resolution, int fps) {
    if (resolution.empty()) return "";
    auto xpos = resolution.find('x');
    if (xpos == std::string::npos) return resolution;       // not WxH -> raw
    std::string h = resolution.substr(xpos + 1);
    if (h.empty()) return resolution;
    for (char c : h) if (c < '0' || c > '9') return resolution; // non-numeric -> raw
    std::string out = h + "p";
    if (fps > 0) out += std::to_string(fps);
    return out;
}
```

- [ ] **Step 5: Run test to verify it passes**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**
```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget format_video_mode helper"
```

---

## Task 2: `rssi_to_bars` (host TDD)

**Files:** `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("rssi_to_bars", "[aio]") {
    using aio::rssi_to_bars;
    REQUIRE(rssi_to_bars(-55) == 5);
    REQUIRE(rssi_to_bars(-90) == 0);
    REQUIRE(rssi_to_bars(-40) == 5);    // clamp high
    REQUIRE(rssi_to_bars(-100) == 0);   // clamp low
    REQUIRE(rssi_to_bars(-62) == 4);
    REQUIRE(rssi_to_bars(-70) == 3);
    REQUIRE(rssi_to_bars(-80) == 1);
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `rssi_to_bars` not declared.

- [ ] **Step 3: Declare** — in `src/osd_aio_logic.hpp`, before the closing namespace brace:
```cpp
// RSSI dBm -> 0..5 bars, linear: -90 dBm -> 0, -55 dBm -> 5, clamped.
int rssi_to_bars(int rssi_dbm);
```

- [ ] **Step 4: Implement** — in `src/osd_aio_logic.cpp` (uses `<cmath>`, already included), before the closing namespace brace:
```cpp
int rssi_to_bars(int rssi_dbm) {
    const double lo = -90.0, hi = -55.0;
    double f = (static_cast<double>(rssi_dbm) - lo) / (hi - lo);
    int n = static_cast<int>(std::lround(f * 5.0));
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
git commit -m "feat(osd): AIOWidget rssi_to_bars helper"
```

---

## Task 3: `fps_band` (host TDD)

**Files:** `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/test_aio_logic.cpp`:
```cpp
TEST_CASE("fps_band", "[aio]") {
    using aio::fps_band; using aio::Band;
    REQUIRE(fps_band(60, 0) == Band::Neutral);   // no reference
    REQUIRE(fps_band(60, 60) == Band::Good);
    REQUIRE(fps_band(54, 60) == Band::Good);      // 0.90
    REQUIRE(fps_band(53, 60) == Band::Warn);
    REQUIRE(fps_band(42, 60) == Band::Warn);      // 0.70
    REQUIRE(fps_band(41, 60) == Band::Crit);
}
```

- [ ] **Step 2: Run test to verify it fails**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -20'`
Expected: FAIL — `fps_band` not declared.

- [ ] **Step 3: Declare** — in `src/osd_aio_logic.hpp`, before the closing namespace brace:
```cpp
// Live vs configured fps -> band. configured <= 0 -> Neutral (no reference).
// ratio >= 0.90 -> Good; >= 0.70 -> Warn; else Crit.
Band fps_band(int live_fps, int configured_fps);
```

- [ ] **Step 4: Implement** — in `src/osd_aio_logic.cpp`, before the closing namespace brace:
```cpp
Band fps_band(int live_fps, int configured_fps) {
    if (configured_fps <= 0) return Band::Neutral;
    double ratio = static_cast<double>(live_fps) / static_cast<double>(configured_fps);
    if (ratio >= 0.90) return Band::Good;
    if (ratio >= 0.70) return Band::Warn;
    return Band::Crit;
}
```

- [ ] **Step 5: Run test to verify it passes**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS.

- [ ] **Step 6: Commit**
```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "feat(osd): AIOWidget fps_band (live vs configured) helper"
```

---

## Task 4: Air-info bridge (cross-build)

Publishes the configured air resolution/fps as OSD facts. Compiles only in the GS cross-build.

**Files:**
- Create: `src/gsmenu/osd_air_bridge.h`, `src/gsmenu/osd_air_bridge.c`
- Modify: `CMakeLists.txt` (device source list, ~line 301), `src/main.cpp` (include + init call after line 1434)

- [ ] **Step 1: Create the header** — `src/gsmenu/osd_air_bridge.h`:
```c
#ifndef OSD_AIR_BRIDGE_H
#define OSD_AIR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers a settings snapshot listener that republishes the configured air
 * video resolution + fps (sourced from fpvd GET /air/config via the settings
 * snapshot) as OSD facts "air.video.resolution" (string) and "air.video.fps"
 * (int). Call once at startup, AFTER pp_settings_register_fpvd(). */
void pp_osd_air_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* OSD_AIR_BRIDGE_H */
```

- [ ] **Step 2: Create the source** — `src/gsmenu/osd_air_bridge.c`:
```c
#include "osd_air_bridge.h"
#include "settings.h"   /* pp_settings_get, pp_settings_set_snapshot_listener */
#include "../osd.h"     /* osd_publish_str_fact, osd_publish_int_fact */

#include <stdlib.h>     /* free, atoi */

/* Read the configured air resolution + fps from the current settings snapshot
 * and (re)publish them as OSD facts. pp_settings_get returns a heap string the
 * caller must free(); NULL/empty means "not available yet" -> skip that fact. */
static void publish_air_video(void *ud) {
    (void)ud;
    char *res = pp_settings_get("air", "camera", "size"); /* e.g. "1920x1080" */
    char *fps = pp_settings_get("air", "camera", "fps");  /* e.g. "60" */
    if (res && res[0]) {
        osd_publish_str_fact("air.video.resolution", NULL, 0, res);
    }
    if (fps && fps[0]) {
        osd_publish_int_fact("air.video.fps", NULL, 0, (long)atoi(fps));
    }
    free(res);
    free(fps);
}

void pp_osd_air_bridge_init(void) {
    /* Fires on every snapshot refresh (boot, periodic poll, and post-apply). */
    pp_settings_set_snapshot_listener(publish_air_video, NULL);
    /* Publish once now in case a snapshot already arrived before registration. */
    publish_air_video(NULL);
}
```

- [ ] **Step 3: Add the bridge to the device source list** — in `CMakeLists.txt`, the block at ~line 300:
```cmake
  # fpvd provider sources (device build)
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_fpvd.c
      third_party/cjson/cJSON.c
  )
```
becomes:
```cmake
  # fpvd provider sources (device build)
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_fpvd.c
      src/gsmenu/osd_air_bridge.h
      src/gsmenu/osd_air_bridge.c
      third_party/cjson/cJSON.c
  )
```

- [ ] **Step 4: Wire the init call into main.cpp** — add the include near the other gsmenu include (`#include "gsmenu/settings.h"` is at line 63):
```cpp
#include "gsmenu/osd_air_bridge.h"
```
Then find `pp_settings_register_fpvd();` (line 1434) and add the init call immediately after it:
```cpp
	pp_settings_register_fpvd();
	pp_osd_air_bridge_init();
```

- [ ] **Step 5: Cross-build compile check**
Run the cross-build command (Conventions). Expected: `PPBUILD_DONE rc=0`. Likely fixes if it errors: include path for `osd.h` (the `../osd.h` relative include is intentional — keep it), or a missing `extern "C"` (the header already guards it).

- [ ] **Step 6: Commit**
```sh
git add src/gsmenu/osd_air_bridge.h src/gsmenu/osd_air_bridge.c CMakeLists.txt src/main.cpp
git commit -m "feat(osd): air-info bridge publishing air.video.resolution/fps facts"
```

---

## Task 5: Rewire AIOWidget (cross-build)

Single atomic change to `AIOWidget` in `src/osd.cpp`: remap slots, add the live-fps slot, switch tiles to fixed reserved widths, source VIDEO from the air-config facts, add the deviation-colored FPS tile, and repoint the signal bars to RSSI. (All interdependent, so one task = one clean compile.)

**Files:** `src/osd.cpp` (AIOWidget class + the `type == "AIOWidget"` factory branch)

- [ ] **Step 1: Replace the `Slot` enum** — replace the current enum (`SLOT_VIDEO_H = 0` … `SLOT_COUNT`) with:
```cpp
    enum Slot {
        SLOT_VIDEO_RES = 0, // air.video.resolution    (string)
        SLOT_VIDEO_FPS,     // air.video.fps           (int, configured)
        SLOT_FREQ,          // wfbcli.rx.ant_stats.freq(uint, per-antenna)
        SLOT_PKT_ALL,       // wfbcli.rx.packets.all.delta   (uint)
        SLOT_PKT_LOST,      // wfbcli.rx.packets.lost.delta  (uint)
        SLOT_PKT_FEC,       // wfbcli.rx.packets.fec_rec.delta (uint, reserved)
        SLOT_BITRATE,       // gstreamer.received_bytes(uint, -> Mb/s)
        SLOT_LATENCY,       // video.latency.total_ms  (uint)
        SLOT_FPS_LIVE,      // video.displayed_frame   (uint, -> per-second)
        SLOT_RSSI,          // wfbcli.rx.ant_stats.rssi_avg (int, per-antenna)
        SLOT_SNR,           // wfbcli.rx.ant_stats.snr_avg  (int, per-antenna)
        SLOT_REC,           // dvr.recording           (bool)
        SLOT_COUNT
    };
```

- [ ] **Step 2: Update `setFact`** — in the `switch (idx)`, remove the `case SLOT_VIDEO_FPS:` block (fps is now a static int → handled by `default:`), and add a `SLOT_FPS_LIVE` case carrying the old per-second-rate logic. Replace the old `SLOT_VIDEO_FPS` case:
```cpp
        case SLOT_VIDEO_FPS:
            fps.add(static_cast<long>(fact.getUintValue()));
            args[idx] = Fact(FactMeta("aio.fps"),
                             (ulong)fps.rate_per_second_over_last_ms(1000));
            break;
```
with:
```cpp
        case SLOT_FPS_LIVE:
            fps.add(static_cast<long>(fact.getUintValue()));
            args[idx] = Fact(FactMeta("aio.fps_live"),
                             (ulong)fps.rate_per_second_over_last_ms(1000));
            break;
```
(The `default:` branch already stores the fact as-is, which now covers `SLOT_VIDEO_RES` (string), `SLOT_VIDEO_FPS` (static int), `SLOT_LATENCY`, and `SLOT_PKT_FEC`. Update its trailing comment to `// SLOT_VIDEO_RES, SLOT_VIDEO_FPS, SLOT_LATENCY, SLOT_PKT_FEC`.) The `fps` `RunningAverage` member and its ctor init stay unchanged.

- [ ] **Step 3: Add a string accessor** — next to the other `arg_*` accessors (after `arg_d`), add:
```cpp
    std::string arg_s(int idx) {
        return (args[idx].isDefined() && args[idx].getType() == Fact::T_STRING)
                   ? args[idx].getStrValue() : std::string();
    }
```

- [ ] **Step 4: Add `reserve` to `Tile` and update the tile builders** — replace the `Tile` struct and the `metric_tile`/`neutral_tile` builders with:
```cpp
    struct Tile {
        std::string label;
        std::string value;
        std::string unit;     // may be empty
        aio::Rgba value_col;
        aio::Rgba rail_col;
        std::string reserve;  // width sample (widest expected value); fixes layout
    };

    aio::Rgba neutral_rail() const { return aio::Rgba{1, 1, 1, 0.5}; }
    aio::Rgba label_col() const    { return aio::Rgba{1, 1, 1, 0.62}; }
    aio::Rgba unit_col() const     { return aio::Rgba{1, 1, 1, 0.66}; }

    // Threshold-/band-colored tile. Pass the resolved band directly so callers
    // can use any band source (resolve_band, fps_band, ...).
    Tile metric_tile(const std::string &label, const std::string &value,
                     const std::string &unit, aio::Band band,
                     const std::string &reserve) {
        aio::Rgba col = aio::resolve_color(band, scheme, false);
        aio::Rgba rail = (scheme == aio::Scheme::Accent && band != aio::Band::Neutral)
                             ? col : aio::Rgba{1, 1, 1, 1};
        return Tile{label, value, unit, col, rail, reserve};
    }
    Tile neutral_tile(const std::string &label, const std::string &value,
                      const std::string &unit, const std::string &reserve) {
        return Tile{label, value, unit,
                    aio::Rgba{1, 1, 1, 1}, neutral_rail(), reserve};
    }
```

- [ ] **Step 5: Make `draw_tile` and `measure_tile` use the reserved width** — replace both with:
```cpp
    // Returns the tile's drawn width so the caller can advance x. The value-field
    // width comes from t.reserve (a fixed sample), NOT the live value, so the
    // value, unit, and all neighbouring tiles stay put as digits change.
    double draw_tile(cairo_t *cr, double x, double baseline, const Tile &t, double s) {
        const double pad   = px(26 * s, 2);
        const double rail_w = px(30 * s, 4);
        const double rail_h = px(4 * s, 2);
        const double gap    = px(2 * s, 1);
        const double label_sz = 14 * s;
        const double value_sz = 46 * s;
        const double unit_sz  = 16 * s;

        double cx = x + pad;
        double rail_y = baseline - value_sz - label_sz - gap * 2 - rail_h;
        set_rgba(cr, t.rail_col);
        rounded_rect(cr, cx, rail_y, rail_w, rail_h, px(2 * s, 1));
        cairo_fill(cr);

        font_label(cr, label_sz);
        double label_y = rail_y + rail_h + gap + label_sz;
        text_shadow(cr, cx, label_y, t.label, label_col(), s);

        // Reserved value-field width from the widest sample.
        font_value(cr, value_sz);
        cairo_text_extents_t re;
        cairo_text_extents(cr, t.reserve.c_str(), &re);
        double value_field = re.x_advance;

        // Value, left-aligned within the reserved field.
        double value_y = baseline;
        text_shadow(cr, cx, value_y, t.value, t.value_col, s);

        // Unit (optional) at a fixed offset after the reserved value field.
        double tile_right = cx + value_field;
        if (!t.unit.empty()) {
            font_unit(cr, unit_sz);
            double ux = cx + value_field + px(6 * s, 1);
            text_shadow(cr, ux, value_y, t.unit, unit_col(), s);
            cairo_text_extents_t ue;
            cairo_text_extents(cr, t.unit.c_str(), &ue);
            tile_right = ux + ue.x_advance;
        }
        double content_right = std::max(cx + rail_w, tile_right);
        return (content_right - x) + pad;
    }
```
and
```cpp
    // Measure a tile's width from its reserve sample. Note: sets Cairo font state
    // (font_value/font_unit); draw_tile re-sets the font, and draw()'s
    // save/restore bounds it.
    double measure_tile(cairo_t *cr, const Tile &t, double s) {
        const double pad = px(26 * s, 2);
        const double rail_w = px(30 * s, 4);
        font_value(cr, 46 * s);
        cairo_text_extents_t re; cairo_text_extents(cr, t.reserve.c_str(), &re);
        double right = re.x_advance;
        if (!t.unit.empty()) {
            font_unit(cr, 16 * s);
            cairo_text_extents_t ue; cairo_text_extents(cr, t.unit.c_str(), &ue);
            right += px(6 * s, 1) + ue.x_advance;
        }
        double content = std::max(rail_w, right);
        return pad + content + pad;
    }
```

- [ ] **Step 6: Rewrite `draw_strip`** — replace the whole `draw_strip` body with:
```cpp
    void draw_strip(cairo_t *cr, double W, double H, double s) {
        const double pad_x = px(46 * s, 2);
        const double pad_b = px(26 * s, 2);
        const double baseline = H - pad_b;

        // ---- Left group: VIDEO (configured air mode), WIFI CH -------------
        std::string res = arg_s(SLOT_VIDEO_RES);
        int cfg_fps = (int)arg_u(SLOT_VIDEO_FPS);
        std::string video = res.empty() ? std::string("--")
                                        : aio::format_video_mode(res, cfg_fps);
        std::string chan = "--";
        if (last_freq > 0) {
            auto c = aio::freq_to_channel((int)last_freq);
            chan = c ? std::to_string(*c) : (std::to_string(last_freq));
        }
        double x = pad_x;
        x += draw_tile(cr, x, baseline, neutral_tile("VIDEO", video, "", "1080p120"), s);
        x += draw_tile(cr, x, baseline, neutral_tile("WIFI CH", chan, "", "8888"), s);

        // ---- Right group (right-anchored, fixed widths) ------------------
        int lq = aio::link_quality_pct(window_sum(pkt_all), window_sum(pkt_lost));
        double br = arg_d(SLOT_BITRATE);
        long lat = (long)arg_u(SLOT_LATENCY);
        auto rssi = rssi_agg.best(now_ms());
        auto snr  = snr_agg.best(now_ms());

        // Live FPS tile, deviation-colored vs configured fps.
        Tile fps_t = neutral_tile("FPS", "--", "", "888");
        if (args[SLOT_FPS_LIVE].isDefined()) {
            int live = (int)arg_u(SLOT_FPS_LIVE);
            aio::Band fb = aio::fps_band(live, cfg_fps);
            fps_t = (fb == aio::Band::Neutral)
                ? neutral_tile("FPS", std::to_string(live), "", "888")
                : metric_tile("FPS", std::to_string(live), "", fb, "888");
        }

        std::vector<Tile> right;
        right.push_back(metric_tile("LINK", std::to_string(lq), "%",
                                    aio::resolve_band(aio::Metric::Link, lq), "100"));
        right.push_back(metric_tile("BITRATE", fmt1(br), "Mb/s",
                                    aio::resolve_band(aio::Metric::Bitrate, br), "888.8"));
        right.push_back(metric_tile("LATENCY", std::to_string(lat), "ms",
                                    aio::resolve_band(aio::Metric::Latency, (double)lat), "888"));
        right.push_back(fps_t);
        right.push_back(rssi
            ? metric_tile("RSSI", std::to_string(*rssi), "dBm",
                          aio::resolve_band(aio::Metric::Rssi, (double)*rssi), "-888")
            : neutral_tile("RSSI", "--", "dBm", "-888"));
        right.push_back(snr
            ? metric_tile("SNR", std::to_string(*snr), "dB",
                          aio::resolve_band(aio::Metric::Snr, (double)*snr), "88")
            : neutral_tile("SNR", "--", "dB", "88"));

        // Signal bars = RSSI strength, RSSI-band colored.
        int bars = rssi ? aio::rssi_to_bars((int)*rssi) : 0;
        aio::Rgba bar_col = rssi
            ? aio::resolve_color(aio::resolve_band(aio::Metric::Rssi, (double)*rssi), scheme, false)
            : aio::Rgba{1, 1, 1, 1};

        const double bars_w = 5 * px(8 * s, 3) + 4 * px(5 * s, 1) + px(26 * s, 2);
        double total = bars_w;
        for (auto &t : right) total += measure_tile(cr, t, s);
        double rx = W - pad_x - total;
        draw_signal_bars(cr, rx, baseline, bars, bar_col, s);
        rx += bars_w;
        for (auto &t : right) rx += draw_tile(cr, rx, baseline, t, s);
    }
```

- [ ] **Step 7: Update the factory default matchers** — in the `else if (type == "AIOWidget")` branch, replace the 11 `matchers.push_back(...)` lines inside `if (matchers.empty()) {` with these 12 (order MUST match the new Slot enum):
```cpp
					matchers.push_back(FactMatcher("air.video.resolution"));          // SLOT_VIDEO_RES
					matchers.push_back(FactMatcher("air.video.fps"));                 // SLOT_VIDEO_FPS
					matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.freq"));      // SLOT_FREQ
					matchers.push_back(FactMatcher("wfbcli.rx.packets.all.delta"));   // SLOT_PKT_ALL
					matchers.push_back(FactMatcher("wfbcli.rx.packets.lost.delta"));  // SLOT_PKT_LOST
					matchers.push_back(FactMatcher("wfbcli.rx.packets.fec_rec.delta"));// SLOT_PKT_FEC
					matchers.push_back(FactMatcher("gstreamer.received_bytes"));      // SLOT_BITRATE
					matchers.push_back(FactMatcher("video.latency.total_ms"));        // SLOT_LATENCY
					matchers.push_back(FactMatcher("video.displayed_frame"));         // SLOT_FPS_LIVE
					matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.rssi_avg"));  // SLOT_RSSI
					matchers.push_back(FactMatcher("wfbcli.rx.ant_stats.snr_avg"));   // SLOT_SNR
					matchers.push_back(FactMatcher("dvr.recording"));                 // SLOT_REC
```
(The `else if (matchers.size() != (size_t)AIOWidget::SLOT_COUNT)` warning below stays as-is — `SLOT_COUNT` is now 12.)

- [ ] **Step 8: Cross-build compile check**
Run the cross-build command. Expected: `PPBUILD_DONE rc=0`. Note: `aio::signal_bar_count` is now unused but still defined — that's fine; it's removed in Task 6. Fix any compile errors (most likely a missed reference to the old `SLOT_VIDEO_H` or the old `metric_tile(Metric,double)` signature) and rebuild until green.

- [ ] **Step 9: Commit**
```sh
git add src/osd.cpp
git commit -m "feat(osd): AIOWidget video-from-air-config, live FPS tile, RSSI bars, fixed layout"
```

---

## Task 6: Remove the now-unused `signal_bar_count` (host)

After Task 5 the bars use `rssi_to_bars` and LINK% is shown numerically via `link_quality_pct`, so `signal_bar_count` has no caller.

**Files:** `src/osd_aio_logic.hpp`, `src/osd_aio_logic.cpp`, `tests/test_aio_logic.cpp`

- [ ] **Step 1: Remove the test** — delete the entire `TEST_CASE("signal_bar_count", "[aio]") { ... }` block from `tests/test_aio_logic.cpp`.

- [ ] **Step 2: Remove the declaration** — in `src/osd_aio_logic.hpp`, delete:
```cpp
// Filled signal-bar count 0..5 from link quality %.
int signal_bar_count(int lq_pct);
```

- [ ] **Step 3: Remove the definition** — in `src/osd_aio_logic.cpp`, delete the whole `int signal_bar_count(int lq_pct) { ... }` function.

- [ ] **Step 4: Verify host tests still build + pass**
Run: `nix-shell --run 'cmake --build build-test --target aio_logic_tests -j 2>&1 | tail -5 && ./build-test/aio_logic_tests "[aio]"'`
Expected: PASS (the remaining `[aio]` cases, including the three new ones).

- [ ] **Step 5: Confirm the real target still compiles without it**
Run the cross-build command. Expected: `PPBUILD_DONE rc=0` (osd.cpp no longer references `signal_bar_count`).

- [ ] **Step 6: Commit**
```sh
git add src/osd_aio_logic.hpp src/osd_aio_logic.cpp tests/test_aio_logic.cpp
git commit -m "refactor(osd): drop unused signal_bar_count (bars now RSSI-driven)"
```

---

## Task 7: Deploy + on-device verification (manual)

**Files:** none (operational). Requires the live GS (`root@10.18.0.1`) and eyes on the goggle.

- [ ] **Step 1: Deploy the freshly cross-built binary** (swap via `.new` since the running exe can't be overwritten):
```sh
scp -o ConnectTimeout=10 -o ServerAliveInterval=3 \
  /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/build/pixelpilot-custom/pixelpilot \
  root@10.18.0.1:/usr/bin/pixelpilot.new
ssh -o ConnectTimeout=10 root@10.18.0.1 'cp -a /usr/bin/pixelpilot /usr/bin/pixelpilot.preaio2; chmod +x /usr/bin/pixelpilot.new; mv /usr/bin/pixelpilot.new /usr/bin/pixelpilot; kill $(pidof pixelpilot)'
sleep 3
ssh -o ConnectTimeout=10 root@10.18.0.1 'md5sum /proc/$(pidof pixelpilot)/exe /usr/bin/pixelpilot'
```
Expected: the two md5sums match (supervisor respawned the new binary). (No font/config change needed — the AIOWidget `osd.json` is already live from the prior deploy.)

- [ ] **Step 2: Verify VIDEO ← air config**
With the link up, confirm the VIDEO tile shows the configured mode (e.g. `1080p60`), and `--` before fpvd connects. Then in gsmenu change Camera → resolution and/or fps; after Apply, confirm the VIDEO tile updates to the new `<height>p<fps>` within a refresh.

- [ ] **Step 3: Verify the live FPS tile**
Confirm a new `FPS` tile appears between LATENCY and RSSI, tracks the live framerate, and colors by deviation from the configured fps (green when matching; amber/red as it drops; neutral white if the configured fps isn't known).

- [ ] **Step 4: Verify fixed layout**
Watch values change (LINK 92→100, RSSI swinging, BITRATE). Confirm no tile or the overall group shifts horizontally as digit widths change.

- [ ] **Step 5: Verify signal bars = RSSI**
Confirm the bars at the left of the right group track RSSI strength and recolor with the RSSI tile's band (degrade the link to watch them drop), not with LINK.

- [ ] **Step 6: Tuning pass (if needed)**
If `rssi_to_bars` endpoints (−90/−55) or `fps_band` ratios (0.90/0.70) look off on the goggle, adjust the constants in `src/osd_aio_logic.cpp`, update the affected `[aio]` tests, rebuild host tests, cross-build, redeploy, and commit.

- [ ] **Step 7: Revert (only if needed)**
```sh
ssh -o ConnectTimeout=10 root@10.18.0.1 'cp -a /usr/bin/pixelpilot.preaio2 /usr/bin/pixelpilot.rev && mv /usr/bin/pixelpilot.rev /usr/bin/pixelpilot; kill $(pidof pixelpilot)'
```

---

## Spec coverage check

- VIDEO ← air config (`format_video_mode`, `air.video.*` facts, bridge, `--` fallback) → Tasks 1, 4, 5. ✓
- Air-info bridge (snapshot listener → facts; public APIs; post-apply propagation) → Task 4. ✓
- Live FPS tile beside LATENCY, deviation-colored (`fps_band`, `SLOT_FPS_LIVE`) → Tasks 3, 5. ✓
- Fixed/no-jitter layout (per-tile `reserve`) → Task 5 (steps 4–6). ✓
- Signal bars → RSSI (`rssi_to_bars`, RSSI band) → Tasks 2, 5. ✓
- Slot/setFact/matcher consistency (12 entries, 1:1) → Task 5 (steps 1, 2, 7). ✓
- Remove `signal_bar_count` → Task 6. ✓
- Keep `fps` RunningAverage (repurposed for live fps) → Task 5 (step 2, member unchanged). ✓
- On-device verification of all four behaviors → Task 7. ✓
