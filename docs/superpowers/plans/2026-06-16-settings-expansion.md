# gsmenu Settings Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Camera Resilience dropdown (greying out GOP size when active) and an OSD Enabled toggle, and refactor the Dynamic Link page down to Enabled + a new Compute section + Failsafe.

**Architecture:** New `(domain,page,key)`→JSON-path entries in the `settings_fpvd.c` keymap, matching sim defaults in `settings_dummy.c`, and page wiring in `pages/camera.c` and `pages/dynamiclink.c`. Conditional UI uses the established snapshot-listener pattern (lock-state reapply, then a page-specific gate/visibility pass).

**Tech Stack:** C, LVGL, cJSON, Catch2 (host sim build via `nix-shell` + `build-test`).

---

## File Structure

- `src/gsmenu/settings_fpvd.c` — keymap table: +7 entries, −12 entries.
- `src/gsmenu/settings_dummy.c` — sim defaults: +7 entries, −12 entries.
- `src/gsmenu/pages/camera.c` — resilience dropdown + GOP gate + OSD toggle.
- `src/gsmenu/pages/dynamiclink.c` — builder rewrite (strip + Compute section).
- `tests/test_settings_fpvd.cpp` — keymap + lock assertions.

## Build / test reference

All commands run inside the project dev shell (lvgl needs SDL2/libpng from `shell.nix`). `build-test/` is already configured.

- Build + run keymap/lock tests: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap],[lock]"'`
  (Catch2 ORs comma-separated tags; space-separated quoted tags AND and match nothing.)
