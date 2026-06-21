# Dynamic-link FEC-mode-aware UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Dynamic Link lock UX aware of the Link page's FEC mode, and remove the obsolete Failsafe section from the Dynamic Link page.

**Architecture:** The GS menu's lock engine is centralized and data-driven: `prov_is_locked` reads the live air snapshot, and `pp_page_reapply_lock_state` re-evaluates every row on each snapshot fanout. We add one shared, mode-aware predicate (`dl_locks_field`) that both the widget-grey path (`prov_is_locked`) and the apply/push gate (`prov_set_async`) call, plus a matching change in the simulator provider. The Dynamic Link page only loses its Failsafe rows.

**Tech Stack:** C (gsmenu provider + LVGL pages), C++ Catch2 v3 host tests, cJSON, cpp-httplib mock server, Nix dev shell.

## Global Constraints

- **GS-menu-only change. No drone-side behavior changes.** (spec: Out of scope)
- **Host build/test dir:** `build-sim/` with `shell-sim.nix`, `CMAKE_BUILD_TYPE=Release`. Always wrap in `nix-shell shell-sim.nix --run '...'`. Reconfigure with `cmake -DCMAKE_BUILD_TYPE=Release build-sim` if a CMake target/source changes.
- **Tests use Catch2 v3** (`#include <catch2/catch_test_macros.hpp>`).
- **`dl_locks_field` must be called WITHOUT holding `G.mu`** — it acquires the mutex internally to read the snapshot.
- **Do NOT modify `fpvd_is_locked_path` or `LOCKED_PATHS`.** The FEC-mode exceptions live only in `dl_locks_field`; `link.fec.*` stays under the `link.fec` locked-path prefix. (So `tests/test_settings_fpvd.cpp` lines asserting `fpvd_is_locked_path("link.fec.mode")==true` etc. remain valid and must be left untouched.)
- Lock rule table (Dynamic Link **on**):

  | Field | rs mode | swfec mode |
  | --- | --- | --- |
  | `air/wfbng/fec_mode` | unlocked | unlocked |
  | `air/wfbng/fec_deadline_ms`, `fec_overhead_pct` | locked (hidden) | **unlocked** |
  | `air/wfbng/fec_k`, `fec_n` | locked | locked (hidden) |
  | `air/dlink/compute_base_redundancy`, `compute_blocks_per_frame` | unlocked | **locked** |
  | other `LOCKED_PATHS` (bandwidth, bitrate, roi, …) | locked | locked |

  Dynamic Link **off** → everything unlocked (unchanged).

---

## File Structure

- `src/gsmenu/settings_fpvd.c` — add `dl_locks_field`; rewire `prov_is_locked` + `prov_set_async`; remove `safe_*` keymap entries; add the `flightlog_enabled` keymap entry (Tasks 1, 2, 4, 5).
- `src/gsmenu/settings_dummy.c` — mode-aware `dummy_is_locked`; remove `safe_*` seeds; add the `flightlog_enabled` seed (Tasks 3, 4, 5).
- `src/gsmenu/pages/dynamiclink.c` — remove the Failsafe section; add the Flight Log toggle (Tasks 4, 5).
- `tests/test_settings_fpvd_integration.cpp` — lock + apply-gate behavior tests (Tasks 1, 2).
- `tests/test_settings_dummy_lock.cpp` — **new**, simulator-provider lock parity (Task 3).
- `tests/test_dynamiclink_page.cpp` — Failsafe-gone / Compute-present / Flight Log assertions (Tasks 4, 5).
- `tests/test_settings_fpvd.cpp` — drop `safe_*` keymap assertions; add the `flightlog_enabled` keymap assertion (Tasks 4, 5).
- `CMakeLists.txt` — register the new test file in `settings_tests` (Task 3).

---

### Task 1: Mode-aware lock predicate — widget-grey path

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (add `dl_locks_field` just above `prov_set_async` ~line 871; rewrite `prov_is_locked` ~lines 950-972)
- Test: `tests/test_settings_fpvd_integration.cpp`

