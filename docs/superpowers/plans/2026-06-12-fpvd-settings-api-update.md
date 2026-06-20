# fpvd Settings API Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update PixelPilot's fpvd settings provider to the new client-orchestrated fpvd-GS API: `/gs/*` config tree, drone-first orchestration for shared link settings, a Beamforming toggle, and TX power in dBm.

**Architecture:** The single-provider job model in `src/gsmenu/settings_fpvd.c` is retained (worker thread, coalescing queue, snapshots, keymap). The old `/link` coordinator and root `/config` endpoint groups collapse into one `FPVD_EP_GS` group (`/gs/config` + `/gs/apply`). A new pure "step planner" function turns a job into an ordered list of HTTP steps (drone-first for shared rows, MAC handshake for beamforming, GS-only degradation when the drone is offline), which the job runner executes with per-step retries. Reachability splits into `gs_connected` and `drone_reachable` (fed by a new `GET /gs/status` poll).

**Tech Stack:** C (provider, LVGL UI), cJSON, libcurl, Catch2 + cpp-httplib mock server for tests. Host test builds run via `nix-shell shell-sim.nix` in `build-sim/`.

**Spec:** `docs/superpowers/specs/2026-06-12-fpvd-settings-api-update-design.md`
**API reference:** `/home/gilankpam/Projects/drone/fpvd/docs/api.md`

**Branch:** `feat/fpvd-settings-api-update` (already created, spec committed).

## Build & test commands (used throughout)

```bash
# Build a test target (from repo root):
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j'
# Run it:
./build-sim/fpvd_tests                      # all
./build-sim/fpvd_tests '[plan]'             # by tag
# Other targets: settings_tests, pixelpilot (sim GUI — compile gate only)
```

Notes from the build memory: do NOT reconfigure `build-test/` under `shell-sim.nix`; stay in `build-sim/`. If link errors mention `__asan_init`, run `nix-shell shell-sim.nix --run 'cmake -DCMAKE_BUILD_TYPE=Release build-sim'` once and rebuild. If CMakeLists.txt changed, reconfigure with `nix-shell shell-sim.nix --run 'cmake build-sim && cmake --build build-sim --target <t> -j'`.

## File map

| File | Change |
|---|---|
| `src/gsmenu/settings_fpvd_internal.h` | `FPVD_EP_GS`, `fpvd_row_kind_t`, `fpvd_step_t`, `fpvd_plan_steps()`, drop `FPVD_T_RXPOWER`, keymap entry gets `kind` instead of `apply_to` |
| `src/gsmenu/settings_fpvd.c` | keymap rewrite, path helpers, `LOCKED_PATHS` rename, step planner, runner rewrite, `/gs/status` poll, reachability split, `is_reachable`, txpower null fallback |
| `src/gsmenu/settings.h` / `settings.c` | provider method `is_reachable` + wrapper `pp_settings_is_reachable()` |
| `src/gsmenu/helper.c` | `pp_page_reapply_lock_state` uses per-row reachability |
| `src/gsmenu/settings_dummy.c` | dBm seeds, `beamforming` seed, `is_reachable` + `PP_SIM_DRONE_OFFLINE` |
| `src/gsmenu/pages/link.c` | dBm sliders (−10..30, unit label), Beamforming toggle |
| `src/gsmenu/settings_gs_rxpower.c/h` | **deleted** |
| `tests/test_settings_fpvd.cpp` | keymap/endpoint/lock/plan tests updated + added |
| `tests/test_settings_fpvd_integration.cpp` | mock server rewritten to new routes; orchestration tests |
| `tests/test_settings_gs_rxpower.cpp` | **deleted** |
| `tests/test_settings.cpp` | dummy/dispatcher reachability tests |
| `CMakeLists.txt` | drop rxpower sources + `gs_rxpower_tests` target |

---

### Task 1: Types, keymap, path helpers, locked paths

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c` (keymap table, path helpers, `LOCKED_PATHS`, removal of `FPVD_T_RXPOWER` cases)
- Test: `tests/test_settings_fpvd.cpp`

This task changes the enums, so the old tests stop compiling — update the tests first to the new expectations, watch them fail, then change the header + tables. The integration test file will also stop compiling; to keep the loop tight, temporarily exclude it (step 3) and restore it in Task 4 when the runner is rewritten.

- [ ] **Step 1: Update unit tests to the new keymap/endpoint/lock expectations**

In `tests/test_settings_fpvd.cpp`:

Replace the `"lock: matches exact locked paths"` test body's txpower line:

```cpp
TEST_CASE("lock: matches exact locked paths", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.mcs") == true);
    REQUIRE(fpvd_is_locked_path("link.txPowerDbm") == true);
    REQUIRE(fpvd_is_locked_path("link.width") == true);
    REQUIRE(fpvd_is_locked_path("video.bitrate") == true);
    REQUIRE(fpvd_is_locked_path("video.qpDelta") == true);
    /* old key no longer exists in the schema */
    REQUIRE(fpvd_is_locked_path("link.txpower") == false);
}
```

Replace the whole `"endpoint: keymap entries carry the right endpoint + applyTo"` test:

```cpp
TEST_CASE("endpoint: keymap entries carry endpoint + row kind", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e->endpoint == FPVD_EP_AIR);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Shared link rows: GS endpoint, drone-first orchestration. */
    e = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_SHARED);
    REQUIRE(std::strcmp(e->path, "link.channel") == 0);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_SHARED);
    REQUIRE(std::strcmp(e->path, "link.width") == 0);

    /* GS card power: plain dBm int on the GS side. */
    e = fpvd_keymap_lookup("gs", "link", "rx_power");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txPowerDbm") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Drone TX power: renamed path, dBm. */
    e = fpvd_keymap_lookup("gs", "wfbng", "txpower");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txPowerDbm") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_AIR);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Beamforming toggle: client-owned handshake. */
    e = fpvd_keymap_lookup("gs", "link", "beamforming");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.beamforming.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_BEAMFORM);
}
```

Replace the two routing tests (`"endpoint: routing helpers pick air vs link paths"` and `"path helpers route EP_CONFIG to /config and /apply"`) with:

```cpp
TEST_CASE("endpoint: routing helpers map AIR and GS trees", "[fpvd][endpoint]") {
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_AIR), "/air/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_AIR), "/air/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(FPVD_EP_AIR),  "/air/config") == 0);

    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_GS), "/gs/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_GS), "/gs/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(FPVD_EP_GS),  "/gs/config?pending=true") == 0);
}
```

In `"keymap: pixelpilot rows route to EP_CONFIG + pixelpilot.* paths"`, rename the test and change every `FPVD_EP_CONFIG` to `FPVD_EP_GS`, and add a kind check:

```cpp
TEST_CASE("keymap: pixelpilot rows route to EP_GS as staged rows", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;
    e = fpvd_keymap_lookup("gs", "display", "video_scale");
    REQUIRE(e != nullptr);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_STAGED);
    REQUIRE(std::strcmp(e->path, "pixelpilot.videoScale") == 0);
    REQUIRE(e->type == FPVD_T_PERCENT_TO_FRAC);

    e = fpvd_keymap_lookup("gs", "display", "screen_mode");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "pixelpilot.screenMode") == 0);
    REQUIRE(e->type == FPVD_T_STRING);

    e = fpvd_keymap_lookup("gs", "dvr", "dvr_reenc_bitrate");
    REQUIRE(e != nullptr);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_STAGED);
    REQUIRE(std::strcmp(e->path, "pixelpilot.dvr.reencBitrate") == 0);

    e = fpvd_keymap_lookup("gs", "dvr", "dvr_osd");
    REQUIRE(std::strcmp(e->path, "pixelpilot.dvr.osd") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);

    REQUIRE(fpvd_keymap_lookup("gs", "display", "color_correction") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "rec_enabled") == nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -20'
