# Link FEC Mode Row Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a FEC Mode selector (`rs` / `swfec`) to the Link settings page that pushes `link.fec.mode` to the drone and shows the FEC parameter rows appropriate to the selected mode.

**Architecture:** Three new keymap entries map `air/wfbng/{fec_mode,fec_deadline_ms,fec_overhead_pct}` to `link.fec.{mode,deadlineMs,overheadPct}`. The Link page (`link.c`) gains a mode dropdown plus Deadline/Overhead sliders, and a per-page snapshot listener (mirroring `dynamiclink.c`) hides the non-applicable row group via `LV_OBJ_FLAG_HIDDEN`. The `link.fec` prefix is already a locked path, so the new rows inherit the dynamic-link lock with no extra work.

**Tech Stack:** C, LVGL, cJSON, Catch2 (host sim build via `nix-shell` + `build-test`).

---

## File Structure

- `src/gsmenu/settings_fpvd.c` — keymap table (add 3 entries). No `LOCKED_PATHS` change.
- `src/gsmenu/pages/link.c` — page layout, conditional-visibility listener.
- `src/gsmenu/settings_dummy.c` — sim provider defaults + locked-key parity.
- `tests/test_settings_fpvd.cpp` — keymap + locked-path assertions.

## Build / test reference

All builds run inside the project dev shell. From repo root:

- Build tests: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail'`
- Run keymap/lock tests: `nix-shell --run './build-test/fpvd_tests "[keymap]" "[lock]"'`
- Compile-check the UI (sim GUI target, can't run headless): `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`

---

## Task 1: Keymap entries for the three new FEC fields

**Files:**
- Test: `tests/test_settings_fpvd.cpp`
- Modify: `src/gsmenu/settings_fpvd.c` (KEYMAP table, after the `fec_n` entry ~line 98)

- [ ] **Step 1: Write the failing test**

Add this new `TEST_CASE` immediately after the existing `"keymap: lookup returns null for unknown triples"` case (around line 39) in `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("keymap: FEC mode/deadline/overhead map to link.fec.*", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "wfbng", "fec_mode");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.mode") == 0);
    REQUIRE(e->type == FPVD_T_ENUM);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_deadline_ms");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.deadlineMs") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_overhead_pct");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.overheadPct") == 0);
    REQUIRE(e->type == FPVD_T_INT);
}
```

Also extend the existing `"lock: matches subtrees"` case (around line 213) by adding these three lines inside it:

```cpp
    REQUIRE(fpvd_is_locked_path("link.fec.mode") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.deadlineMs") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.overheadPct") == true);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap]"'`
Expected: FAIL — `fpvd_keymap_lookup("air","wfbng","fec_mode")` returns `nullptr` so the `REQUIRE(e != nullptr)` assertion fails. (The `[lock]` additions already pass because `link.fec` is an existing locked prefix.)

- [ ] **Step 3: Add the keymap entries**

In `src/gsmenu/settings_fpvd.c`, immediately after the `fec_n` line (currently line 98):

```c
    { "air", "wfbng", "fec_n",      "link.fec.n",      FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_mode",         "link.fec.mode",        FPVD_T_ENUM, FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_deadline_ms",  "link.fec.deadlineMs",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "fec_overhead_pct", "link.fec.overheadPct", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

(The first line above is the existing `fec_n` entry, shown for placement — add only the three new lines after it.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests "[keymap]" "[lock]"'`
Expected: PASS — all assertions green.

- [ ] **Step 5: Commit**

```bash
git add tests/test_settings_fpvd.cpp src/gsmenu/settings_fpvd.c
git commit -m "feat(gsmenu): map link.fec.mode/deadlineMs/overheadPct in keymap"
```

---

## Task 2: Sim provider defaults and locked-key parity

**Files:**
- Modify: `src/gsmenu/settings_dummy.c` (default values table ~line 64; `g_dummy_locked_keys[]` ~line 175)

This task has no unit test of its own (the dummy provider table is plain data used by the GUI sim). Verification is the compile + the sim run in Task 3.

- [ ] **Step 1: Add default values**

In `src/gsmenu/settings_dummy.c`, after the `fec_n` default (currently line 64):

```c
    { "fec_k",        "8" },
    { "fec_n",        "12" },
    { "fec_mode",          "swfec" },
    { "fec_deadline_ms",   "30" },
    { "fec_overhead_pct",  "50" },
```

(The `fec_k`/`fec_n` lines are existing, shown for placement — add only the three new lines.)

- [ ] **Step 2: Add locked-key parity**

In the same file, extend `g_dummy_locked_keys[]` (currently line 175) to include the three new keys:

```c
static const char *g_dummy_locked_keys[] = {
    "mcs_index", "txpower", "fec_k", "fec_n",
    "fec_mode", "fec_deadline_ms", "fec_overhead_pct",
    "bandwidth", /* link.width */
    "bitrate",
    "qp_delta",
    "roi_enabled", "roi_qp", "roi_center", "roi_steps",
};
```

- [ ] **Step 3: Compile-check**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean (no errors). The new rows aren't referenced yet by `link.c`, so this only verifies the data tables compile.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/settings_dummy.c
git commit -m "feat(gsmenu): sim defaults + lock parity for FEC mode/deadline/overhead"
```

---

## Task 3: Link page — mode dropdown, sliders, conditional visibility

**Files:**
- Modify: `src/gsmenu/pages/link.c`

This is UI wiring on the GUI-only sim target; it has no host unit test. Verification is a clean `pixelpilot` compile (the documented CI gate for UI/page changes) plus a manual sim run.

- [ ] **Step 1: Add includes for the visibility helper**

At the top of `src/gsmenu/pages/link.c`, after the existing `#include "../settings.h"` line, add:

```c
#include <string.h>
#include <stdlib.h>
```

- [ ] **Step 2: Add the visibility helper and combined listener**

In `src/gsmenu/pages/link.c`, insert this block immediately before `lv_obj_t *build_link_tab(lv_obj_t *parent) {`:

```c
extern void pp_page_reapply_lock_state(lv_obj_t *);

/* Show exactly one FEC parameter group based on link.fec.mode.
 * rs    -> FEC_K / FEC_N      (rows tagged LV_OBJ_FLAG_USER_1)
 * swfec -> Deadline / Overhead (rows tagged LV_OBJ_FLAG_USER_2)
 * An unknown/empty mode hides both groups until the snapshot arrives. */
static void apply_fec_visibility(lv_obj_t *page) {
    char *v = pp_settings_get("air", "wfbng", "fec_mode");
    bool is_rs    = v && strcmp(v, "rs") == 0;
    bool is_swfec = v && strcmp(v, "swfec") == 0;
    free(v);

    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1)) {
            if (is_rs) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            else       lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        } else if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            if (is_swfec) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void snapshot_listener_cb(void *user_data) {
    lv_obj_t *page = (lv_obj_t *)user_data;
    apply_fec_visibility(page);
    pp_page_reapply_lock_state(page);
}
```

- [ ] **Step 3: Add the dropdown and tag the rs-group rows**

In `build_link_tab`, replace the existing FEC_K/FEC_N block (currently lines 36–42):

```c
    lv_obj_t *fec_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
                                "air", "wfbng", "fec_k", 1, 31);
    lv_obj_t *fec_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
                                "air", "wfbng", "fec_n", 2, 32);
    /* Enforce k <= n - 2 from both sides. */
    pp_slider_set_relation(fec_k, "air", "wfbng", "fec_n", -2, /*is_max*/ true);
    pp_slider_set_relation(fec_n, "air", "wfbng", "fec_k",  2, /*is_max*/ false);
```

with:

```c
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "FEC Mode",
                "air", "wfbng", "fec_mode", "rs\nswfec");

    lv_obj_t *fec_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
                                "air", "wfbng", "fec_k", 1, 31);
    lv_obj_t *fec_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
                                "air", "wfbng", "fec_n", 2, 32);
    /* Enforce k <= n - 2 from both sides. */
    pp_slider_set_relation(fec_k, "air", "wfbng", "fec_n", -2, /*is_max*/ true);
    pp_slider_set_relation(fec_n, "air", "wfbng", "fec_k",  2, /*is_max*/ false);

    lv_obj_t *fec_deadline = pp_slider(page, LV_SYMBOL_SETTINGS, "Deadline (ms)",
                                       "air", "wfbng", "fec_deadline_ms", 10, 50);
    lv_obj_t *fec_overhead = pp_slider(page, LV_SYMBOL_SETTINGS, "Overhead (%)",
                                       "air", "wfbng", "fec_overhead_pct", 0, 100);

    /* Conditional groups: rs -> k/n, swfec -> deadline/overhead. */
    lv_obj_add_flag(fec_k,        LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(fec_n,        LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(fec_deadline, LV_OBJ_FLAG_USER_2);
    lv_obj_add_flag(fec_overhead, LV_OBJ_FLAG_USER_2);
```

- [ ] **Step 4: Wire the combined listener and initial visibility**

In `build_link_tab`, replace the existing listener-registration tail (currently lines 53–59):

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
    apply_fec_visibility(page);
    pp_page_reapply_lock_state(page);
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
```

(The `extern void pp_page_reapply_lock_state(lv_obj_t *);` declaration was moved to file scope in Step 2, so it is removed here.)

- [ ] **Step 5: Compile-check the sim**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean. If the build reports the `extern` declaration is duplicated, confirm Step 4 removed the in-function `extern` line.

- [ ] **Step 6: Manual sim verification**

Run: `./sim.sh`
Expected: On the Link page a **FEC Mode** dropdown appears above FEC_K. With `swfec` selected (sim default), **Deadline (ms)** and **Overhead (%)** are visible and FEC_K/FEC_N are hidden. Switching the dropdown to `rs` hides Deadline/Overhead and shows FEC_K/FEC_N immediately; switching back reverses it.

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/pages/link.c
git commit -m "feat(gsmenu): add FEC Mode row with mode-gated FEC params on Link page"
```

---

## Task 4: Run the full test suite

- [ ] **Step 1: Build and run all fpvd tests**

Run: `nix-shell --run 'cmake --build build-test --target fpvd_tests -j 2>&1 | tail && ./build-test/fpvd_tests'`
Expected: all test cases pass (no regressions in `[config]`/`[network]`/`[endpoint]`/`[caps]` from the keymap addition).

- [ ] **Step 2: Final compile-check of the sim**

Run: `nix-shell --run 'cmake --build build-test --target pixelpilot -j 2>&1 | tail'`
Expected: builds clean.

---

## Notes for the implementer

- `pp_settings_get(domain, page, key)` returns a heap `char*` that the caller must `free()` (it may be `NULL`). This is why `apply_fec_visibility` frees `v` before reading the booleans.
- `FPVD_T_ENUM` stores the dropdown's selected option text verbatim, so the options string must be exactly `"rs\nswfec"` — no friendly labels.
- LVGL group navigation skips objects with `LV_OBJ_FLAG_HIDDEN`, so hidden rows added to the page group are correctly bypassed during focus movement. This is the same arrangement used by `dynamiclink.c` (group-add loop runs, then `apply_*_visibility`).
- Do not touch `LOCKED_PATHS` — `"link.fec"` already covers `link.fec.mode`, `link.fec.deadlineMs`, and `link.fec.overheadPct` as subtree matches.