**Interfaces:**
- Produces: `static bool dl_locks_field(const fpvd_keymap_entry_t *e, const char *d, const char *p, const char *k)` — true when the field is currently locked by Dynamic Link, applying the FEC-mode exceptions. Caller must not hold `G.mu`. Used here by `prov_is_locked` and in Task 2 by `prov_set_async`.
- Consumes: existing `fpvd_keymap_lookup`, `fpvd_is_locked_path`, `G.air_snapshot`, `G.mu`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_settings_fpvd_integration.cpp` (uses the existing `GsMockServer` / `install_provider_pointing_at` helpers in that file):

```cpp
TEST_CASE("integration: DL on + swfec — mode/deadline/overhead editable, "
          "k/n + compute knobs locked", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == true);
    /* Min/Max bitrate + Max MCS stay editable in both modes. */
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_min_bitrate_kbps") == false);
    m.stop();
}

TEST_CASE("integration: DL on + rs — compute knobs editable, k/n locked, "
          "deadline/overhead locked (hidden)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"rs"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == false);
    m.stop();
}

TEST_CASE("integration: DL off — FEC + compute rows all editable", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    m.stop();
}
```

- [ ] **Step 2: Build + run to verify failure**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[network]"'
```
Expected: FAIL — the swfec `fec_mode`/`deadline`/`overhead` cases expect `false` but today every `link.fec.*` is locked when DL is on; the compute cases expect `true` but today `dynamicLink.compute.*` is never locked.

- [ ] **Step 3: Add `dl_locks_field`**

In `src/gsmenu/settings_fpvd.c`, immediately above `static void prov_set_async(...)` (~line 871), add:

```c
/* True when the Dynamic Link lock currently governs this field, applying the
 * FEC-mode-aware exceptions:
 *   - fec_mode is always editable (the user selects rs/swfec even with DL on).
 *   - In swfec mode, deadlineMs/overheadPct are editable, and the compute
 *     baseRedundancyRatio/blocksPerFrame become locked (swfec ignores them).
 * Caller passes the already-resolved keymap entry; must NOT hold G.mu. */
static bool dl_locks_field(const fpvd_keymap_entry_t *e,
                           const char *d, const char *p, const char *k) {
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on   = en && cJSON_IsTrue(en);
    cJSON *link  = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "link") : NULL;
    cJSON *fec   = link ? cJSON_GetObjectItemCaseSensitive(link, "fec") : NULL;
    cJSON *mode  = fec ? cJSON_GetObjectItemCaseSensitive(fec, "mode") : NULL;
    bool swfec   = mode && cJSON_IsString(mode) && strcmp(mode->valuestring, "swfec") == 0;
    pthread_mutex_unlock(&G.mu);

    if (!dl_on) return false;

    bool is_air_wfbng = (!strcmp(d, "air") && !strcmp(p, "wfbng"));
    bool is_air_dlink = (!strcmp(d, "air") && !strcmp(p, "dlink"));

    /* FEC Mode: always editable. */
    if (is_air_wfbng && !strcmp(k, "fec_mode")) return false;
    /* swfec: deadline/overhead editable; in rs they're hidden anyway. */
    if (swfec && is_air_wfbng &&
        (!strcmp(k, "fec_deadline_ms") || !strcmp(k, "fec_overhead_pct")))
        return false;
    /* swfec: the compute redundancy/blocks knobs are ignored, so grey them. */
    if (swfec && is_air_dlink &&
        (!strcmp(k, "compute_base_redundancy") || !strcmp(k, "compute_blocks_per_frame")))
        return true;

    /* Default: locked iff the path is under a locked prefix. */
    return fpvd_is_locked_path(e->path);
}
```

- [ ] **Step 4: Rewire `prov_is_locked`**

Replace the body of `prov_is_locked` (from the `is_bandwidth` line through the closing brace, ~lines 958-972) so the tail delegates to the helper:

