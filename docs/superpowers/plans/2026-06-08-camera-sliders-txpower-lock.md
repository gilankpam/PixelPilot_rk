# Camera sliders + TX-power lock — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make camera GOP size a fractional/variable-step slider (0.0–1.0 by 0.1, then 1.0 up to 30), change camera bitrate from a dropdown to a 0.5–26 Mbps slider, and make TX Power editable while dynamic link is on.

**Architecture:** Extract a pure scale/step helper (`pp_slider_scale`) with Catch2 tests, then drive `pp_slider` from a config struct (`pp_slider_cfg_t`) via a new `pp_slider_ex()`; keep `pp_slider(min,max)` as a thin wrapper so existing call sites are untouched. Backend: switch the camera bitrate keymap to raw kbps and drop `link.txpower` from the lock list.

**Tech Stack:** C, LVGL v9.5, Catch2 (host sim build), CMake, Nix.

**Spec:** `docs/superpowers/specs/2026-06-08-camera-sliders-txpower-lock-design.md`

---

## Conventions

**Sim/test build + run:**
```bash
nix-shell shell-sim.nix --run "cmake -DUSE_SIMULATOR=ON -S . -B build-sim && cmake --build build-sim -j" 2>&1 | tail -15
```
**Run the scale unit tests:** `nix-shell shell-sim.nix --run "./build-sim/settings_tests '[slider]'"` → expect `All tests passed`.
**Screenshot a menu state** (harness from earlier work; `d`=enter/activate, `s`=down): `nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/x.png PP_SIM_KEYS='...' ./build-sim/pixelpilot"`.
**Commit** end-of-body line: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/gsmenu/widgets/pp_slider_scale.{c,h}` | **new** pure step/format/parse/serialize logic | 1 |
| `tests/test_slider_scale.cpp` | **new** Catch2 tests for the above | 1 |
| `CMakeLists.txt` | add scale `.c` + test to `settings_tests`; add `.c` to sim+device source lists | 1,2 |
| `src/gsmenu/widgets/pp_slider.{c,h}` | cfg-driven core `pp_slider_ex` + `pp_slider` wrapper + unit label | 2 |
| `src/gsmenu/pages/camera.c` | GOP → `pp_slider_ex`; bitrate dropdown → `pp_slider_ex` | 3,4 |
| `src/gsmenu/settings_fpvd.c` | bitrate keymap → `FPVD_T_INT`; drop `link.txpower` from `LOCKED_PATHS` | 4,5 |
| `src/gsmenu/settings_dummy.c` | bitrate seed → kbps | 4 |

---

## Task 1: pure scale logic + tests (TDD)

**Files:** Create `src/gsmenu/widgets/pp_slider_scale.h`, `pp_slider_scale.c`, `tests/test_slider_scale.cpp`; Modify `CMakeLists.txt`.

- [ ] **Step 1: Create the header `src/gsmenu/widgets/pp_slider_scale.h`**

```c
#ifndef PP_SLIDER_SCALE_H
#define PP_SLIDER_SCALE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PP_SER_INT = 0, PP_SER_FLOAT_DIV = 1 } pp_slider_ser_t;

/* Slider value model: an integer "raw" value. Display value = raw/disp_div
 * with `decimals` places (trailing zeros trimmed). Stepping is `step`, or
 * `fine_step` when raw is below `fine_threshold` (fine_step=0 => uniform). */
typedef struct {
    int32_t raw_min, raw_max;
    int32_t step;
    int32_t fine_step;        /* 0 => uniform stepping */
    int32_t fine_threshold;
    int32_t disp_div;         /* >=1; display = raw/disp_div */
    int8_t  decimals;
    const char *unit;         /* NULL/"" => none (used by the widget, not here) */
    pp_slider_ser_t serialize;
} pp_slider_cfg_t;