```
Expected: compile errors — `FPVD_EP_GS`, `FPVD_ROW_PLAIN`, `e->kind` undeclared.

- [ ] **Step 3: Temporarily exclude the integration test from the build**

In `CMakeLists.txt`, in the `fpvd_tests` target source list, comment out the integration test (it is rewritten in Task 4):

```cmake
      tests/test_settings_fpvd.cpp)
      # tests/test_settings_fpvd_integration.cpp — re-enabled in the runner-rewrite task
```
(Keep `src/gsmenu/settings_gs_rxpower.c` in the target for now; it is removed in Task 6.)

- [ ] **Step 4: Rewrite the type definitions in `settings_fpvd_internal.h`**

Replace the `fpvd_type_t`, `fpvd_endpoint_t`, and `fpvd_keymap_entry_t` definitions with:

```c
typedef enum {
    FPVD_T_INT,
    FPVD_T_FLOAT,
    FPVD_T_BOOL,
    FPVD_T_STRING,
    FPVD_T_ENUM,
    FPVD_T_BITRATE_KBPS,    /* UI string "15M" ↔ JSON int 15000 */
    FPVD_T_SECONDS_FROM_MIN, /* UI int minutes ↔ JSON int seconds */
    FPVD_T_PERCENT_TO_FRAC,  /* UI int 0..100 ↔ JSON float 0.0..1.0 */
} fpvd_type_t;

typedef enum {
    FPVD_EP_AIR, /* drone proxy: /air/config + /air/apply,  GET /air/config */
    FPVD_EP_GS,  /* GS tree:     /gs/config  + /gs/apply,   GET /gs/config?pending=true */
} fpvd_endpoint_t;

typedef enum {
    FPVD_ROW_PLAIN,    /* PATCH + apply on the row's endpoint */
    FPVD_ROW_STAGED,   /* PATCH /gs/config only; explicit Apply commits */
    FPVD_ROW_SHARED,   /* drone-first cross-device orchestration (channel, width) */
    FPVD_ROW_BEAMFORM, /* beamforming MAC handshake (drone + GS) */
} fpvd_row_kind_t;

typedef struct {
    const char     *domain;
    const char     *page;
    const char     *key;
    const char     *path;
    fpvd_type_t     type;
    fpvd_endpoint_t endpoint;
    fpvd_row_kind_t kind;
} fpvd_keymap_entry_t;
```

Remove `#include "settings_gs_rxpower.h"` from `settings_fpvd.c` only if present in this header — it is included from the .c file; leave the .c include for now (removed in Task 6).

- [ ] **Step 5: Rewrite the keymap, path helpers, and locked paths in `settings_fpvd.c`**

Keymap: every existing `FPVD_EP_AIR` row keeps its line but the trailing `NULL` becomes `FPVD_ROW_PLAIN`. The link/pixelpilot rows change as follows (full replacement for those sections):

```c
    /* Link — shared radio (client-orchestrated: drone first, then GS) */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT, FPVD_EP_GS, FPVD_ROW_SHARED },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT, FPVD_EP_GS, FPVD_ROW_SHARED },

    /* Link — GS card power (dBm) and beamforming (client-owned handshake) */
    { "gs",  "link",  "rx_power",    "link.txPowerDbm",         FPVD_T_INT,  FPVD_EP_GS, FPVD_ROW_PLAIN },
    { "gs",  "link",  "beamforming", "link.beamforming.enabled",FPVD_T_BOOL, FPVD_EP_GS, FPVD_ROW_BEAMFORM },

    /* Link — drone TX power (dBm) + modulation (drone-owned) */
    { "gs",  "wfbng", "txpower",    "link.txPowerDbm", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    { "air", "wfbng", "mcs_index",  "link.mcs",        FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
    /* ... stbc/ldpc/fec_k/fec_n likewise with FPVD_ROW_PLAIN ... */
```

PixelPilot rows: change `FPVD_EP_CONFIG, NULL` → `FPVD_EP_GS, FPVD_ROW_STAGED` on all 11 rows.

Path helpers:

```c
const char *fpvd_write_path(fpvd_endpoint_t ep) {
    return ep == FPVD_EP_GS ? "/gs/config" : "/air/config";
}
const char *fpvd_apply_path(fpvd_endpoint_t ep) {
    return ep == FPVD_EP_GS ? "/gs/apply" : "/air/apply";
}
const char *fpvd_read_path(fpvd_endpoint_t ep) {
    return ep == FPVD_EP_GS ? "/gs/config?pending=true" : "/air/config";
}
```

`LOCKED_PATHS`: change `"link.txpower"` → `"link.txPowerDbm"`.

Remove the `FPVD_T_RXPOWER` cases from `fpvd_snapshot_read_string()` and `value_to_cjson()`.

**Compile stopgaps for this task only** (the runner is rewritten in Task 4; keep the file compiling now):
- In `fpvd_job_t`, replace `char apply_to[8];` with `fpvd_row_kind_t kind;`; in `enqueue_locked()` replace the `apply_to` copy with `j->kind = e->kind;`.
- In `run_job_unlocked()`: delete the `FPVD_T_RXPOWER` branch of the PATCH-body build (keep only the plain `fpvd_build_patch_body(job.path, job.value, job.type)` call); replace the `job.endpoint == FPVD_EP_CONFIG` stage-only check with `job.kind == FPVD_ROW_STAGED`; replace the `FPVD_EP_LINK` applyTo body branch with a plain `r = fpvd_http_post(apply_url);` for all endpoints.
- In `refresh_snapshot_unlocked()`: change the three GETs to `/gs/config?pending=true` (into `G.gs_snapshot`, no `{"link":...}` wrapping — store the parsed body directly) and `/air/config` (into `G.air_snapshot`); delete the `config_snapshot` GET and field uses by pointing the old `config_snapshot` reads at `G.gs_snapshot` (in `prov_get`, the `FPVD_EP_CONFIG` switch case disappears: GS rows read `G.gs_snapshot`, AIR rows read `G.air_snapshot`). Remove `cJSON *config_snapshot` from the struct and its cleanup in `pp_settings_register_fpvd`.
- In `prov_get()`: drop the `FPVD_T_RXPOWER` branch entirely (plain `fpvd_snapshot_read_string` for all rows; the null-fallback comes in Task 4).
- In `prov_apply()`: change `j->endpoint = FPVD_EP_CONFIG;` to `j->endpoint = FPVD_EP_GS;`.
- In `prov_is_locked()`: unchanged logic (the bandwidth exception still works — `link.width` is still a locked path).