```c
    /* DL governs drone-owned (AIR) fields. The Bandwidth row is the one
     * GS-row exception: gs/wfbng/bandwidth -> link.width is pushed to the
     * drone and rejected by its dynamic-link lock, so disable it too. Other
     * GS rows (e.g. rx_power = the GS card's own power) stay editable. */
    bool is_bandwidth = (!strcmp(d, "gs") && !strcmp(p, "wfbng") &&
                         !strcmp(k, "bandwidth"));
    if (e->endpoint != FPVD_EP_AIR && !is_bandwidth) return false;
    return dl_locks_field(e, d, p, k);
}
```

(Keep the `pp_runtime_cfg_owns` short-circuit and the `fpvd_keymap_lookup` + `if (!e) return false;` lines above this unchanged. The previous `fpvd_is_locked_path` early-return and the inline snapshot read are now gone — `dl_locks_field` owns that decision.)

- [ ] **Step 5: Build + run to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[network]"'
```
Expected: PASS — all `[network]` cases, including the three new ones.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp
git commit -m "gsmenu: FEC-mode-aware Dynamic Link lock predicate (widget-grey path)"
```

---

### Task 2: Apply/push gate consistency — write path

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (`prov_set_async` DL gate, ~lines 880-888)
- Test: `tests/test_settings_fpvd_integration.cpp`

**Interfaces:**
- Consumes: `dl_locks_field` (from Task 1).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_settings_fpvd_integration.cpp`:

```cpp
TEST_CASE("integration: FEC mode push allowed while DL on (swfec)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    DoneWaiter w;
    pp_settings_set_async("air", "wfbng", "fec_mode", "rs", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);                       /* not rejected by the DL lock */
    auto writes = m.writes_only();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(m.last_air_patch_body.find("\"mode\":\"rs\"") != std::string::npos);
    m.stop();
}