/* Step magnitude for one press: dir>0 up, dir<0 down. */
int32_t pp_slider_step(int32_t raw, const pp_slider_cfg_t *cfg, int dir);
/* Numeric display of raw (no unit), trailing zeros + bare '.' trimmed. */
void    pp_slider_fmt(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n);
/* Parse a backend string to a clamped raw value. */
int32_t pp_slider_parse(const char *s, const pp_slider_cfg_t *cfg);
/* Wire string: PP_SER_INT => "%d" of raw; PP_SER_FLOAT_DIV => pp_slider_fmt. */
void    pp_slider_ser(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the failing tests `tests/test_slider_scale.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <string>
extern "C" {
#include "gsmenu/widgets/pp_slider_scale.h"
}

static const pp_slider_cfg_t GOP = {0, 300, 10, 1, 10, 10, 1, nullptr, PP_SER_FLOAT_DIV};
static const pp_slider_cfg_t BR  = {500, 26000, 500, 0, 0, 1000, 1, "Mbps", PP_SER_INT};

static std::string fmt(int32_t raw, const pp_slider_cfg_t& c) {
    char b[32]; pp_slider_fmt(raw, &c, b, sizeof b); return b;
}
static std::string ser(int32_t raw, const pp_slider_cfg_t& c) {
    char b[32]; pp_slider_ser(raw, &c, b, sizeof b); return b;
}

TEST_CASE("GOP fractional display", "[slider]") {
    REQUIRE(fmt(0,  GOP) == "0");
    REQUIRE(fmt(5,  GOP) == "0.5");
    REQUIRE(fmt(10, GOP) == "1");
    REQUIRE(fmt(20, GOP) == "2");
    REQUIRE(fmt(300,GOP) == "30");
}
TEST_CASE("GOP variable step is symmetric across 1.0", "[slider]") {
    REQUIRE(pp_slider_step(9,  &GOP, +1) == 1);   /* 0.9 -> 1.0 */
    REQUIRE(pp_slider_step(10, &GOP, +1) == 10);  /* 1.0 -> 2.0 */
    REQUIRE(pp_slider_step(20, &GOP, -1) == 10);  /* 2.0 -> 1.0 */
    REQUIRE(pp_slider_step(10, &GOP, -1) == 1);   /* 1.0 -> 0.9 */
}
TEST_CASE("GOP parse + serialize", "[slider]") {
    REQUIRE(pp_slider_parse("0.5", &GOP) == 5);
    REQUIRE(pp_slider_parse("2",   &GOP) == 20);
    REQUIRE(pp_slider_parse("",    &GOP) == 0);   /* empty -> raw_min */
    REQUIRE(ser(5,  GOP) == "0.5");
    REQUIRE(ser(20, GOP) == "2");
}
TEST_CASE("bitrate kbps<->Mbps display, raw wire, clamp", "[slider]") {
    REQUIRE(fmt(12500, BR) == "12.5");
    REQUIRE(fmt(26000, BR) == "26");
    REQUIRE(pp_slider_parse("12500", BR) == 12500);
    REQUIRE(pp_slider_parse("99999", BR) == 26000); /* clamp to max */
    REQUIRE(pp_slider_parse("100",   BR) == 500);   /* clamp to min */
    REQUIRE(ser(12500, BR) == "12500");
    REQUIRE(pp_slider_step(12500, &BR, +1) == 500); /* uniform */
}
```

- [ ] **Step 3: Add the scale `.c` + test to the `settings_tests` target in `CMakeLists.txt`**

In the `add_executable(settings_tests …)` list, add `src/gsmenu/widgets/pp_slider_scale.c` next to `pp_slider_bounds.c`, and `tests/test_slider_scale.cpp` next to `tests/test_slider_bounds.cpp`.

- [ ] **Step 4: Run the tests to verify they FAIL (link error / no impl)**

Build (Conventions). Expected: `settings_tests` fails to build (undefined `pp_slider_*`) — confirms the test exercises real code.

- [ ] **Step 5: Implement `src/gsmenu/widgets/pp_slider_scale.c`**

```c
#include "pp_slider_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int32_t pp_slider_step(int32_t raw, const pp_slider_cfg_t *cfg, int dir) {
    if (cfg->fine_step <= 0) return cfg->step;
    if (dir > 0) return (raw >= cfg->fine_threshold) ? cfg->step : cfg->fine_step;
    return (raw > cfg->fine_threshold) ? cfg->step : cfg->fine_step;
}

void pp_slider_fmt(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n) {
    int32_t div = cfg->disp_div > 0 ? cfg->disp_div : 1;
    if (div == 1 && cfg->decimals == 0) { snprintf(buf, n, "%d", (int)raw); return; }
    snprintf(buf, n, "%.*f", cfg->decimals, (double)raw / (double)div);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';            /* drop a bare trailing '.' */
    }
}

int32_t pp_slider_parse(const char *s, const pp_slider_cfg_t *cfg) {
    if (!s || !*s) return cfg->raw_min;
    int32_t raw;
    if (cfg->serialize == PP_SER_FLOAT_DIV) {
        int32_t div = cfg->disp_div > 0 ? cfg->disp_div : 1;
        raw = (int32_t)lround(atof(s) * (double)div);
    } else {
        raw = (int32_t)strtol(s, NULL, 10);
    }
    return clampi(raw, cfg->raw_min, cfg->raw_max);
}

void pp_slider_ser(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n) {
    if (cfg->serialize == PP_SER_FLOAT_DIV) pp_slider_fmt(raw, cfg, buf, n);
    else snprintf(buf, n, "%d", (int)raw);
}
```

- [ ] **Step 6: Run the tests to verify they PASS**

`nix-shell shell-sim.nix --run "cmake --build build-sim -j --target settings_tests && ./build-sim/settings_tests '[slider]'"` → expect `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/widgets/pp_slider_scale.c src/gsmenu/widgets/pp_slider_scale.h tests/test_slider_scale.cpp CMakeLists.txt
git commit -m "feat(gsmenu): pp_slider_scale — fractional/variable-step value logic + tests"
```

---

## Task 2: drive `pp_slider` from a config (`pp_slider_ex` + wrapper)

**Files:** Modify `src/gsmenu/widgets/pp_slider.h`, `src/gsmenu/widgets/pp_slider.c`, `CMakeLists.txt`.

- [ ] **Step 1: Declare `pp_slider_ex` in `pp_slider.h`**

Add `#include "pp_slider_scale.h"` near the top, and after the existing `pp_slider(...)` declaration add:
```c
/* Config-driven slider (fractional/scaled/variable-step). `pp_slider` is a
 * thin wrapper over this with a plain-integer default config. */
lv_obj_t *pp_slider_ex(lv_obj_t *parent_page,
                       const char *icon_text, const char *label,
                       const char *domain, const char *page, const char *key,
                       const pp_slider_cfg_t *cfg);
```

- [ ] **Step 2: Rework `pp_slider.c` to be cfg-driven**

Add `#include "pp_slider_scale.h"` and `#include "pp_slider.h"` (already present). In `struct pp_slider_data`, replace the `int32_t min, max;` fields with a config and a unit label; keep `value`/`saved_val` as the raw value:
```c
    pp_slider_cfg_t cfg;     /* replaces min/max */
    lv_obj_t *unit_lbl;      /* small dim unit label, NULL if cfg.unit empty */
```
Update `effective_max`/`effective_min` to use the cfg bounds (relation still folds in):
```c
static int32_t effective_max(pp_slider_data_t *d) {
    if (d->rel_key && d->rel_is_max)
        return pp_slider_bound_max(d->cfg.raw_max, d->rel_domain, d->rel_page, d->rel_key, d->rel_offset);
    return d->cfg.raw_max;
}
static int32_t effective_min(pp_slider_data_t *d) {
    if (d->rel_key && !d->rel_is_max)
        return pp_slider_bound_min(d->cfg.raw_min, d->rel_domain, d->rel_page, d->rel_key, d->rel_offset);
    return d->cfg.raw_min;
}
```
Replace `refresh_num` to format via the scale helper:
```c
static void refresh_num(pp_slider_data_t *d) {
    char buf[32];
    pp_slider_fmt(d->value, &d->cfg, buf, sizeof buf);
    lv_label_set_text(d->num, buf);
}
```
Delete the `step_for(...)` helper (replaced by `pp_slider_step` / `cfg.step`). In `on_key`, compute the per-press step from the scale helper and adjust raw. Replace the whole `LV_KEY_UP` and `LV_KEY_DOWN` branches with:
```c
    } else if (k == LV_KEY_UP) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint32_t now = lv_tick_get();
            d->hold_count = pp_slider_accel_update(now, d->last_key_ms, d->last_key, k, d->hold_count);
            d->last_key = k; d->last_key_ms = now;
            int32_t step = pp_slider_step(d->value, &d->cfg, +1);
            int32_t scaled = pp_slider_accel_step(step, d->hold_count);
            int32_t emax = effective_max(d);
            d->value += scaled; if (d->value > emax) d->value = emax;
            refresh_num(d); consumed = true;
        }
    } else if (k == LV_KEY_DOWN) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint32_t now = lv_tick_get();
            d->hold_count = pp_slider_accel_update(now, d->last_key_ms, d->last_key, k, d->hold_count);
            d->last_key = k; d->last_key_ms = now;
            int32_t step = pp_slider_step(d->value, &d->cfg, -1);
            int32_t scaled = pp_slider_accel_step(step, d->hold_count);
            int32_t emin = effective_min(d);
            d->value -= scaled; if (d->value < emin) d->value = emin;
            refresh_num(d); consumed = true;
        }
    }
```
In the ENTER-apply branch, serialize raw via the scale helper instead of `snprintf("%d", value)`:
```c
                char buf[32];
                pp_slider_ser(d->value, &d->cfg, buf, sizeof buf);
                int32_t attempted = d->value;
                d->value = d->saved_val;
                refresh_num(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct slider_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d = d; ctx->target_val = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf, slider_done_cb, ctx);
                consumed = true;
```

- [ ] **Step 3: Replace the constructor — `pp_slider_ex` (full builder) + `pp_slider` wrapper**

Replace the existing `pp_slider(...)` definition with `pp_slider_ex(...)` containing the same row-building body, but: store `d->cfg = *cfg; d->value = cfg->raw_min;`; create the unit label when `cfg->unit && *cfg->unit`; init via `pp_slider_parse`. Concretely, the builder body changes are:

After creating the `num` label (and its ExtraBold font line), add the unit label:
```c
    lv_obj_t *unit = NULL;
    if (cfg->unit && *cfg->unit) {
        unit = lv_label_create(col);
        lv_label_set_text(unit, cfg->unit);
        lv_obj_set_style_text_font(unit, pp_font_med_sm(), 0);
        lv_obj_set_style_text_color(unit, lv_color_hex(PP_C_INK), 0);
        lv_obj_set_style_text_opa(unit, 102, 0);            /* ~40% dim unit */
        lv_obj_set_style_pad_left(unit, PP_SCALE(4), 0);
    }
```
Set the data fields:
```c
    pp_slider_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->cfg    = *cfg;
    d->value  = cfg->raw_min;
    d->num    = num;
    d->unit_lbl = unit;
    d->up_chev = up; d->down_chev = dn;
```
Init read:
```c
    char *v = pp_settings_get(domain, page, key);
    if (v && *v) { d->value = pp_slider_parse(v, &d->cfg); refresh_num(d); }
    free(v);
```
Then add the wrapper that preserves today's behavior:
```c
lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max) {
    int32_t range = max - min; if (range < 0) range = -range;
    int32_t step = range / 20; if (step < 1) step = 1;
    pp_slider_cfg_t cfg = {
        .raw_min = min, .raw_max = max, .step = step,
        .fine_step = 0, .fine_threshold = 0,
        .disp_div = 1, .decimals = 0, .unit = NULL, .serialize = PP_SER_INT,
    };
    return pp_slider_ex(parent_page, icon_text, label, domain, page, key, &cfg);
}
```
> Note: `pp_slider.c` must `#include "../styles.h"` (it already does) for `pp_font_med_sm`/`PP_C_INK`/`PP_SCALE`.

- [ ] **Step 4: Add `pp_slider_scale.c` to the sim + device source lists in `CMakeLists.txt`**

Add `src/gsmenu/widgets/pp_slider_scale.h` + `.c` after `pp_slider.c` in BOTH `SIMULATOR_SOURCES` and the device `LIB_SOURCE_FILES` (mirror how `pp_slider_bounds.c`/`pp_slider_accel.c` appear in both). Confirm: `grep -c "pp_slider_scale.c" CMakeLists.txt` → `3` (settings_tests + 2 lists).

- [ ] **Step 5: Build + verify existing sliders unchanged**

Build (Conventions; expect `Built target pixelpilot` + `settings_tests` green). Screenshot Link tab (which has plain `pp_slider` TX Power/RX Power/FEC):
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t2.png PP_SIM_KEYS='sd' ./build-sim/pixelpilot"
```
Read `/tmp/t2.png`: TX Power / RX Power / FEC_K / FEC_N still show plain integer values exactly as before (wrapper preserves behavior).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/widgets/pp_slider.c src/gsmenu/widgets/pp_slider.h CMakeLists.txt
git commit -m "feat(gsmenu): config-driven pp_slider_ex; pp_slider becomes a wrapper"
```

---

## Task 3: GOP size → fractional `pp_slider_ex`

**Files:** Modify `src/gsmenu/pages/camera.c`.

- [ ] **Step 1: Replace the GOP slider with a fractional `pp_slider_ex`**

Add `#include "../widgets/pp_slider_scale.h"` near the other includes. Replace:
```c
    pp_slider(page, LV_SYMBOL_SETTINGS, "GOP size",
              "air", "camera", "gopsize", 1, 20);
```
with:
```c
    static const pp_slider_cfg_t gop_cfg = {
        .raw_min = 0, .raw_max = 300, .step = 10,
        .fine_step = 1, .fine_threshold = 10,
        .disp_div = 10, .decimals = 1, .unit = NULL, .serialize = PP_SER_FLOAT_DIV,
    };
    pp_slider_ex(page, LV_SYMBOL_SETTINGS, "GOP size",
                 "air", "camera", "gopsize", &gop_cfg);
```

- [ ] **Step 2: Build + verify GOP stepping in the sim**

Build. The GOP row is the 5th focusable Camera row (Size,FPS,Bitrate,Codec,GOP). Capture idle then mid-edit after stepping down to a fractional value (`d` enter, 4×`s` to GOP, `d` to edit, `s` to decrement):
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t3.png PP_SIM_KEYS='dssssds' ./build-sim/pixelpilot"
```
Read `/tmp/t3.png`: GOP edits in 0.1 steps below 1.0 (e.g. shows `0.9`/`0.8`) in amber edit color. (Dummy seed `gopsize="1"` → starts at `1`.)

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/pages/camera.c
git commit -m "feat(gsmenu): GOP size fractional slider (0.0-1.0 by 0.1, then 1.0 to 30)"
```

---

## Task 4: Bitrate dropdown → Mbps slider (+ backend kbps)

**Files:** Modify `src/gsmenu/pages/camera.c`, `src/gsmenu/settings_fpvd.c`, `src/gsmenu/settings_dummy.c`.

- [ ] **Step 1: Backend — bitrate keymap to raw kbps**

In `src/gsmenu/settings_fpvd.c`, change the camera bitrate keymap entry type from `FPVD_T_BITRATE_KBPS` to `FPVD_T_INT`:
```c
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_INT,   FPVD_EP_AIR, NULL },
```
(`video.bitrate` is stored as kbps; the slider now owns kbps↔Mbps display. `FPVD_T_BITRATE_KBPS` stays defined for any future use.)

- [ ] **Step 2: Dummy seed — bitrate to kbps**

In `src/gsmenu/settings_dummy.c`, change the seed `{ "bitrate", "15M" },` to `{ "bitrate", "12000" },` (raw kbps, = 12.0 Mbps).

- [ ] **Step 3: Camera page — dropdown → slider**

In `src/gsmenu/pages/camera.c`, replace:
```c
    pp_dropdown(page, LV_SYMBOL_AUDIO, "Bitrate",
                "air", "camera", "bitrate",
                "5M\n10M\n15M\n20M\n25M");
```
with:
```c
    static const pp_slider_cfg_t bitrate_cfg = {
        .raw_min = 500, .raw_max = 26000, .step = 500,
        .fine_step = 0, .fine_threshold = 0,
        .disp_div = 1000, .decimals = 1, .unit = "Mbps", .serialize = PP_SER_INT,
    };
    pp_slider_ex(page, LV_SYMBOL_AUDIO, "Bitrate",
                 "air", "camera", "bitrate", &bitrate_cfg);
```
(`pp_dropdown` include can stay; it's still used for Size/FPS/Codec/RC Mode/Rotate.)

- [ ] **Step 4: Build + verify bitrate slider in the sim**

Build (expect `Built target pixelpilot` + `settings_tests` green). Bitrate is the 3rd focusable Camera row. Capture idle + mid-edit (`d` enter, 2×`s` to Bitrate, `d` edit, `s` down):
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t4.png PP_SIM_KEYS='dssds' ./build-sim/pixelpilot"
```
Read `/tmp/t4.png`: Bitrate row shows a value like `12.0 Mbps` (from the 12000 seed) and edits in 0.5 steps (`11.5`, `11.0`) in amber, with the dim `Mbps` unit. No dropdown caret.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/pages/camera.c src/gsmenu/settings_fpvd.c src/gsmenu/settings_dummy.c
git commit -m "feat(gsmenu): bitrate as 0.5-26 Mbps slider; backend stores raw kbps"
```

---

## Task 5: TX Power editable during dynamic link

**Files:** Modify `src/gsmenu/settings_fpvd.c`.

- [ ] **Step 1: Drop `link.txpower` from `LOCKED_PATHS`**

In `src/gsmenu/settings_fpvd.c`, remove the `"link.txpower",` line from the `LOCKED_PATHS[]` array. The remaining entries (`link.mcs`, `link.fec`, `link.width`, `video.bitrate`, `video.qpDelta`, `video.roi`) stay. Result: `prov_is_locked` returns false for TX Power even when `dynamicLink.enabled` is true.

- [ ] **Step 2: Build to confirm it compiles**

Build (Conventions). Expect `Built target pixelpilot`, no errors. (The lock only triggers with the live fpvd backend + DLink on; the dummy backend has no lock, so this is verified on-device — see Step 3.)

- [ ] **Step 3: Note on-device verification**

After deploy: enable Dynamic Link, confirm the Link ▸ TX Power row is NOT greyed/locked and a change applies without a "Locked by Dynamic Link" toast. (Other locked rows — MCS/FEC/Bandwidth/Bitrate — should still lock.)

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/settings_fpvd.c
git commit -m "feat(gsmenu): TX Power editable while dynamic link is enabled"
```

---

## Self-Review (done during planning)

**Spec coverage:** TX-power unlock (T5), `pp_slider_ex` cfg approach + wrapper (T2), fractional GOP variable-step (T1 logic + T3 wiring), bitrate Mbps slider + raw-kbps backend + dummy seed (T1 logic + T4), unit tests (T1), sim/device verification (T2-T5). ✓

**Placeholder scan:** every code step has concrete code; commands have expected output. ✓

**Type/name consistency:** `pp_slider_cfg_t` / `pp_slider_ser_t` / `PP_SER_INT` / `PP_SER_FLOAT_DIV` / `pp_slider_step`/`_fmt`/`_parse`/`_ser` are defined in T1 and used identically in T2–T4; `pp_slider_ex` defined in T2, used in T3/T4; cfg field names match the struct throughout. ✓