- Compile-check the UI (GUI target, can't run headless): `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
- Full suite: `nix-shell --run './build-test/fpvd_tests'`

---

## Task 1: Keymap entries + tests

**Files:**
- Test: `tests/test_settings_fpvd.cpp`
- Modify: `src/gsmenu/settings_fpvd.c`

- [ ] **Step 1: Write the failing tests**

Add these two new `TEST_CASE`s after the existing `"keymap: lookup returns null for unknown triples"` case (around line 39) in `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("keymap: camera resilience/osd + dlink compute/maxMcs", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "resilience");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.resilience") == 0);
    REQUIRE(e->type == FPVD_T_ENUM);

    e = fpvd_keymap_lookup("air", "camera", "osd_enabled");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "osd.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);

    e = fpvd_keymap_lookup("air", "dlink", "compute_base_redundancy");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.baseRedundancyRatio") == 0);
    REQUIRE(e->type == FPVD_T_FLOAT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_blocks_per_frame");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.blocksPerFrame") == 0);
    REQUIRE(e->type == FPVD_T_FLOAT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_min_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.minBitrateKbps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_max_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.maxBitrateKbps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("gs", "dlink", "max_mcs");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.maxMcs") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_GS);
}

TEST_CASE("keymap: removed dynamic-link rows no longer resolve", "[fpvd][keymap]") {
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "interleaving") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "mavlink_enable") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "osd_enabled") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "osd_debug_latency") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "health_timeout_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "min_idr_interval_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "apply_stagger_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "apply_subpace_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_threshold_kbps") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_low_anchor_kbps") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_floor") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_step") == nullptr);
}
```

Also extend the existing `"lock: does not match unrelated"` case (around line 220) by adding these four lines inside it:

```cpp
    REQUIRE(fpvd_is_locked_path("video.resilience") == false);
    REQUIRE(fpvd_is_locked_path("osd.enabled") == false);
    REQUIRE(fpvd_is_locked_path("dynamicLink.compute") == false);
    REQUIRE(fpvd_is_locked_path("dynamicLink.maxMcs") == false);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap]"'`
Expected: FAIL — the new `"keymap: camera resilience/osd + dlink compute/maxMcs"` case fails at `REQUIRE(e != nullptr)` for `resilience` (not in the keymap yet). The `"removed dynamic-link rows..."` case currently FAILS too (those keys still resolve). The `[lock]` additions already pass.

- [ ] **Step 3: Add the camera keymap entries**

In `src/gsmenu/settings_fpvd.c`, find the camera `gopsize` line:
```c
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT,          FPVD_EP_AIR, FPVD_ROW_PLAIN },
```
and insert the resilience entry immediately after it:
```c
    { "air", "camera", "resilience", "video.resilience",  FPVD_T_ENUM,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
```
Then find the camera `qp_delta` line:
```c
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT,            FPVD_EP_AIR, FPVD_ROW_PLAIN },
```
and insert the osd_enabled entry immediately after it:
```c
    { "air", "camera", "osd_enabled","osd.enabled",       FPVD_T_BOOL,           FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

- [ ] **Step 4: Replace the dead Dynamic Link keymap entries with the new ones**

In `src/gsmenu/settings_fpvd.c`, replace this exact block (the 12 entries between `enabled` and `safe_mcs`):
```c
    { "air", "dlink", "interleaving",         "dynamicLink.interleavingSupported",FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "mavlink_enable",       "dynamicLink.mavlinkEnable",        FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "osd_enabled",          "dynamicLink.osd.enabled",          FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "osd_debug_latency",    "dynamicLink.osd.debugLatency",     FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "health_timeout_ms",    "dynamicLink.healthTimeoutMs",      FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "min_idr_interval_ms",  "dynamicLink.minIdrIntervalMs",     FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "apply_stagger_ms",     "dynamicLink.applyStaggerMs",       FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "apply_subpace_ms",     "dynamicLink.applySubPaceMs",       FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "roiqp_threshold_kbps", "dynamicLink.roiQp.thresholdKbps",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "roiqp_low_anchor_kbps","dynamicLink.roiQp.lowAnchorKbps",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "roiqp_floor",          "dynamicLink.roiQp.floor",          FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "roiqp_step",           "dynamicLink.roiQp.step",           FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
```
with:
```c
    { "air", "dlink", "compute_base_redundancy",  "dynamicLink.compute.baseRedundancyRatio", FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_blocks_per_frame", "dynamicLink.compute.blocksPerFrame",      FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_min_bitrate_kbps", "dynamicLink.compute.minBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "compute_max_bitrate_kbps", "dynamicLink.compute.maxBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "gs",  "dlink", "max_mcs",                  "dynamicLink.maxMcs",                      FPVD_T_INT,   FPVD_EP_GS,  FPVD_ROW_PLAIN },
```
Leave the `enabled` entry above and the `safe_*` entries below untouched. Do NOT modify `LOCKED_PATHS`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap],[lock]"'`
Expected: PASS — all keymap and lock assertions green.

- [ ] **Step 6: Commit**

```bash
git add tests/test_settings_fpvd.cpp src/gsmenu/settings_fpvd.c
git commit -m "feat(gsmenu): keymap for resilience/osd + dlink compute/maxMcs; drop dead dlink keys"
```

---

## Task 2: Sim provider defaults

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`

No unit test of its own (plain data used by the GUI sim). Verification is a clean compile plus the Task 1 suite still passing.

- [ ] **Step 1: Add the Camera resilience + OSD defaults**

In `src/gsmenu/settings_dummy.c`, find the Camera recording block ending:
```c
    { "rec_maxmb",    "500" },
```
and insert immediately after it:
```c

    /* Camera — Resilience + OSD */
    { "resilience",   "off" },
    { "osd_enabled",  "off" },
```

- [ ] **Step 2: Replace the dead Dynamic Link defaults with the new ones**

In the same file, replace this exact block:
```c
    /* Dynamic Link — General */
    { "enabled",            "off" },
    { "interleaving",       "on" },
    { "mavlink_enable",     "on" },
    /* Dynamic Link — OSD */
    { "osd_enabled",        "on" },
    { "osd_debug_latency",  "off" },
    /* Dynamic Link — Timing */
    { "health_timeout_ms",  "10000" },
    { "min_idr_interval_ms","500" },
    { "apply_stagger_ms",   "50" },
    { "apply_subpace_ms",   "5" },
    /* Dynamic Link — ROI QP */
    { "roiqp_threshold_kbps", "6000" },
    { "roiqp_low_anchor_kbps","2000" },
    { "roiqp_floor",          "-24" },
    { "roiqp_step",           "3" },
```
with:
```c
    /* Dynamic Link — General */
    { "enabled",            "off" },
    /* Dynamic Link — Compute */
    { "compute_base_redundancy",  "0.5" },
    { "compute_blocks_per_frame", "2.0" },
    { "compute_min_bitrate_kbps", "1000" },
    { "compute_max_bitrate_kbps", "24000" },
    { "max_mcs",                  "5" },
```
Leave the `/* Dynamic Link — Safe Ceilings */` block (`safe_*`) below it untouched. Do NOT add anything to `g_dummy_locked_keys[]`.

- [ ] **Step 3: Compile-check + tests still pass**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap],[lock]"'`
Expected: both targets build clean; keymap/lock tests still PASS.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/settings_dummy.c
git commit -m "feat(gsmenu): sim defaults for resilience/osd + dlink compute; drop dead dlink defaults"
```

---

## Task 3: Camera page — resilience dropdown, GOP gate, OSD toggle

**Files:**
- Modify: `src/gsmenu/pages/camera.c`

GUI-only; verification is a clean `pixelpilot` compile and manual sim check.

- [ ] **Step 1: Add includes**

In `src/gsmenu/pages/camera.c`, after the existing `#include "../settings.h"` line, add:
```c
#include <string.h>
#include <stdlib.h>
```

- [ ] **Step 2: Add the gate helper and combined listener before `build_camera_tab`**

Insert this block immediately before `lv_obj_t *build_camera_tab(lv_obj_t *parent) {`:
```c
extern void pp_page_reapply_lock_state(lv_obj_t *);

/* The drone ignores video.gopSize when video.resilience != "off"; grey out the
 * GOP size row (plain disabled + dimmed, no lock icon) to signal it's inactive.
 * The GOP row is tagged LV_OBJ_FLAG_USER_1 by the builder. Runs after the lock
 * pass so the lock pass cannot overwrite the disable. */
static void apply_resilience_gate(lv_obj_t *page) {
    char *v = pp_settings_get("air", "camera", "resilience");
    bool gated = v && strcmp(v, "off") != 0;
    free(v);
    if (!gated) return;
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            lv_obj_add_state(c, LV_STATE_DISABLED);
            lv_obj_set_style_opa(c, LV_OPA_60, 0);
        }
    }
    pp_page_rescue_focus(page);
}

static void snapshot_listener_cb(void *user_data) {
    lv_obj_t *page = (lv_obj_t *)user_data;
    pp_page_reapply_lock_state(page);
    apply_resilience_gate(page);
}
```

- [ ] **Step 3: Capture/tag the GOP row and add the Resilience dropdown**

Replace this block:
```c
    pp_slider_ex(page, LV_SYMBOL_SETTINGS, "GOP size",
                 "air", "camera", "gopsize", &gop_cfg);
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "RC Mode",
                "air", "camera", "rc_mode",
                "cbr\nvbr");
```
with:
```c
    lv_obj_t *gop = pp_slider_ex(page, LV_SYMBOL_SETTINGS, "GOP size",
                                 "air", "camera", "gopsize", &gop_cfg);
    lv_obj_add_flag(gop, LV_OBJ_FLAG_USER_1);   /* greyed out when resilience != off */
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "Resilience",
                "air", "camera", "resilience",
                "off\nrescue\nquality\nsprint\nracing\nendurance\npatrol\nrally\nrange\nfpv");
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "RC Mode",
                "air", "camera", "rc_mode",
                "cbr\nvbr");
```

- [ ] **Step 4: Add the OSD section after the Video section**

Replace this block:
```c
    pp_slider(page, LV_SYMBOL_SETTINGS, "QP Delta",
              "air", "camera", "qp_delta", -32, 0);

    pp_section_header(page, "ROI");
```
with:
```c
    pp_slider(page, LV_SYMBOL_SETTINGS, "QP Delta",
              "air", "camera", "qp_delta", -32, 0);

    pp_section_header(page, "OSD");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "OSD Enabled",
              "air", "camera", "osd_enabled");

    pp_section_header(page, "ROI");
```

- [ ] **Step 5: Wire the combined listener and initial gate**

Replace this block at the end of `build_camera_tab`:
```c
    extern void pp_page_reapply_lock_state(lv_obj_t *);
    /* The dispatcher supports multiple listeners via fanout, so this
     * registration coexists with the Dynamic Link tab's own listener. */
    pp_settings_set_snapshot_listener(
        (pp_settings_snapshot_cb)pp_page_reapply_lock_state, page);
    pp_page_reapply_lock_state(page);
    return page;
```
with:
```c
    /* The dispatcher supports multiple listeners via fanout, so this
     * registration coexists with the Dynamic Link tab's own listener. */
    pp_page_reapply_lock_state(page);
    apply_resilience_gate(page);
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
```
(The in-function `extern void pp_page_reapply_lock_state(lv_obj_t *);` is removed — it moved to file scope in Step 2.)

- [ ] **Step 6: Compile-check**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean, "Built target pixelpilot". If it reports a duplicate `extern` declaration, confirm Step 5 removed the in-function line.

- [ ] **Step 7: Manual sim verification**

Run: `./sim.sh`
Expected: On the Camera page, a **Resilience** dropdown appears directly below GOP size. With `off` selected (sim default), GOP size is editable. Selecting any other preset greys out (dims, non-focusable) the GOP size row immediately; returning to `off` re-enables it. A new **OSD** section with an **OSD Enabled** toggle appears between Video and ROI.

- [ ] **Step 8: Commit**

```bash
git add src/gsmenu/pages/camera.c
git commit -m "feat(gsmenu): camera resilience dropdown (gates GOP) + OSD enabled toggle"
```

---

## Task 4: Dynamic Link page — strip to Enabled + Compute + Failsafe

**Files:**
- Modify: `src/gsmenu/pages/dynamiclink.c`

GUI-only; verification is a clean `pixelpilot` compile and manual sim check. The file's `apply_visibility`, `snapshot_listener_cb`, and includes are unchanged — only the `build_dynamiclink_tab` function body is rewritten.

- [ ] **Step 1: Replace the builder function**

Replace the entire existing `build_dynamiclink_tab` function (from `lv_obj_t *build_dynamiclink_tab(lv_obj_t *parent) {` through its closing `}`) with:
```c
lv_obj_t *build_dynamiclink_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "dlink");

    pp_section_header(page, "General");
    lv_obj_t *enabled = pp_toggle(page, LV_SYMBOL_POWER, "Enabled",
                                  "air", "dlink", "enabled");
    lv_obj_add_flag(enabled, LV_OBJ_FLAG_USER_3);   /* visibility anchor */

    pp_section_header(page, "Compute");
    static const pp_slider_cfg_t redundancy_cfg = {
        .raw_min = 1, .raw_max = 20, .step = 1, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 10, .decimals = 1,
        .unit = NULL, .serialize = PP_SER_FLOAT_DIV,
    };
    pp_slider_ex(page, LV_SYMBOL_SETTINGS, "Base Redundancy Ratio",
                 "air", "dlink", "compute_base_redundancy", &redundancy_cfg);
    static const pp_slider_cfg_t blocks_cfg = {
        .raw_min = 1, .raw_max = 16, .step = 1, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 2, .decimals = 1,
        .unit = NULL, .serialize = PP_SER_FLOAT_DIV,
    };
    pp_slider_ex(page, LV_SYMBOL_SETTINGS, "Blocks / Frame",
                 "air", "dlink", "compute_blocks_per_frame", &blocks_cfg);
    /* Mirrors the camera Bitrate slider: displays Mbps, stores raw kbps int. */
    static const pp_slider_cfg_t bitrate_cfg = {
        .raw_min = 500, .raw_max = 26000, .step = 500, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 1000, .decimals = 1,
        .unit = "Mbps", .serialize = PP_SER_INT,
    };
    lv_obj_t *min_br = pp_slider_ex(page, LV_SYMBOL_AUDIO, "Min Bitrate",
                                    "air", "dlink", "compute_min_bitrate_kbps", &bitrate_cfg);
    lv_obj_t *max_br = pp_slider_ex(page, LV_SYMBOL_AUDIO, "Max Bitrate",
                                    "air", "dlink", "compute_max_bitrate_kbps", &bitrate_cfg);
    /* Enforce min <= max - 500 kbps from both sides. */
    pp_slider_set_relation(min_br, "air", "dlink", "compute_max_bitrate_kbps", -500, /*is_max*/ true);
    pp_slider_set_relation(max_br, "air", "dlink", "compute_min_bitrate_kbps",  500, /*is_max*/ false);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Max MCS",
              "gs", "dlink", "max_mcs", 0, 7);

    pp_section_header(page, "Failsafe");
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS",
              "air", "dlink", "safe_mcs", 0, 7);
    lv_obj_t *safe_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC K",
                                 "air", "dlink", "safe_k", 1, 31);
    lv_obj_t *safe_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC N",
                                 "air", "dlink", "safe_n", 2, 32);
    pp_slider_set_relation(safe_k, "air", "dlink", "safe_n", -2, /*is_max*/ true);
    pp_slider_set_relation(safe_n, "air", "dlink", "safe_k",  2, /*is_max*/ false);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Block Depth",
              "air", "dlink", "safe_depth", 1, 8);
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "air", "dlink", "safe_bandwidth", "10\n20\n40");
    pp_slider(page, LV_SYMBOL_UP, "TX Power (dBm)",
              "air", "dlink", "safe_txpower_dbm", -10, 30);
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate (kbps)",
              "air", "dlink", "safe_bitrate_kbps", 500, 30000);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }

    apply_visibility(page);
    pp_page_reapply_lock_state(page);
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
}
```

- [ ] **Step 2: Compile-check**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean. (If the compiler warns that `pp_page_reapply_lock_state` is implicitly declared, note it is already `extern`-declared at file scope in `dynamiclink.c` — unchanged by this task.)

- [ ] **Step 3: Manual sim verification**

Run: `./sim.sh`
Expected: The Dynamic Link page now shows only **General** (Enabled), **Compute** (Base Redundancy Ratio, Blocks / Frame, Min Bitrate, Max Bitrate, Max MCS), and **Failsafe** (unchanged). Interleaving, MAVLink, OSD, Timing, and ROI QP rows are gone. Toggling **Enabled** off hides the Compute and Failsafe rows (existing visibility behavior); on shows them. Min/Max Bitrate display in Mbps.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/pages/dynamiclink.c
git commit -m "feat(gsmenu): refactor Dynamic Link page to Enabled + Compute + Failsafe"
```

---

## Task 5: Full suite + final compile

- [ ] **Step 1: Run the complete fpvd test suite**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests'`
Expected: all test cases pass (no regressions in `[config]`/`[network]`/`[endpoint]`/`[caps]`).

- [ ] **Step 2: Final compile-check of the sim**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean.

---

## Notes for the implementer

- `pp_settings_get(domain, page, key)` returns a heap `char*` the caller must `free()` (may be `NULL`); the gate frees it before reading the boolean.
- `FPVD_T_ENUM` stores the dropdown's selected option text verbatim — the resilience options string must be exactly `"off\nrescue\nquality\nsprint\nracing\nendurance\npatrol\nrally\nrange\nfpv"`.
- `pp_slider_ex` with `PP_SER_FLOAT_DIV` serializes `raw/disp_div` as a float; with `PP_SER_INT` it serializes the raw integer. So Base Redundancy (`disp_div=10`) maps stored `0.5`↔raw `5`; Blocks/Frame (`disp_div=2`) maps `2.0`↔raw `4`; Min/Max Bitrate (`disp_div=1000`, `PP_SER_INT`) store raw kbps and display Mbps.
- `max_mcs` is the only GS-endpoint row added here (`"gs","dlink","max_mcs"` → `FPVD_EP_GS`); the four `compute_*` rows are drone-side (`"air","dlink",...`).
- Do not touch `LOCKED_PATHS` or `g_dummy_locked_keys[]` — none of the new paths are adaptive-link-locked.
- The Dynamic Link page keeps its existing `apply_visibility` (hides everything past the `LV_OBJ_FLAG_USER_3` Enabled anchor when disabled) and `snapshot_listener_cb` — only the builder body changes.