TEST_CASE("integration: compute knob push rejected while DL on (swfec)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    DoneWaiter w;
    pp_settings_set_async("air", "dlink", "compute_base_redundancy", "0.7",
                          DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);
    REQUIRE(w.err_str() == "Locked by Dynamic Link");
    for (auto &l : m.snapshot_log())
        REQUIRE(l != "PATCH /air/config");    /* never sent */
    m.stop();
}
```

- [ ] **Step 2: Build + run to verify failure**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[network]"'
```
Expected: FAIL — today the gate rejects `fec_mode` (it's under `link.fec`, DL on) so the first case's `rc == 0` fails; and it allows `compute_base_redundancy` (not a locked path) so the second case's `rc != 0` fails.

- [ ] **Step 3: Rewire the apply gate**

In `prov_set_async` (`src/gsmenu/settings_fpvd.c`), replace the DL-lock block (the `if (e->endpoint == FPVD_EP_AIR && fpvd_is_locked_path(e->path)) { ... }` block, ~lines 880-888) with:

```c
    /* Dynamic-link lock only governs drone-owned (AIR) fields; the mode-aware
     * exceptions live in dl_locks_field (shared with prov_is_locked). */
    if (e->endpoint == FPVD_EP_AIR && dl_locks_field(e, d, p, k)) {
        schedule_done(cb, ud, -1, "Locked by Dynamic Link");
        return;
    }
```

- [ ] **Step 4: Build + run to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[network]"'
```
Expected: PASS — all `[network]` cases, including the two new ones, plus the existing `dynamic_link_locked` rejection test still passes (mcs_index stays locked).

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp
git commit -m "gsmenu: route the Dynamic Link apply gate through dl_locks_field"
```

---

### Task 3: Simulator provider lock parity

**Files:**
- Modify: `src/gsmenu/settings_dummy.c` (`g_dummy_locked_keys` ~lines 162-169; `dummy_is_locked` ~lines 185-194)
- Create: `tests/test_settings_dummy_lock.cpp`
- Modify: `CMakeLists.txt` (add the new file to the `settings_tests` target, ~line 238)

**Interfaces:**
- Consumes: `pp_settings_register_dummy`, `pp_settings_set`, `pp_settings_is_locked` (public `settings.h` API). The dummy provider keys off the per-session overlay, which `pp_settings_set` writes synchronously.

- [ ] **Step 1: Write the failing test**

Create `tests/test_settings_dummy_lock.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "gsmenu/settings.h"
}

TEST_CASE("dummy lock: FEC mode always editable; swfec frees deadline/overhead "
          "and locks the compute knobs", "[caps][dummy]") {
    pp_settings_register_dummy();

    /* Dynamic Link ON + swfec. */
    pp_settings_set("air", "dlink", "enabled", "on");
    pp_settings_set("air", "wfbng", "fec_mode", "swfec");

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == true);

    /* Switch to rs: compute knobs editable; deadline/overhead locked (hidden). */
    pp_settings_set("air", "wfbng", "fec_mode", "rs");
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);

    /* Dynamic Link OFF: nothing locked. */
    pp_settings_set("air", "dlink", "enabled", "off");
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
}
```

- [ ] **Step 2: Register the test file**

In `CMakeLists.txt`, inside the `add_executable(settings_tests ...)` source list, add the new file after `tests/test_slider_scale.cpp`:

```cmake
      tests/test_slider_scale.cpp
      tests/test_settings_dummy_lock.cpp)
```

- [ ] **Step 3: Reconfigure, build + run to verify failure**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake -DCMAKE_BUILD_TYPE=Release build-sim >/dev/null && cmake --build build-sim --target settings_tests -j 2>&1 | tail -3 && ./build-sim/settings_tests "[dummy]"'
```
Expected: FAIL — today `fec_mode` is in `g_dummy_locked_keys` (so it reports locked when DL on) and the compute knobs are never locked.

- [ ] **Step 4: Update `dummy_is_locked` and the locked-key list**

In `src/gsmenu/settings_dummy.c`, remove `"fec_mode"` from `g_dummy_locked_keys` (keep `fec_deadline_ms`/`fec_overhead_pct`):

```c
static const char *g_dummy_locked_keys[] = {
    "mcs_index", "txpower", "fec_k", "fec_n",
    "fec_deadline_ms", "fec_overhead_pct",
    "bandwidth", /* link.width */
    "bitrate",
    "qp_delta",
    "roi_enabled", "roi_qp", "roi_center", "roi_steps",
};
```

Replace `dummy_is_locked` with the mode-aware version:

```c
static bool dummy_is_locked(const char *d, const char *p, const char *k) {
    (void)d; (void)p;
    const char *enabled = find_value("enabled");
    bool dlink_on = enabled && strcmp(enabled, "on") == 0;
    if (!dlink_on) return false;

    const char *mode = find_value("fec_mode");
    bool swfec = mode && strcmp(mode, "swfec") == 0;

    /* swfec: deadline/overhead become editable; the compute redundancy/blocks
     * knobs become locked. Mirrors dl_locks_field() in settings_fpvd.c. */
    if (swfec && (strcmp(k, "fec_deadline_ms") == 0 ||
                  strcmp(k, "fec_overhead_pct") == 0))
        return false;
    if (swfec && (strcmp(k, "compute_base_redundancy") == 0 ||
                  strcmp(k, "compute_blocks_per_frame") == 0))
        return true;

    for (size_t i = 0; i < sizeof(g_dummy_locked_keys)/sizeof(g_dummy_locked_keys[0]); i++) {
        if (strcmp(g_dummy_locked_keys[i], k) == 0) return true;
    }
    return false;
}
```

- [ ] **Step 5: Build + run to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests -j 2>&1 | tail -3 && ./build-sim/settings_tests "[dummy]"'
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_dummy.c tests/test_settings_dummy_lock.cpp CMakeLists.txt
git commit -m "gsmenu sim: mirror the FEC-mode-aware Dynamic Link lock"
```

---

### Task 4: Remove the Failsafe section (full)

**Files:**
- Modify: `src/gsmenu/pages/dynamiclink.c` (delete the Failsafe block, ~lines 78-88)
- Modify: `src/gsmenu/settings_fpvd.c` (delete the four `safe_*` keymap entries, ~lines 113-116)
- Modify: `src/gsmenu/settings_dummy.c` (delete the four `safe_*` seeds, ~lines 81-85)
- Test: `tests/test_dynamiclink_page.cpp` (replace the two Failsafe cases)
- Test: `tests/test_settings_fpvd.cpp` (drop two `safe_*` keymap assertions)

- [ ] **Step 1: Rewrite the page tests (failing)**

In `tests/test_dynamiclink_page.cpp`, replace the two existing `TEST_CASE`s ("Dynamic Link page drops deprecated Failsafe rows" and "Dynamic Link page keeps the surviving Failsafe rows", lines 70-94) with:

```cpp
TEST_CASE("Dynamic Link page drops the whole Failsafe section", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    /* Header + all four Failsafe rows are gone. */
    REQUIRE_FALSE(subtree_has_label(page, "Failsafe"));
    REQUIRE_FALSE(subtree_has_label(page, "MCS"));
    REQUIRE_FALSE(subtree_has_label(page, "FEC K"));
    REQUIRE_FALSE(subtree_has_label(page, "FEC N"));
    REQUIRE_FALSE(subtree_has_label(page, "Bitrate (kbps)"));

    lv_obj_delete(scr);
}

TEST_CASE("Dynamic Link page keeps the Compute rows", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    REQUIRE(subtree_has_label(page, "Base Redundancy Ratio"));
    REQUIRE(subtree_has_label(page, "Blocks / Frame"));
    REQUIRE(subtree_has_label(page, "Min Bitrate"));
    REQUIRE(subtree_has_label(page, "Max Bitrate"));
    REQUIRE(subtree_has_label(page, "Max MCS"));

    lv_obj_delete(scr);
}
```

- [ ] **Step 2: Build + run to verify failure**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/dynamiclink_page_tests'
```
Expected: FAIL — the page still builds the `"Failsafe"` header and the `MCS`/`FEC K`/`FEC N`/`Bitrate (kbps)` rows, so the `REQUIRE_FALSE`s fail.

- [ ] **Step 3: Remove the Failsafe section from the page**

In `src/gsmenu/pages/dynamiclink.c`, delete this block (after the `Max MCS` slider, ~lines 78-88):

```c
    pp_section_header(page, "Failsafe");
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS",
              "air", "dlink", "safe_mcs", 0, 7);
    lv_obj_t *safe_k = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC K",
                                 "air", "dlink", "safe_k", 1, 31);
    lv_obj_t *safe_n = pp_slider(page, LV_SYMBOL_SETTINGS, "FEC N",
                                 "air", "dlink", "safe_n", 2, 32);
    pp_slider_set_relation(safe_k, "air", "dlink", "safe_n", -2, /*is_max*/ true);
    pp_slider_set_relation(safe_n, "air", "dlink", "safe_k",  2, /*is_max*/ false);
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate (kbps)",
              "air", "dlink", "safe_bitrate_kbps", 500, 30000);