- [ ] **Step 6: Build and run unit tests**

```bash
nix-shell shell-sim.nix --run 'cmake build-sim && cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -5'
./build-sim/fpvd_tests '[keymap],[endpoint],[lock],[snapshot],[patch]'
```
Expected: PASS (all).

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/settings_fpvd_internal.h src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp CMakeLists.txt
git commit -m "feat(gsmenu): remap fpvd provider to /gs tree, row kinds, txPowerDbm paths"
```

---

### Task 2: Step planner (pure, unit-tested)

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h` (declarations)
- Modify: `src/gsmenu/settings_fpvd.c` (implementation)
- Test: `tests/test_settings_fpvd.cpp`

The planner converts (row kind, endpoint, path, type, value, drone reachability, GS MAC) into an ordered list of HTTP steps. The runner (Task 4) just executes the list. This makes the orchestration sequencing testable without HTTP.

- [ ] **Step 1: Add declarations to `settings_fpvd_internal.h`**

```c
#define FPVD_PLAN_MAX 6

typedef struct {
    char method[8];    /* "PATCH" | "POST" */
    char url_path[28]; /* e.g. "/gs/config" */
    char body[256];    /* "" => no body */
    int  retries;      /* extra attempts after first failure (0 = single try) */
    bool gs_side;      /* step targets the GS tree (for error wording + dirty flag) */
} fpvd_step_t;

/* Plan the HTTP steps for one settings write. Pure (no HTTP, no globals).
 * gs_local_mac may be NULL (only needed for FPVD_ROW_BEAMFORM enable).
 * Returns the number of steps written to out, or -1 with a message in err. */
int fpvd_plan_steps(fpvd_row_kind_t kind, fpvd_endpoint_t ep,
                    const char *path, fpvd_type_t type, const char *value,
                    bool drone_reachable, const char *gs_local_mac,
                    fpvd_step_t *out, size_t max,
                    char *err, size_t errn);
```

- [ ] **Step 2: Write the failing planner tests**

Append to `tests/test_settings_fpvd.cpp`:

```cpp
static int plan(fpvd_row_kind_t kind, fpvd_endpoint_t ep, const char *path,
                fpvd_type_t type, const char *value, bool reachable,
                const char *mac, fpvd_step_t *steps, char *err) {
    return fpvd_plan_steps(kind, ep, path, type, value, reachable, mac,
                           steps, FPVD_PLAN_MAX, err, 160);
}

TEST_CASE("plan: plain AIR row is patch+apply on /air", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT, "90",
                 true, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].method, "PATCH") == 0);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"video":{"fps":90}})");
    REQUIRE(s[0].gs_side == false);
    REQUIRE(std::strcmp(s[1].method, "POST") == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(s[1].body[0] == '\0');
}

TEST_CASE("plan: plain AIR row rejected when drone unreachable", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT, "90",
                 false, nullptr, s, err);
    REQUIRE(n == -1);
    REQUIRE(std::string(err) == "Drone unreachable");
}

TEST_CASE("plan: plain GS row is patch+apply on /gs", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_GS, "link.txPowerDbm", FPVD_T_INT, "25",
                 false /* GS rows work regardless */, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"link":{"txPowerDbm":25}})");
    REQUIRE(s[0].gs_side == true);
    REQUIRE(std::strcmp(s[1].url_path, "/gs/apply") == 0);
    REQUIRE(s[1].gs_side == true);
}

TEST_CASE("plan: staged row is a single GS patch", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_STAGED, FPVD_EP_GS, "pixelpilot.dvr.osd", FPVD_T_BOOL,
                 "on", true, nullptr, s, err);
    REQUIRE(n == 1);
    REQUIRE(std::strcmp(s[0].method, "PATCH") == 0);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"pixelpilot":{"dvr":{"osd":true}}})");
}

TEST_CASE("plan: shared row online is drone-first, GS retried", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_SHARED, FPVD_EP_GS, "link.channel", FPVD_T_INT, "100",
                 true, nullptr, s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"link":{"channel":100}})");
    REQUIRE(s[0].retries == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"link":{"channel":100}})");
    REQUIRE(s[2].retries == 3);
    REQUIRE(s[2].gs_side == true);
    REQUIRE(std::strcmp(s[3].url_path, "/gs/apply") == 0);
    REQUIRE(s[3].retries == 3);
}

TEST_CASE("plan: shared row offline degrades to GS-only", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_SHARED, FPVD_EP_GS, "link.channel", FPVD_T_INT, "100",
                 false, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/gs/apply") == 0);
}

TEST_CASE("plan: beamforming enable carries remoteMac and stbc=false", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", true, "84:fc:14:6c:36:e6", s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) ==
        R"({"link":{"beamforming":{"enabled":true,"remoteMac":"84:fc:14:6c:36:e6"},"stbc":false}})");
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"link":{"beamforming":{"enabled":true}}})");
    REQUIRE(std::strcmp(s[3].url_path, "/gs/apply") == 0);
}

TEST_CASE("plan: beamforming disable restores stbc", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "off", true, "84:fc:14:6c:36:e6", s, err);
    REQUIRE(n == 4);
    REQUIRE(std::string(s[0].body) ==
        R"({"link":{"beamforming":{"enabled":false},"stbc":true}})");
    REQUIRE(std::string(s[2].body) == R"({"link":{"beamforming":{"enabled":false}}})");
}

TEST_CASE("plan: beamforming rejected offline or without MAC", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    REQUIRE(plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", false, "aa:bb:cc:dd:ee:ff", s, err) == -1);
    REQUIRE(std::string(err) == "Drone unreachable");
    err[0] = '\0';
    REQUIRE(plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", true, nullptr, s, err) == -1);
    REQUIRE(std::string(err) == "GS card MAC unknown");
}

TEST_CASE("plan: bad value yields error not steps", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    REQUIRE(plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT,
                 "notanint", true, nullptr, s, err) == -1);
    REQUIRE(err[0] != '\0');
}
```

- [ ] **Step 3: Run tests to verify they fail**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -5'
```
Expected: link error — `fpvd_plan_steps` undefined.

- [ ] **Step 4: Implement the planner in `settings_fpvd.c`**

Place after `fpvd_is_locked_path()`:

```c
#define FPVD_GS_APPLY_RETRIES 3

