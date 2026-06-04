# Disable Bandwidth Row Under Dynamic Link — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock the Link-tab **Bandwidth** dropdown (greyed + "Locked by Dynamic Link") whenever Dynamic Link is enabled, while leaving the GS-only `rx_power` row editable.

**Architecture:** One narrow special-case in `prov_is_locked` (`src/gsmenu/settings_fpvd.c`) marks `gs/wfbng/bandwidth` as DL-lockable in addition to AIR rows. All UI mechanics (build-time lock in `pp_dropdown`, live re-lock in `pp_page_reapply_lock_state`, the locked-edit toast) already consume `pp_settings_is_locked`, so no other code changes are needed.

**Tech Stack:** C (LVGL gsmenu), Catch2 host tests under `USE_SIMULATOR=ON` (`build-test/`), nix-shell build env.

**Reference spec:** `docs/superpowers/specs/2026-06-04-gsmenu-bandwidth-dl-lock-design.md`

---

### Task 1: Lock the Bandwidth row when Dynamic Link is enabled

**Files:**
- Test: `tests/test_settings_fpvd_integration.cpp` (add two `TEST_CASE`s near the existing `dynamic_link_locked` case ~line 182)
- Modify: `src/gsmenu/settings_fpvd.c` — `prov_is_locked` (currently ~lines 828-839)

- [ ] **Step 1: Write the failing tests**

Append these two cases to `tests/test_settings_fpvd_integration.cpp` (they reuse the file's existing `GsMockServer`, `install_provider_pointing_at`, and `wait_first_poll` helpers — do not redefine them):

```cpp
TEST_CASE("integration: Bandwidth row DL-locked while rx_power stays editable",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    // Bandwidth (gs/wfbng/bandwidth -> link.width) is pushed to the drone and
    // rejected by its DL lock, so it must report locked while DL is on.
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == true);
    // rx_power (gs/link/rx_power -> link.txpower) is the GS card's OWN power
    // (apply_to "gs"), not drone-controlled — it must stay editable.
    REQUIRE(pp_settings_is_locked("gs", "link", "rx_power") == false);
    m.stop();
}

TEST_CASE("integration: Bandwidth row editable when Dynamic Link is off",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == false);
    m.stop();
}
```

- [ ] **Step 2: Build and run the new tests — verify they FAIL**

Run:
```bash
nix-shell --run 'cmake --build build-test --target fpvd_tests -j4' \
  && ./build-test/fpvd_tests "integration: Bandwidth row DL-locked while rx_power stays editable"
```
Expected: FAIL on `REQUIRE(pp_settings_is_locked("gs","wfbng","bandwidth") == true)` (currently returns `false` — `prov_is_locked` bails for the non-AIR Bandwidth row). The DL-off case and the rx_power assertion already pass; only the Bandwidth-locked assertion fails.

- [ ] **Step 3: Implement the fix**

In `src/gsmenu/settings_fpvd.c`, edit `prov_is_locked`. Replace:

```c
static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    if (e->endpoint != FPVD_EP_AIR) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
```

with:

```c
static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    /* DL governs drone-owned (AIR) fields. The Bandwidth row is the one LINK
     * exception: gs/wfbng/bandwidth -> link.width is pushed to the drone and
     * rejected by its dynamic-link lock, so disable it too. Other LINK rows
     * (e.g. rx_power = the GS card's own power) stay editable. */
    bool is_bandwidth = (!strcmp(d, "gs") && !strcmp(p, "wfbng") &&
                         !strcmp(k, "bandwidth"));
    if (e->endpoint != FPVD_EP_AIR && !is_bandwidth) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
```

(`<string.h>` is already included in this file; the rest of the function — reading `air_snapshot.dynamicLink.enabled` under the mutex — is unchanged.)

- [ ] **Step 4: Build and run the new tests — verify they PASS**

Run:
```bash
nix-shell --run 'cmake --build build-test --target fpvd_tests -j4' \
  && ./build-test/fpvd_tests "integration: Bandwidth row DL-locked while rx_power stays editable" \
  && ./build-test/fpvd_tests "integration: Bandwidth row editable when Dynamic Link is off"
```
Expected: PASS (both cases, all REQUIREs).

- [ ] **Step 5: Run the full fpvd_tests suite — no regressions**

Run:
```bash
./build-test/fpvd_tests
```
Expected: all assertions pass (existing AIR DL-lock cases unaffected; `rx_power` still editable).

- [ ] **Step 6: Commit**

```bash
git add tests/test_settings_fpvd_integration.cpp src/gsmenu/settings_fpvd.c
git commit -m "$(printf 'feat(gsmenu): lock Bandwidth row when Dynamic Link is enabled\n\nBandwidth maps to link.width, which the drone DL-locks; selecting it while\nDL is on cannot take effect. prov_is_locked now treats gs/wfbng/bandwidth as\nDL-lockable (reusing PP_ROW_LOCKED_DYNAMIC). rx_power (GS card power) stays\neditable.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Self-Review

- **Spec coverage:** Lock Bandwidth when DL on (Step 1 case 1 + Step 3 fix) ✓; rx_power stays editable (Step 1 case 1 assertion) ✓; DL-off leaves Bandwidth editable (Step 1 case 2) ✓; reuse of existing PP_ROW_LOCKED_DYNAMIC machinery (no code needed, noted in Architecture) ✓; `d/p/k` match approach ✓.
- **Placeholder scan:** none — all code and commands are concrete.
- **Type consistency:** `pp_settings_is_locked`, `prov_is_locked`, `fpvd_keymap_lookup`, `fpvd_is_locked_path`, `GsMockServer`, `install_provider_pointing_at`, `wait_first_poll` all match existing names in the file/codebase.