```

- [ ] **Step 4: Build + run to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/dynamiclink_page_tests'
```
Expected: PASS.

- [ ] **Step 5: Remove the `safe_*` keymap entries**

In `src/gsmenu/settings_fpvd.c`, delete the four keymap rows (~lines 113-116):

```c
    { "air", "dlink", "safe_mcs",             "dynamicLink.safe.mcs",             FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "safe_k",               "dynamicLink.safe.k",               FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "safe_n",               "dynamicLink.safe.n",               FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "dlink", "safe_bitrate_kbps",    "dynamicLink.safe.bitrateKbps",     FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

- [ ] **Step 6: Remove the `safe_*` simulator seeds**

In `src/gsmenu/settings_dummy.c`, delete the comment + four seed entries (~lines 81-85):

```c
    /* Dynamic Link — Safe Ceilings */
    { "safe_mcs",         "1" },
    { "safe_k",           "8" },
    { "safe_n",           "12" },
    { "safe_bitrate_kbps","2000" },
```

- [ ] **Step 7: Drop the `safe_*` keymap test assertions**

In `tests/test_settings_fpvd.cpp`:

(a) In the first keymap `TEST_CASE`, delete the trailing `safe_bitrate_kbps` block so it ends at the `fec_k` assertion (remove the blank line + these three lines, ~lines 32-35):

```cpp

    e = fpvd_keymap_lookup("air", "dlink", "safe_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.safe.bitrateKbps") == 0);