static void step_init(fpvd_step_t *s, const char *method, const char *url,
                      const char *body, int retries, bool gs_side) {
    memset(s, 0, sizeof *s);
    snprintf(s->method, sizeof s->method, "%s", method);
    snprintf(s->url_path, sizeof s->url_path, "%s", url);
    if (body) snprintf(s->body, sizeof s->body, "%s", body);
    s->retries = retries;
    s->gs_side = gs_side;
}

/* Serialize a one-field patch body for `path`=`value` into buf. */
static bool patch_body_str(const char *path, const char *value,
                           fpvd_type_t type, char *buf, size_t n) {
    cJSON *body = fpvd_build_patch_body(path, value, type);
    if (!body) return false;
    char *s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!s) return false;
    snprintf(buf, n, "%s", s);
    free(s);
    return true;
}

int fpvd_plan_steps(fpvd_row_kind_t kind, fpvd_endpoint_t ep,
                    const char *path, fpvd_type_t type, const char *value,
                    bool drone_reachable, const char *gs_local_mac,
                    fpvd_step_t *out, size_t max,
                    char *err, size_t errn) {
    char body[256];
    if (max < 4) { snprintf(err, errn, "Plan buffer too small"); return -1; }

    switch (kind) {
    case FPVD_ROW_PLAIN: {
        if (ep == FPVD_EP_AIR && !drone_reachable) {
            snprintf(err, errn, "Drone unreachable"); return -1;
        }
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        bool gs = (ep == FPVD_EP_GS);
        step_init(&out[0], "PATCH", fpvd_write_path(ep), body, 0, gs);
        step_init(&out[1], "POST",  fpvd_apply_path(ep), NULL, 0, gs);
        return 2;
    }
    case FPVD_ROW_STAGED: {
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        step_init(&out[0], "PATCH", "/gs/config", body, 0, true);
        return 1;
    }
    case FPVD_ROW_SHARED: {
        if (!patch_body_str(path, value, type, body, sizeof body)) {
            snprintf(err, errn, "Invalid value"); return -1;
        }
        size_t n = 0;
        if (drone_reachable) {
            /* Drone first: the GS then retunes onto the link the drone has
             * already moved to (api.md, client orchestration). */
            step_init(&out[n++], "PATCH", "/air/config", body, 0, false);
            step_init(&out[n++], "POST",  "/air/apply",  NULL, 0, false);
        }
        step_init(&out[n++], "PATCH", "/gs/config", body, FPVD_GS_APPLY_RETRIES, true);
        step_init(&out[n++], "POST",  "/gs/apply",  NULL, FPVD_GS_APPLY_RETRIES, true);
        return (int)n;
    }
    case FPVD_ROW_BEAMFORM: {
        if (!drone_reachable) { snprintf(err, errn, "Drone unreachable"); return -1; }
        bool enable = (strcmp(value, "on") == 0 || strcmp(value, "true") == 0);
        if (enable && (!gs_local_mac || !gs_local_mac[0])) {
            snprintf(err, errn, "GS card MAC unknown"); return -1;
        }
        /* STBC and TX-beamforming are mutually exclusive on the drone:
         * disable stbc when enabling BF, restore it when disabling. */
        if (enable) {
            snprintf(body, sizeof body,
                "{\"link\":{\"beamforming\":{\"enabled\":true,\"remoteMac\":\"%s\"},"
                "\"stbc\":false}}", gs_local_mac);
        } else {
            snprintf(body, sizeof body,
                "{\"link\":{\"beamforming\":{\"enabled\":false},\"stbc\":true}}");
        }
        step_init(&out[0], "PATCH", "/air/config", body, 0, false);
        step_init(&out[1], "POST",  "/air/apply",  NULL, 0, false);
        snprintf(body, sizeof body,
            "{\"link\":{\"beamforming\":{\"enabled\":%s}}}", enable ? "true" : "false");
        step_init(&out[2], "PATCH", "/gs/config", body, FPVD_GS_APPLY_RETRIES, true);
        step_init(&out[3], "POST",  "/gs/apply",  NULL, FPVD_GS_APPLY_RETRIES, true);
        return 4;
    }
    }
    snprintf(err, errn, "Unknown row kind");
    return -1;
}
```

- [ ] **Step 5: Build and run the plan tests**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3'
./build-sim/fpvd_tests '[plan]'
```
Expected: PASS (10 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd_internal.h src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp
git commit -m "feat(gsmenu): pure step planner for drone-first and beamforming orchestration"
```

---

### Task 3: Dispatcher + helper reachability API

**Files:**
- Modify: `src/gsmenu/settings.h`, `src/gsmenu/settings.c`, `src/gsmenu/helper.c`
- Test: `tests/test_settings.cpp`

- [ ] **Step 1: Write the failing dispatcher test**

Append to `tests/test_settings.cpp`:

```cpp
TEST_CASE("dispatcher: is_reachable defaults to true without provider method", "[caps]") {
    /* The dummy provider (registered by other tests) has no is_reachable
     * until the dummy task lands; the dispatcher must default to true. */
    pp_settings_register_dummy();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);
}
```

- [ ] **Step 2: Run to verify it fails to compile**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests -j 2>&1 | tail -5'
```
Expected: `pp_settings_is_reachable` undeclared.

- [ ] **Step 3: Implement**

`settings.h` — add to `pp_settings_provider_t` after `is_connected`:

```c
    /* Optional: returns true if the device backing this key is reachable
     * (e.g. the drone for /air-backed rows). NULL → dispatcher returns true.
     * Distinct from is_connected, which covers the provider's own backend. */
    bool  (*is_reachable)(const char *domain, const char *page, const char *key);
```

and after `pp_settings_is_connected`:

```c
bool  pp_settings_is_reachable(const char *domain, const char *page,
                               const char *key);
```

`settings.c` — after `pp_settings_is_connected`:

```c
bool pp_settings_is_reachable(const char *d, const char *p, const char *k) {
    if (g_provider && g_provider->is_reachable) {
        return g_provider->is_reachable(d, p, k);
    }
    return true;
}
```

`helper.c` — in `pp_page_reapply_lock_state`, change the offline branch:

```c
        } else if (!connected || !pp_settings_is_reachable(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_OFFLINE);
        } else if (pp_settings_is_locked(h->d, h->p, h->k)) {
```