```

(b) In the dlink `enabled`-kind `TEST_CASE`, the `safe_mcs` lookup (~lines 590-591) asserts a sibling dlink row is `FPVD_ROW_PLAIN`. Repoint it to a surviving plain dlink row:

```cpp
    /* the other dlink rows stay plain drone-only writes */
    e = fpvd_keymap_lookup("air", "dlink", "compute_base_redundancy");
    REQUIRE(e->kind == FPVD_ROW_PLAIN);
```

- [ ] **Step 8: Build + run both suites to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests settings_tests dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[keymap]" && ./build-sim/fpvd_tests "[network]" && ./build-sim/dynamiclink_page_tests'
```
Expected: PASS — no assertion references `dynamicLink.safe.*`; the page builds without the Failsafe section.

- [ ] **Step 9: Commit**

```bash
git add src/gsmenu/pages/dynamiclink.c src/gsmenu/settings_fpvd.c src/gsmenu/settings_dummy.c tests/test_dynamiclink_page.cpp tests/test_settings_fpvd.cpp
git commit -m "gsmenu: remove the Dynamic Link Failsafe section (UI + keymap + seeds + tests)"
```

---

### Task 5: Add the Flight Log toggle (GS `dynamicLink.flightlog.enabled`)

The GS config (`http://10.18.0.1:8080/gs/config`) exposes
`dynamicLink.flightlog.enabled` (boolean) alongside `dynamicLink.maxMcs`. It is a
**GS-domain field** (`FPVD_EP_GS`), so it is pushed via `PATCH /gs/config` and is
**never** governed by the dynamic-link lock (GS rows are exempt in
`prov_is_locked`). It renders as a toggle in the Dynamic Link page's General
section, right after the Enabled toggle — placing it past the
`LV_OBJ_FLAG_USER_3` visibility anchor means it shows only while Dynamic Link is
enabled, consistent with the rest of the page.

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (add one keymap entry after the `max_mcs` row, ~line 112)
- Modify: `src/gsmenu/pages/dynamiclink.c` (add the toggle after the Enabled toggle, ~line 45)
- Modify: `src/gsmenu/settings_dummy.c` (add one seed default after `max_mcs`)
- Test: `tests/test_settings_fpvd.cpp` (keymap lookup assertion)
- Test: `tests/test_dynamiclink_page.cpp` (row-presence assertion)

**Interfaces:**
- Produces: keymap triple `gs/dlink/flightlog_enabled` → path `dynamicLink.flightlog.enabled`, `FPVD_T_BOOL`, `FPVD_EP_GS`, `FPVD_ROW_PLAIN`.

- [ ] **Step 1: Write the failing tests**

(a) In `tests/test_settings_fpvd.cpp`, inside the existing keymap `TEST_CASE`
"keymap: camera resilience/osd + dlink compute/maxMcs" (after the
`compute_base_redundancy` assertion block, ~line 59), add:

```cpp
    e = fpvd_keymap_lookup("gs", "dlink", "flightlog_enabled");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.flightlog.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);
    REQUIRE(e->endpoint == FPVD_EP_GS);
```

(b) In `tests/test_dynamiclink_page.cpp`, add a row-presence case:

```cpp
TEST_CASE("Dynamic Link page has the Flight Log toggle", "[dynamiclink]") {
    lv_obj_t *scr = setup_screen();
    lv_obj_t *page = build_dynamiclink_tab(scr);

    REQUIRE(subtree_has_label(page, "Flight Log"));

    lv_obj_delete(scr);
}
```

- [ ] **Step 2: Build + run to verify failure**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[keymap]" && ./build-sim/dynamiclink_page_tests'
```
Expected: FAIL — `fpvd_keymap_lookup("gs","dlink","flightlog_enabled")` returns null, and the page has no `"Flight Log"` label.

- [ ] **Step 3: Add the keymap entry**

In `src/gsmenu/settings_fpvd.c`, add this row immediately after the `max_mcs`
entry (`{ "gs", "dlink", "max_mcs", "dynamicLink.maxMcs", ... }`):

```c
    { "gs",  "dlink", "flightlog_enabled",        "dynamicLink.flightlog.enabled",           FPVD_T_BOOL,  FPVD_EP_GS,  FPVD_ROW_PLAIN },
```

- [ ] **Step 4: Add the page toggle**

In `src/gsmenu/pages/dynamiclink.c`, after
`lv_obj_add_flag(enabled, LV_OBJ_FLAG_USER_3);` and before
`pp_section_header(page, "Compute");`, add:

```c
    pp_toggle(page, LV_SYMBOL_SD_CARD, "Flight Log",
              "gs", "dlink", "flightlog_enabled");
```

- [ ] **Step 5: Add the simulator seed default**

In `src/gsmenu/settings_dummy.c`, in the Dynamic Link seed block, add after the
`{ "max_mcs", "5" },` entry:

```c
    { "flightlog_enabled",        "on" },
```

- [ ] **Step 6: Build + run to verify pass**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests "[keymap]" && ./build-sim/dynamiclink_page_tests'
```
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/pages/dynamiclink.c src/gsmenu/settings_dummy.c tests/test_settings_fpvd.cpp tests/test_dynamiclink_page.cpp
git commit -m "gsmenu: add Flight Log toggle (GS dynamicLink.flightlog.enabled) to Dynamic Link page"
```

---

### Task 6: Full regression + simulator compile

**Files:** none (verification only)

- [ ] **Step 1: Run all affected suites**

Run:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests settings_tests dynamiclink_page_tests -j 2>&1 | tail -3 && ./build-sim/fpvd_tests && ./build-sim/settings_tests && ./build-sim/dynamiclink_page_tests'
```
Expected: PASS — every case across the three binaries (no tag filter).

- [ ] **Step 2: Compile-check the GUI simulator**

The simulator can't run headless here, so a clean compile is the gate for the page/widget changes:
```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target pixelpilot -j 2>&1 | tail -5'
```
Expected: `Built target pixelpilot` with no errors.

- [ ] **Step 3 (manual, on GS): live mode-flip check**

Per the spec data flow, on the ground station: enable Dynamic Link, then on the Link page flip **FEC Mode** between `rs` and `swfec`. Confirm:
- `swfec`: Deadline / Overhead become editable; on the Dynamic Link page, **Base Redundancy Ratio** and **Blocks / Frame** grey out.
- `rs`: FEC_K / FEC_N stay greyed; the two compute rows un-grey.
- The Dynamic Link page has no **Failsafe** section.
- The Dynamic Link page shows a **Flight Log** toggle (visible while Dynamic Link
  is enabled); toggling it round-trips to the GS config
  (`dynamicLink.flightlog.enabled`) and stays editable regardless of FEC mode.

---

## Self-Review

**Spec coverage:**
- "FEC Mode always editable" → Task 1 (`fec_mode` exception) + Task 2 (push allowed). ✓
- "swfec frees Deadline/Overhead" → Task 1. ✓
- "swfec greys Base Redundancy + Blocks/Frame" → Task 1 (compute lock) + Task 2 (push rejected). ✓
- "rs keeps FEC_K/FEC_N locked" → Task 1 (default branch via `fpvd_is_locked_path`). ✓
- "Shared predicate across widget-grey and apply gate" → Tasks 1 + 2 both call `dl_locks_field`. ✓
- "Sim parity" → Task 3. ✓
- "Full Failsafe removal (UI + keymap + seeds + tests)" → Task 4. ✓
- "Add Flight Log toggle (GS `dynamicLink.flightlog.enabled`)" → Task 5 (plan addition beyond the original spec; GS-domain row, lock-exempt). ✓
- "No drone-side changes" → no task touches drone code. ✓

**Placeholder scan:** no TBD/TODO; every code + command step is concrete. ✓

**Type consistency:** `dl_locks_field(const fpvd_keymap_entry_t *, const char *, const char *, const char *)` is defined in Task 1 and called identically in Tasks 1 and 2. `dummy_is_locked` signature unchanged. Test helpers (`GsMockServer`, `DoneWaiter`, `install_provider_pointing_at`, `subtree_has_label`, `setup_screen`) are pre-existing in their files. ✓