- [ ] **Step 4: Build and run**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests -j 2>&1 | tail -3'
./build-sim/settings_tests '[caps]'
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings.h src/gsmenu/settings.c src/gsmenu/helper.c tests/test_settings.cpp
git commit -m "feat(gsmenu): per-row reachability API; offline rows keyed to backing device"
```

---

### Task 4: Provider core — reachability state, /gs/status poll, runner rewrite

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `CMakeLists.txt` (re-enable integration test)
- Test: `tests/test_settings_fpvd_integration.cpp` (rewrite)

This is the biggest task: replace the two-phase `run_job_unlocked` with a plan executor, add the `/gs/status` poll, split connectivity flags, and implement `is_reachable` + the txpower null fallback.

- [ ] **Step 1: Rewrite the integration test mock server and tests**

Replace the mock-server portion of `tests/test_settings_fpvd_integration.cpp` (the `GsMockServer` struct and routes) with the new API shape. Keep the existing helpers (`ensure_lv_init`, env setup, any wait/poll utilities already in the file — reuse them as-is). New server:

```cpp
/* Mock fpvd-GS: /gs/config, /gs/apply, /gs/status, and the /air/* proxy. */
struct GsMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;

    std::mutex mu;
    std::vector<std::string> log;   /* "METHOD path" in arrival order */
    std::string last_gs_patch_body, last_air_patch_body;

    std::atomic<bool> drone_up{true};      /* false => /air/* returns 502 */
    std::atomic<int>  gs_apply_fail_n{0};  /* fail this many /gs/apply calls */

    std::string gs_config_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":25,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";
    std::string gs_status_response =
        R"({"fpvd":{"version":"0.1.0","uptimeMs":1000},)"
        R"("runner":{"running":true,"pid":1,"restarts":0},)"
        R"("radio":[{"wlan":"wlx0","type":"monitor","channel":132,)"
        R"("freqMhz":5660,"widthMhz":40,"txpowerDbm":19.0}],)"
        R"("link":{"linkId":7669206,"droneReachable":true},)"
        R"("beamforming":{"enabled":false,"localMac":"84:fc:14:6c:36:e6"}})";
    std::string air_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":20,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":true,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";

    void note(const char *m, const std::string &p) {
        std::lock_guard<std::mutex> g(mu);
        log.push_back(std::string(m) + " " + p);
    }
    /* Status string with droneReachable patched to match drone_up. */
    std::string status_now() {
        std::string s = gs_status_response;
        if (!drone_up) {
            auto pos = s.find("\"droneReachable\":true");
            if (pos != std::string::npos)
                s.replace(pos, strlen("\"droneReachable\":true"),
                          "\"droneReachable\":false");
        }
        return s;
    }

    void start() {
        svr.Get("/gs/config", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/gs/config");
            res.set_content(gs_config_response, "application/json");
        });
        svr.Patch("/gs/config", [&](const httplib::Request &req, httplib::Response &res) {
            note("PATCH", "/gs/config");
            { std::lock_guard<std::mutex> g(mu); last_gs_patch_body = req.body; }
            res.set_content(gs_config_response, "application/json");
        });
        svr.Post("/gs/apply", [&](const httplib::Request &, httplib::Response &res) {
            note("POST", "/gs/apply");
            if (gs_apply_fail_n > 0) {
                gs_apply_fail_n--;
                res.status = 500;
                res.set_content(R"({"applied":false,"error":"runner failed; rolled back"})",
                                "application/json");
                return;
            }
            res.set_content(R"({"applied":true})", "application/json");
        });
        svr.Get("/gs/status", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/gs/status");
            res.set_content(status_now(), "application/json");
        });
        svr.Get("/air/config", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/air/config");
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            res.set_content(air_response, "application/json");
        });
        svr.Patch("/air/config", [&](const httplib::Request &req, httplib::Response &res) {
            note("PATCH", "/air/config");
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            { std::lock_guard<std::mutex> g(mu); last_air_patch_body = req.body; }
            res.set_content(air_response, "application/json");
        });
        svr.Post("/air/apply", [&](const httplib::Request &, httplib::Response &res) {
            note("POST", "/air/apply");
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            res.set_content(R"({"applied":true,"version":2,"restarted":["radio"]})",
                            "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([&] { svr.listen_after_bind(); });
        while (!svr.is_running())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void stop() { svr.stop(); if (th.joinable()) th.join(); }
    std::vector<std::string> snapshot_log() {
        std::lock_guard<std::mutex> g(mu); return log;
    }
};
```

Keep/adapt the existing test fixture that sets `PP_FPVD_URL` to `http://127.0.0.1:<port>` and calls `pp_settings_register_fpvd()`, plus the existing done-callback wait helper (a `std::atomic<int>` rc + polling `lv_timer_handler()`; reuse whatever pattern the current file uses for waiting on async sets — keep it identical).

Then write these test cases (replacing the old `/link` ones; keep any old cases that only exercise plain AIR rows, updating their asserted URLs):

```cpp
TEST_CASE("integration: shared channel change is drone-first then GS", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    /* fixture: setenv PP_FPVD_URL, pp_settings_register_fpvd() */
    ...register provider against srv.port...

    ...async set ("gs","wfbng","gs_channel") = "100", wait for rc == 0...

    auto log = srv.snapshot_log();
    /* Filter out polling GETs; assert write ordering. */
    std::vector<std::string> writes;
    for (auto &l : log)
        if (l.rfind("GET", 0) != 0) writes.push_back(l);
    REQUIRE(writes.size() == 4);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(writes[1] == "POST /air/apply");
    REQUIRE(writes[2] == "PATCH /gs/config");
    REQUIRE(writes[3] == "POST /gs/apply");
    REQUIRE(srv.last_air_patch_body == R"({"link":{"channel":100}})");
    REQUIRE(srv.last_gs_patch_body  == R"({"link":{"channel":100}})");
    srv.stop();
}

TEST_CASE("integration: shared change degrades to GS-only when drone down", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();
    ...register provider, wait one poll round so drone_reachable latches false...

    ...async set ("gs","wfbng","gs_channel") = "108", wait for rc == 0...

    auto log = srv.snapshot_log();
    std::vector<std::string> writes;
    for (auto &l : log)
        if (l.rfind("GET", 0) != 0) writes.push_back(l);
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0] == "PATCH /gs/config");
    REQUIRE(writes[1] == "POST /gs/apply");
    srv.stop();
}

TEST_CASE("integration: GS apply failure is retried then succeeds", "[fpvd][network]") {
    GsMockServer srv; srv.gs_apply_fail_n = 1; srv.start();
    ...register provider...
    ...async set ("gs","wfbng","gs_channel") = "112", wait rc == 0...
    /* Two POST /gs/apply entries: the failed first attempt and the retry. */
    int applies = 0;
    for (auto &l : srv.snapshot_log()) if (l == "POST /gs/apply") applies++;
    REQUIRE(applies == 2);
    srv.stop();
}

TEST_CASE("integration: beamforming enable sends MAC handshake", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    ...register provider, wait one poll round (localMac comes from /gs/status)...
    ...async set ("gs","link","beamforming") = "on", wait rc == 0...
    REQUIRE(srv.last_air_patch_body ==
        R"({"link":{"beamforming":{"enabled":true,"remoteMac":"84:fc:14:6c:36:e6"},"stbc":false}})");
    REQUIRE(srv.last_gs_patch_body ==
        R"({"link":{"beamforming":{"enabled":true}}})");
    srv.stop();
}

TEST_CASE("integration: beamforming rejected while drone down", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();
    ...register provider, wait one poll round...
    ...async set ("gs","link","beamforming") = "on", expect rc != 0
       and err == "Drone unreachable"; assert no PATCH in srv log...
    srv.stop();
}

TEST_CASE("integration: staged pixelpilot row patches /gs/config without apply", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    ...register provider...
    ...async set ("gs","dvr","dvr_osd") = "off", wait rc == 0...
    REQUIRE(pp_settings_has_pending() == true);
    for (auto &l : srv.snapshot_log()) REQUIRE(l != "POST /gs/apply");
    /* Explicit apply commits and clears pending. */
    ...pp_settings_apply(...), wait rc == 0...
    REQUIRE(pp_settings_has_pending() == false);
    srv.stop();
}

TEST_CASE("integration: rx_power reads dBm, null falls back to status radio", "[fpvd][network]") {
    GsMockServer srv;
    srv.gs_config_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":null,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{}})";
    srv.start();
    ...register provider (primes snapshots synchronously)...
    char *v = pp_settings_get("gs", "link", "rx_power");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "19");   /* radio[0].txpowerDbm 19.0 rounded */
    free(v);
    srv.stop();
}
```

The `...` sections are the file's existing register/wait plumbing — copy it from the current test bodies (it sets `PP_FPVD_URL` via `setenv`, calls `pp_settings_register_fpvd()`, uses an atomic + `lv_timer_handler()` loop to wait for the done callback). Preserve that code verbatim; only the server, routes, and assertions change. **Important (from build memory):** the fpvd job queue is a static array with reused slots and tests run in one process — every test must re-register the provider (`pp_settings_register_fpvd()` resets queue/snapshots) and must not assume server state from a prior test.

- [ ] **Step 2: Re-enable the integration test in CMake**

In `CMakeLists.txt`, restore the line in `fpvd_tests`:

```cmake
      tests/test_settings_fpvd.cpp
      tests/test_settings_fpvd_integration.cpp)
```

- [ ] **Step 3: Build to verify the new tests fail**

```bash
nix-shell shell-sim.nix --run 'cmake build-sim && cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -10'
```
Expected: compiles (or fails on missing behavior) — then `./build-sim/fpvd_tests '[network]'` FAILS (old runner still posts to old routes / no status poll / no drone_reachable gating).

- [ ] **Step 4: Rewrite the provider state and refresh**

In `settings_fpvd.c`, update `fpvd_state_t`:

```c
    bool     gs_connected;      /* fpvd-GS HTTP round-trips succeed */
    bool     drone_reachable;   /* /gs/status link.droneReachable, /air corrected */

    cJSON   *air_snapshot;      /* GET /air/config (drone), protected by mu */
    cJSON   *gs_snapshot;       /* GET /gs/config?pending=true (full GS tree) */
    cJSON   *status_snapshot;   /* GET /gs/status (radio, droneReachable, localMac) */
    bool     config_dirty;      /* staged-but-unapplied /gs/config changes */
```

(Remove `connected` and `config_snapshot`; update every reference: `worker_main` poll cadence uses `G.gs_connected`, `prov_is_connected` returns `G.gs_connected`, registration resets all three snapshots and both flags.)

Replace `refresh_snapshot_unlocked()`:

```c
/* Called with G.mu HELD. Releases and re-acquires the mutex around HTTP.
 * gs_connected tracks the GS daemon itself; drone_reachable comes from
 * /gs/status and is corrected by the /air/config result (200 → true,
 * 502/transport → false, leaving air_snapshot stale rather than cleared). */
static void refresh_snapshot_unlocked(void) {
    char *gs_url     = url_join(G.base_url, "/gs/config?pending=true");
    char *status_url = url_join(G.base_url, "/gs/status");
    char *air_url    = url_join(G.base_url, "/air/config");
    if (!gs_url || !status_url || !air_url) {
        free(gs_url); free(status_url); free(air_url);
        G.gs_connected = false; return;
    }
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t gr = fpvd_http_get(gs_url);
    fpvd_http_result_t sr = fpvd_http_get(status_url);
    fpvd_http_result_t ar = fpvd_http_get(air_url);
    pthread_mutex_lock(&G.mu);
    free(gs_url); free(status_url); free(air_url);

    bool was_gs = G.gs_connected, was_drone = G.drone_reachable;

    if (gr.status == 200 && gr.body) {
        cJSON *g = cJSON_Parse(gr.body);
        if (g) { if (G.gs_snapshot) cJSON_Delete(G.gs_snapshot); G.gs_snapshot = g; }
        G.gs_connected = true;
    } else {
        G.gs_connected = false;
    }
    if (sr.status == 200 && sr.body) {
        cJSON *s = cJSON_Parse(sr.body);
        if (s) {
            if (G.status_snapshot) cJSON_Delete(G.status_snapshot);
            G.status_snapshot = s;
            cJSON *link = cJSON_GetObjectItemCaseSensitive(s, "link");
            cJSON *dr   = link ? cJSON_GetObjectItemCaseSensitive(link, "droneReachable") : NULL;
            if (dr && cJSON_IsBool(dr)) G.drone_reachable = cJSON_IsTrue(dr);
        }
    }
    if (ar.status == 200 && ar.body) {
        cJSON *a = cJSON_Parse(ar.body);
        if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
        G.drone_reachable = true;
    } else if (ar.status == 502 || ar.status == 0) {
        G.drone_reachable = false;
    }
    fpvd_http_result_free(&gr);
    fpvd_http_result_free(&sr);
    fpvd_http_result_free(&ar);
    if (was_gs != G.gs_connected || was_drone != G.drone_reachable || G.gs_connected)
        notify_listener();
}
```

Update `parse_error_message()` so the GS error shape (`{"error":"<human text>"}` with no `message`/code semantics) surfaces the text: in the final `else` branch, when there is no `message` field, fall back to the `error` string itself:

```c
    } else {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "message");
        if (m && cJSON_IsString(m)) snprintf(buf, sizeof buf, "%s", m->valuestring);
        else if (err && cJSON_IsString(err) && err->valuestring[0])
            snprintf(buf, sizeof buf, "%s", err->valuestring);
        else snprintf(buf, sizeof buf, "Request rejected");
    }
```

- [ ] **Step 5: Rewrite `run_job_unlocked` as a plan executor**

```c
/* Mutex must be RELEASED on entry. */
static void run_job_unlocked(fpvd_job_t job) {
    int rc = 0;
    char err[160] = {0};
    fpvd_step_t steps[FPVD_PLAN_MAX];
    int nsteps;
    bool air_committed = false, gs_applied = false;

    if (job.apply_only) {
        step_init(&steps[0], "POST", "/gs/apply", NULL, 0, true);
        nsteps = 1;
    } else {
        pthread_mutex_lock(&G.mu);
        bool reachable = G.drone_reachable;
        char mac[24] = {0};
        cJSON *bf = G.status_snapshot ?
            cJSON_GetObjectItemCaseSensitive(G.status_snapshot, "beamforming") : NULL;
        cJSON *lm = bf ? cJSON_GetObjectItemCaseSensitive(bf, "localMac") : NULL;
        if (lm && cJSON_IsString(lm))
            snprintf(mac, sizeof mac, "%s", lm->valuestring);
        pthread_mutex_unlock(&G.mu);

        nsteps = fpvd_plan_steps(job.kind, job.endpoint, job.path, job.type,
                                 job.value, reachable, mac[0] ? mac : NULL,
                                 steps, FPVD_PLAN_MAX, err, sizeof err);
        if (nsteps < 0) { rc = -1; goto done; }
        if (job.kind == FPVD_ROW_SHARED && !reachable)
            fprintf(stderr, "fpvd: %s applied to GS only (drone unreachable)\n", job.path);
    }

    for (int i = 0; i < nsteps && rc == 0; i++) {
        char *url = url_join(G.base_url, steps[i].url_path);
        if (!url) { rc = -1; snprintf(err, sizeof err, "Out of memory"); break; }
        fpvd_http_result_t r = { 0, NULL, 0 };
        for (int a = 0; a <= steps[i].retries; a++) {
            if (a > 0) {
                struct timespec b = { 0, 500 * 1000 * 1000 };
                nanosleep(&b, NULL);
            }
            fpvd_http_result_free(&r);
            if (strcmp(steps[i].method, "PATCH") == 0)
                r = fpvd_http_patch_json(url, steps[i].body);
            else if (steps[i].body[0])
                r = fpvd_http_post_json(url, steps[i].body);
            else
                r = fpvd_http_post(url);
            if (r.status >= 200 && r.status < 300) break;
        }
        free(url);

        if (r.status >= 200 && r.status < 300) {
            if (strcmp(steps[i].url_path, "/air/apply") == 0) air_committed = true;
            if (strcmp(steps[i].url_path, "/gs/apply")  == 0) gs_applied = true;
        } else {
            rc = -1;
            const char *m = NULL;
            if (r.status == 0) {
                m = "GS unreachable";
                pthread_mutex_lock(&G.mu);
                bool was = G.gs_connected; G.gs_connected = false;
                pthread_mutex_unlock(&G.mu);
                if (was) notify_listener();
            } else {
                m = parse_error_message(r.body);
                if (r.status == 502) {
                    pthread_mutex_lock(&G.mu);
                    bool was = G.drone_reachable; G.drone_reachable = false;
                    pthread_mutex_unlock(&G.mu);
                    if (was) notify_listener();
                    if (!m) m = "Drone unreachable";
                }
            }
            if (air_committed && steps[i].gs_side)
                snprintf(err, sizeof err, "Drone updated; GS apply failed: %s",
                         m ? m : "error");
            else
                snprintf(err, sizeof err, "%s", m ? m : "Request failed");
        }
        fpvd_http_result_free(&r);
    }

    if (rc == 0) {
        pthread_mutex_lock(&G.mu);
        if (job.kind == FPVD_ROW_STAGED && !job.apply_only) G.config_dirty = true;
        if (gs_applied) G.config_dirty = false;   /* any /gs/apply commits staged changes */
        refresh_snapshot_unlocked();
        pthread_mutex_unlock(&G.mu);
    }

done:
    schedule_done(job.on_done, job.user_data, rc, err[0] ? err : NULL);
}
```

(Delete the old PATCH/apply phases, the `patch_url/apply_url/body_s` locals, and the rxpower conversion branch. `step_init` from Task 2 is reused — move it above this function if needed.)

- [ ] **Step 6: Add `prov_is_reachable` and the txpower null fallback**

```c
static bool prov_is_reachable(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return true;
    /* AIR rows and the beamforming handshake need the drone; GS rows —
     * including SHARED channel/width (the offline recovery path) — do not. */
    if (e->endpoint != FPVD_EP_AIR && e->kind != FPVD_ROW_BEAMFORM) return true;
    pthread_mutex_lock(&G.mu);
    bool r = G.drone_reachable;
    pthread_mutex_unlock(&G.mu);
    return r;
}
```

Wire into the vtable: `.is_reachable = prov_is_reachable,` in `G_PROVIDER`.

In `prov_get()`, after the generic read, add the fallback (inside the mutex):

```c
    out = snap ? fpvd_snapshot_read_string(snap, e->path, e->type) : strdup("");
    /* GS txPowerDbm may be null (driver default): fall back to the live
     * radio power reported by /gs/status. */
    if (out[0] == '\0' && e->endpoint == FPVD_EP_GS &&
        strcmp(e->path, "link.txPowerDbm") == 0 && G.status_snapshot) {
        cJSON *radio = cJSON_GetObjectItemCaseSensitive(G.status_snapshot, "radio");
        cJSON *first = (radio && cJSON_IsArray(radio)) ? cJSON_GetArrayItem(radio, 0) : NULL;
        cJSON *tx    = first ? cJSON_GetObjectItemCaseSensitive(first, "txpowerDbm") : NULL;
        if (tx && cJSON_IsNumber(tx)) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", (int)(tx->valuedouble + 0.5));
            free(out); out = strdup(buf);
        }
    }
```

Also simplify `prov_get`'s snapshot switch to two cases (`FPVD_EP_GS` → `G.gs_snapshot`, else `G.air_snapshot`) and delete the whole `FPVD_T_RXPOWER` branch.

In `pp_settings_register_fpvd()`, reset the new fields:

```c
    G.gs_connected   = false;
    G.drone_reachable = false;
    if (G.status_snapshot) { cJSON_Delete(G.status_snapshot); G.status_snapshot = NULL; }
```

- [ ] **Step 7: Build and run the full fpvd suite**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3'
./build-sim/fpvd_tests
```
Expected: PASS (unit + `[network]`). Integration tests take a few seconds each.

- [ ] **Step 8: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp CMakeLists.txt
git commit -m "feat(gsmenu): client-orchestrated apply via /gs tree; drone reachability split"
```

---

### Task 5: Delete the rxpower percent mapping module

**Files:**
- Delete: `src/gsmenu/settings_gs_rxpower.c`, `src/gsmenu/settings_gs_rxpower.h`, `tests/test_settings_gs_rxpower.cpp`
- Modify: `CMakeLists.txt`, `src/gsmenu/settings_fpvd.c` (drop the `#include`)

- [ ] **Step 1: Remove the include and the files**

In `settings_fpvd.c` delete the line `#include "settings_gs_rxpower.h"`. Then:

```bash
git rm src/gsmenu/settings_gs_rxpower.c src/gsmenu/settings_gs_rxpower.h tests/test_settings_gs_rxpower.cpp
```

- [ ] **Step 2: Update CMakeLists.txt**

Remove `src/gsmenu/settings_gs_rxpower.c` from the `fpvd_tests` source list and delete the whole `gs_rxpower_tests` target block (the `add_executable(gs_rxpower_tests ...)`, its `target_include_directories`, and `target_link_libraries`). Also check the device/simulator main-binary source lists for `settings_gs_rxpower.c` (`grep -n rxpower CMakeLists.txt`) and remove every occurrence.

- [ ] **Step 3: Verify clean build of tests and simulator**

```bash
nix-shell shell-sim.nix --run 'cmake build-sim && cmake --build build-sim --target fpvd_tests -j 2>&1 | tail -3'
./build-sim/fpvd_tests '[keymap]'
nix-shell shell-sim.nix --run 'cmake --build build-sim --target pixelpilot -j 2>&1 | tail -3'
```
Expected: both build clean; keymap tests PASS.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): drop percent-to-mBm rxpower mapping (txPowerDbm is plain dBm)"
```

---

### Task 6: Dummy provider — dBm seeds, beamforming row, drone-offline sim

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`
- Test: `tests/test_settings.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_settings.cpp`:

```cpp
TEST_CASE("dummy: txpower seeds are dBm and beamforming row exists", "[dummy]") {
    pp_settings_register_dummy();
    char *v = pp_settings_get("gs", "wfbng", "txpower");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "20"); free(v);
    v = pp_settings_get("gs", "link", "rx_power");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "20"); free(v);
    v = pp_settings_get("gs", "link", "beamforming");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "off"); free(v);
}

TEST_CASE("dummy: PP_SIM_DRONE_OFFLINE gates air rows and beamforming", "[dummy]") {
    setenv("PP_SIM_DRONE_OFFLINE", "1", 1);
    pp_settings_register_dummy();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);
    REQUIRE(pp_settings_is_reachable("gs", "link", "beamforming") == false);
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "txpower") == false); /* drone TX power */
    /* GS rows stay reachable — including shared channel/bandwidth. */
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "gs_channel") == true);
    REQUIRE(pp_settings_is_reachable("gs", "link", "rx_power") == true);
    unsetenv("PP_SIM_DRONE_OFFLINE");
    pp_settings_register_dummy();
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests -j 2>&1 | tail -3'
./build-sim/settings_tests '[dummy]'
```
Expected: FAIL (beamforming seed missing; `is_reachable` not implemented → defaults to true everywhere, so the offline case fails).

- [ ] **Step 3: Implement in `settings_dummy.c`**

In the seed table, keep `{ "txpower", "20" }` (now dBm), change `{ "rx_power", "50" }` → `{ "rx_power", "20" }`, and add `{ "beamforming", "off" }` next to the other `gs/link` seeds.

Add the reachability hook (mirroring how the file reads `PP_SIM_FAIL` in its register function — cache the env into a static bool there):

```c
static bool g_drone_offline = false;   /* set in pp_settings_register_dummy */

/* Mirrors the fpvd provider: drone-backed rows are the air domain plus the
 * two gs-domain drone rows (drone TX power, beamforming handshake). */
static bool dummy_is_reachable(const char *d, const char *p, const char *k) {
    if (!g_drone_offline || !d || !p || !k) return true;
    if (strcmp(d, "air") == 0) return false;
    if (strcmp(d, "gs") == 0 && strcmp(p, "wfbng") == 0 && strcmp(k, "txpower") == 0)
        return false;
    if (strcmp(d, "gs") == 0 && strcmp(p, "link") == 0 && strcmp(k, "beamforming") == 0)
        return false;
    return true;
}
```

In `pp_settings_register_dummy()` add `g_drone_offline = getenv("PP_SIM_DRONE_OFFLINE") != NULL;` next to the existing env reads, and add `.is_reachable = dummy_is_reachable,` to the provider struct.

- [ ] **Step 4: Build and run**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests -j 2>&1 | tail -3'
./build-sim/settings_tests '[dummy]'
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_dummy.c tests/test_settings.cpp
git commit -m "feat(gsmenu): dummy provider dBm seeds, beamforming row, drone-offline sim"
```

---

### Task 7: Link tab UI — dBm sliders + Beamforming toggle

**Files:**
- Modify: `src/gsmenu/pages/link.c`

No host-runnable UI test exists; the gate is a clean simulator compile plus the dummy-provider rows from Task 6 (the sim renders this page from the same builders).

- [ ] **Step 1: Update the page builder**

In `build_link_tab()` replace the two power sliders and add the toggle:

```c
    /* dBm sliders: API range -10..30, integer steps, dim unit label. */
    static const pp_slider_cfg_t DBM_CFG = {
        .raw_min = -10, .raw_max = 30, .step = 1, .fine_step = 0,
        .fine_threshold = 0, .disp_div = 1, .decimals = 0,
        .unit = "dBm", .serialize = PP_SER_INT,
    };
    pp_slider_ex(page, LV_SYMBOL_UP, "TX Power",
                 "gs", "wfbng", "txpower", &DBM_CFG);
    pp_slider_ex(page, LV_SYMBOL_DOWN, "RX Power",
                 "gs", "link", "rx_power", &DBM_CFG);
    pp_toggle(page, LV_SYMBOL_WIFI, "Beamforming", "gs", "link", "beamforming");
```

(These replace the existing `pp_slider(..., "txpower", 1, 63)` and `pp_slider(..., "rx_power", 1, 100)` calls; everything else in the function stays. The toggle goes after the RX Power row, before MCS Index.)

- [ ] **Step 2: Compile the simulator**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target pixelpilot -j 2>&1 | tail -3'
```
Expected: clean build, no warnings about `DBM_CFG`.

- [ ] **Step 3: Run the full test sweep**

```bash
./build-sim/fpvd_tests && ./build-sim/settings_tests
```
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/pages/link.c
git commit -m "feat(gsmenu): dBm TX/RX power sliders and Beamforming toggle in link tab"
```

---

### Task 8: Final verification

- [ ] **Step 1: Full clean test run**

```bash
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests --target settings_tests --target pixelpilot -j 2>&1 | tail -5'
./build-sim/fpvd_tests
./build-sim/settings_tests
```
Expected: all targets build; all tests PASS.

- [ ] **Step 2: Grep for leftovers**

```bash
grep -rn "FPVD_EP_LINK\|FPVD_EP_CONFIG\|FPVD_T_RXPOWER\|link.txpower\b\|/link/apply\|applyTo\|rxpower" src/ tests/ CMakeLists.txt
```
Expected: no hits (except possibly comments referencing history — remove those too).

- [ ] **Step 3: Use superpowers:verification-before-completion, then commit any stragglers**

```bash
git status
git add -A && git commit -m "chore(gsmenu): finish fpvd API migration cleanup"   # only if needed
```

Device deployment verification (GS at `root@10.18.0.1` running the new fpvd-GS) is a manual follow-up per the GS deploy workflow memory — not part of this plan's automated gates.
