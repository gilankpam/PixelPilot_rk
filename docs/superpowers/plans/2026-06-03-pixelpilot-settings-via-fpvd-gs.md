# Route PixelPilot Settings Through the Local GS fpvd — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the PixelPilot `gsmenu` settings backend onto a single provider that talks only to the local ground-station fpvd (`http://127.0.0.1:8080`): drone settings via the opaque `/air/*` proxy, GS radio via the `/link` coordinator — deleting the direct-to-drone calls, the client-side fan-out, and the GS-local file/init.d writers.

**Architecture:** One provider (`settings_fpvd.c`) keyed off a per-keymap-entry **endpoint group** (`AIR` → `/air/config` + `/air/apply`; `LINK` → `/link` + `/link/apply`). It keeps two snapshots (`GET /air/config` and `GET /link`). The router, `settings_gs_local`, and `settings_gs_writers` are removed; non-radio GS rows (HDMI mode, decoder codec, restart action) are dropped. RX power keeps its percent↔driver-value mapping and sends a single `link.txpower` scalar.

**Tech Stack:** C99 (provider, LVGL `lv_async_call`), libcurl (HTTP), cJSON (third_party/cjson), Catch2 + cpp-httplib (tests), CMake.

**Spec:** `docs/superpowers/specs/2026-06-03-pixelpilot-settings-via-fpvd-gs-design.md`

---

## Conventions & how to build/test

All test targets are built under the **simulator** CMake configuration (they live in the `if(USE_SIMULATOR)` / `Catch2_FOUND` block). Build and run from a build dir:

```bash
# configure once (simulator + tests)
cmake -S . -B build-test -DUSE_SIMULATOR=ON
# build a single target
cmake --build build-test --target fpvd_tests -j
# run it
./build-test/fpvd_tests
# run a single Catch2 test by name
./build-test/fpvd_tests "endpoint: routing helpers pick air vs link paths"
```

Test executables this plan touches: `fpvd_tests`, `gs_rxpower_tests`, `gs_enum_tests` (kept/extended); `router_tests`, `gs_writers_tests` (deleted).

The **device** build (`-DUSE_SIMULATOR=OFF`) needs the rockchip/gstreamer/drm toolchain and is not part of the TDD loop; we only keep its CMake source list correct. Do not attempt to compile it locally unless that toolchain is present.

---

## File-by-file map

**Modify**
- `src/gsmenu/settings_fpvd_internal.h` — add `FPVD_T_RXPOWER`, `fpvd_endpoint_t`, `endpoint`/`apply_to` fields on the keymap entry, declare routing helpers + `fpvd_http_post_json`, drop `pp_fpvd_provider_for_router`.
- `src/gsmenu/settings_fpvd.c` — endpoint-aware KEYMAP, routing helpers, `fpvd_http_post_json`, dual snapshot, endpoint-routed reads/writes/apply, rx-power conversion, lock gated to AIR, default URL `127.0.0.1:8080`, single registration.
- `src/gsmenu/settings_gs_rxpower.h` / `.c` — add `pp_rxpower_driver_value_to_pct` + `pp_rxpower_primary_driver`; remove `pp_rxpower_build_json`.
- `src/gsmenu/settings_gs_enum.h` / `.c` — remove `pp_gs_enum_hdmi_modes` + `pp_gs_parse_drm_info_modes`.
- `src/gsmenu/settings.c` — drop the `hdmi_mode` branch in `pp_settings_get_options`.
- `src/gsmenu/settings.h` — update `register_fpvd` doc, remove `register_router` declaration.
- `src/gsmenu/pages/display.c` — remove the HDMI Mode row.
- `src/gsmenu/pages/system.c` — remove the Restart PixelPilot row + its drilldown handlers.
- `src/main.cpp:1442` — `pp_settings_register_router()` → `pp_settings_register_fpvd()`.
- `CMakeLists.txt` — device build source list; test targets.
- `tests/test_settings_fpvd.cpp` — add routing-helper unit tests.
- `tests/test_settings_fpvd_integration.cpp` — rewrite mock server for `/air/*` + `/link`.
- `tests/test_settings_gs_rxpower.cpp` — drop `build_json` cases, add inverse/primary-driver cases.
- `tests/test_settings_gs_enum.cpp` — drop `drm_info` cases.

**Delete**
- `src/gsmenu/settings_router.c`, `src/gsmenu/settings_router_internal.h`
- `src/gsmenu/settings_gs_local.c`, `src/gsmenu/settings_gs_local_internal.h`
- `src/gsmenu/settings_gs_writers.c`, `src/gsmenu/settings_gs_writers.h`
- `tests/test_settings_router.cpp`, `tests/test_settings_gs_writers.cpp`

---

## Task 1: Endpoint routing fields + helpers on the keymap (pure)

Adds the `endpoint`/`apply_to` metadata and three pure path helpers. No behavior change yet — `run_job`/`refresh` still use `/config` until Task 3.

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c:13-30` (enum + struct), `:50-107` (KEYMAP), add helpers near `fpvd_keymap_at`
- Test: `tests/test_settings_fpvd.cpp`

- [ ] **Step 1: Write failing tests** — append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("endpoint: keymap entries carry the right endpoint + applyTo", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e->endpoint == FPVD_EP_AIR);

    e = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "both") == 0);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "both") == 0);

    /* GS card power: percent slider -> GS link.txpower, GS-only apply. */
    e = fpvd_keymap_lookup("gs", "link", "rx_power");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txpower") == 0);
    REQUIRE(e->type == FPVD_T_RXPOWER);
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "gs") == 0);

    /* Drone TX power (1..63 driver units): air endpoint. */
    e = fpvd_keymap_lookup("gs", "wfbng", "txpower");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txpower") == 0);
    REQUIRE(e->endpoint == FPVD_EP_AIR);
}

TEST_CASE("endpoint: routing helpers pick air vs link paths", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *air = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(std::strcmp(fpvd_write_path(air), "/air/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(air), "/air/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(air),  "/air/config") == 0);

    const fpvd_keymap_entry_t *lnk = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(std::strcmp(fpvd_write_path(lnk), "/link") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(lnk), "/link/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(lnk),  "/link") == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail (compile error / unknown symbols)**

Run: `cmake --build build-test --target fpvd_tests -j`
Expected: FAIL — `FPVD_EP_AIR`, `FPVD_T_RXPOWER`, `fpvd_write_path`, and the `endpoint`/`apply_to` fields are undeclared.

- [ ] **Step 3: Extend the internal header** — in `src/gsmenu/settings_fpvd_internal.h`, replace the `fpvd_type_t` enum and `fpvd_keymap_entry_t` struct, and add helper + new-http decls:

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
    FPVD_T_RXPOWER,          /* UI pct 1..100 ↔ GS link.txpower mBm (driver curve) */
} fpvd_type_t;

typedef enum {
    FPVD_EP_AIR,   /* drone proxy:   /air/config + /air/apply,  GET /air/config */
    FPVD_EP_LINK,  /* GS link coord: /link       + /link/apply, GET /link       */
} fpvd_endpoint_t;

typedef struct {
    const char     *domain;
    const char     *page;
    const char     *key;
    const char     *path;
    fpvd_type_t     type;
    fpvd_endpoint_t endpoint;
    const char     *apply_to;  /* LINK: "both"|"gs"; AIR: NULL (ignored) */
} fpvd_keymap_entry_t;

/* Endpoint → URL path. Pure; never NULL. */
const char *fpvd_write_path(const fpvd_keymap_entry_t *e);
const char *fpvd_apply_path(const fpvd_keymap_entry_t *e);
const char *fpvd_read_path (const fpvd_keymap_entry_t *e);
```

In the same header, add next to the other `fpvd_http_*` decls:

```c
fpvd_http_result_t fpvd_http_post_json(const char *url, const char *body);
```

And **delete** this line (router is going away):

```c
/* For use by settings_router only. Returns the static provider table. */
const pp_settings_provider_t *pp_fpvd_provider_for_router(void);
```

- [ ] **Step 4: Update the KEYMAP and add the helpers** — in `src/gsmenu/settings_fpvd.c`, replace the whole `KEYMAP[]` initializer (lines 50-107) so every entry has an endpoint + applyTo. Air rows get `FPVD_EP_AIR, NULL`; the two shared GS rows get `FPVD_EP_LINK, "both"`; add the new `gs/link/rx_power` row:

```c
static const fpvd_keymap_entry_t KEYMAP[] = {
    /* Camera — Video */
    { "air", "camera", "size",       "video.resolution",  FPVD_T_STRING,         FPVD_EP_AIR, NULL },
    { "air", "camera", "fps",        "video.fps",         FPVD_T_INT,            FPVD_EP_AIR, NULL },
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_BITRATE_KBPS,   FPVD_EP_AIR, NULL },
    { "air", "camera", "codec",      "video.codec",       FPVD_T_ENUM,           FPVD_EP_AIR, NULL },
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT,          FPVD_EP_AIR, NULL },
    { "air", "camera", "rc_mode",    "video.rcMode",      FPVD_T_ENUM,           FPVD_EP_AIR, NULL },
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — ROI */
    { "air", "camera", "roi_enabled","video.roi.enabled", FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_qp",     "video.roi.qp",      FPVD_T_INT,            FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_center", "video.roi.center",  FPVD_T_PERCENT_TO_FRAC,FPVD_EP_AIR, NULL },
    { "air", "camera", "roi_steps",  "video.roi.steps",   FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — Image */
    { "air", "camera", "mirror",     "image.mirror",      FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "flip",       "image.flip",        FPVD_T_BOOL,           FPVD_EP_AIR, NULL },
    { "air", "camera", "rotate",     "image.rotate",      FPVD_T_INT,            FPVD_EP_AIR, NULL },

    /* Camera — Recording */
    { "air", "camera", "rec_enable", "recording.enabled",    FPVD_T_BOOL,            FPVD_EP_AIR, NULL },
    { "air", "camera", "rec_split",  "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN,FPVD_EP_AIR, NULL },
    { "air", "camera", "rec_maxmb",  "recording.maxMB",      FPVD_T_INT,             FPVD_EP_AIR, NULL },

    /* Link — shared radio (GS-local-first, pushed to drone server-side) */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT,     FPVD_EP_LINK, "both" },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT,     FPVD_EP_LINK, "both" },

    /* Link — GS card power (percent slider → GS link.txpower mBm, GS-only) */
    { "gs",  "link",  "rx_power",   "link.txpower",  FPVD_T_RXPOWER, FPVD_EP_LINK, "gs" },

    /* Link — drone TX power + modulation (drone-owned) */
    { "gs",  "wfbng", "txpower",    "link.txpower",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "mcs_index",  "link.mcs",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "stbc",       "link.stbc",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "wfbng", "ldpc",       "link.ldpc",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "wfbng", "fec_k",      "link.fec.k",    FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "wfbng", "fec_n",      "link.fec.n",    FPVD_T_INT,  FPVD_EP_AIR, NULL },

    /* Dynamic Link */
    { "air", "dlink", "enabled",              "dynamicLink.enabled",              FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "interleaving",         "dynamicLink.interleavingSupported",FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "mavlink_enable",       "dynamicLink.mavlinkEnable",        FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "osd_enabled",          "dynamicLink.osd.enabled",          FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "osd_debug_latency",    "dynamicLink.osd.debugLatency",     FPVD_T_BOOL, FPVD_EP_AIR, NULL },
    { "air", "dlink", "health_timeout_ms",    "dynamicLink.healthTimeoutMs",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "min_idr_interval_ms",  "dynamicLink.minIdrIntervalMs",     FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "apply_stagger_ms",     "dynamicLink.applyStaggerMs",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "apply_subpace_ms",     "dynamicLink.applySubPaceMs",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_threshold_kbps", "dynamicLink.roiQp.thresholdKbps",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_low_anchor_kbps","dynamicLink.roiQp.lowAnchorKbps",  FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_floor",          "dynamicLink.roiQp.floor",          FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "roiqp_step",           "dynamicLink.roiQp.step",           FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_mcs",             "dynamicLink.safe.mcs",             FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_k",               "dynamicLink.safe.k",               FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_n",               "dynamicLink.safe.n",               FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_depth",           "dynamicLink.safe.depth",           FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_bandwidth",       "dynamicLink.safe.bandwidth",       FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_txpower_dbm",     "dynamicLink.safe.txPowerDbm",      FPVD_T_INT,  FPVD_EP_AIR, NULL },
    { "air", "dlink", "safe_bitrate_kbps",    "dynamicLink.safe.bitrateKbps",     FPVD_T_INT,  FPVD_EP_AIR, NULL },
};
```

Then add the three helpers immediately after `fpvd_keymap_at` (after line 125):

```c
const char *fpvd_write_path(const fpvd_keymap_entry_t *e) {
    return (e && e->endpoint == FPVD_EP_LINK) ? "/link" : "/air/config";
}
const char *fpvd_apply_path(const fpvd_keymap_entry_t *e) {
    return (e && e->endpoint == FPVD_EP_LINK) ? "/link/apply" : "/air/apply";
}
const char *fpvd_read_path(const fpvd_keymap_entry_t *e) {
    return (e && e->endpoint == FPVD_EP_LINK) ? "/link" : "/air/config";
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build-test --target fpvd_tests -j && ./build-test/fpvd_tests "[endpoint]"`
Expected: PASS. Run the whole binary too: `./build-test/fpvd_tests` — all existing `[keymap]/[snapshot]/[patch]/[lock]` cases still PASS (paths/types unchanged).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd_internal.h src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp
git commit -m "gsmenu/fpvd: add endpoint routing metadata + path helpers"
```

---

## Task 2: RX-power inverse + primary-driver helpers

Additive only. `pp_rxpower_build_json` stays for now (its caller `settings_gs_local.c` is alive until Task 4, which removes both together) — this keeps the device build coherent. The `gs_rxpower_tests` target builds and passes on its own.

**Files:**
- Modify: `src/gsmenu/settings_gs_rxpower.h`, `src/gsmenu/settings_gs_rxpower.c`
- Test: `tests/test_settings_gs_rxpower.cpp`

- [ ] **Step 1: Write failing tests** — in `tests/test_settings_gs_rxpower.cpp`, keep the existing cases and add:

```cpp
TEST_CASE("rxpower: value -> pct inverts the forward map", "[gs][rxpower]") {
    int p = 0;
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88X2EU, 1950, &p) == 1);
    REQUIRE(p == 50);
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88X2EU, 1019, &p) == 1);
    REQUIRE(p == 1);
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88X2EU, 2900, &p) == 1);
    REQUIRE(p == 100);
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88XXAU_WFB, -2000, &p) == 1);
    REQUIRE(p == 50);
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88XXAU_WFB, -1020, &p) == 1);
    REQUIRE(p == 1);
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_RTL88XXAU_WFB, -3000, &p) == 1);
    REQUIRE(p == 100);
}

TEST_CASE("rxpower: value -> pct unknown driver returns 0", "[gs][rxpower]") {
    int p = 77;
    REQUIRE(pp_rxpower_driver_value_to_pct(PP_NIC_UNKNOWN, 1500, &p) == 0);
    REQUIRE(p == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build-test --target gs_rxpower_tests -j`
Expected: FAIL — `pp_rxpower_driver_value_to_pct` undeclared.

- [ ] **Step 3: Update the rxpower header** — in `src/gsmenu/settings_gs_rxpower.h`, keep `pp_rxpower_build_json` and add:

```c
/* Inverse of pp_rxpower_pct_to_driver_value: map a driver value back to a
 * percent (1..100, rounded). Returns 0 and sets *out_pct=0 for UNKNOWN. */
int pp_rxpower_driver_value_to_pct(pp_nic_driver_t driver, int value, int *out_pct);

/* Driver of the first wlx* NIC (via the enumeration helpers above), or
 * PP_NIC_UNKNOWN when none is present. Honors PP_GS_SYS_CLASS_NET in tests. */
pp_nic_driver_t pp_rxpower_primary_driver(void);
```

- [ ] **Step 4: Update the rxpower implementation** — in `src/gsmenu/settings_gs_rxpower.c`, keep `pp_rxpower_build_json` and add, after `pp_rxpower_pct_to_driver_value`:

```c
int pp_rxpower_driver_value_to_pct(pp_nic_driver_t drv, int value, int *out_pct) {
    if (!out_pct) return 0;
    int min_v, max_v;
    switch (drv) {
    case PP_NIC_RTL88XXAU_WFB: min_v = -1000; max_v = -3000; break;
    case PP_NIC_RTL88X2EU:     min_v =  1000; max_v =  2900; break;
    default: *out_pct = 0; return 0;
    }
    long num   = (long)(value - min_v) * 100;
    long range = (long)(max_v - min_v);
    /* Round-nearest for signed division: range/2 carries range's sign, so an
     * out-of-range value keeps its sign and is caught by the clamp below. */
    long half = range / 2;
    int pct = (int)((num + half) / range);
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    *out_pct = pct;
    return 1;
}

pp_nic_driver_t pp_rxpower_primary_driver(void) {
    char **nics = pp_rxpower_list_wlx_nics();
    pp_nic_driver_t drv = PP_NIC_UNKNOWN;
    if (nics && nics[0]) {
        char *name = pp_rxpower_nic_driver_name(nics[0]);
        drv = pp_nic_driver_from_name(name);
        free(name);
    }
    if (nics) { for (size_t i = 0; nics[i]; i++) free(nics[i]); free(nics); }
    return drv;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build-test --target gs_rxpower_tests -j && ./build-test/gs_rxpower_tests`
Expected: PASS (all cases).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_gs_rxpower.h src/gsmenu/settings_gs_rxpower.c tests/test_settings_gs_rxpower.cpp
git commit -m "gsmenu/rxpower: add value->pct inverse + primary_driver"
```

---

## Task 3: Rewrite the provider for dual-endpoint routing

The core change. The provider now keeps `air_snapshot` (from `GET /air/config`) and `gs_snapshot` (from `GET /link`, wrapped under `"link"`), routes each write/apply by endpoint, converts rx-power, and gates the dynamic-link lock to AIR entries. The integration test's mock server is rewritten to serve `/air/*` and `/link`.

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (state struct, http helper, refresh, run_job, prov_get, lock check, enqueue, register, default URL)
- Modify: `src/gsmenu/settings_fpvd.c` includes — add the rxpower header
- Modify: `CMakeLists.txt` — add `settings_gs_rxpower.c` to `fpvd_tests`
- Test: rewrite `tests/test_settings_fpvd_integration.cpp`

- [ ] **Step 1: Rewrite the integration mock server + tests** — replace the entire contents of `tests/test_settings_fpvd_integration.cpp` with:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
}

namespace fs = std::filesystem;

static void ensure_lv_init() {
    static bool done = false;
    if (done) return;
    lv_init();
    done = true;
}

/* Mock GS fpvd: /link (+ /link/apply) and the /air/* proxy. */
struct GsMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;

    std::atomic<int> link_get{0}, link_patch{0}, link_apply{0};
    std::atomic<int> air_get{0},  air_patch{0},  air_apply{0};
    std::string last_link_patch_body, last_air_patch_body, last_apply_to;

    /* GET /link returns the link block flat (channel/width/txpower/...). */
    std::string link_response =
        R"({"channel":161,"width":20,"txpower":1950,"region":"US",)"
        R"("linkId":7669206,"droneReachable":true})";
    /* GET /air/config returns the drone config. */
    std::string air_response =
        R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":false,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";
    std::string air_patch_error;   /* if set, PATCH /air/config -> 400 */

    void start() {
        svr.Get("/link", [this](const httplib::Request&, httplib::Response& res) {
            link_get++; res.set_content(link_response, "application/json");
        });
        svr.Patch("/link", [this](const httplib::Request& req, httplib::Response& res) {
            link_patch++; last_link_patch_body = req.body;
            res.set_content("{}", "application/json");
        });
        svr.Post("/link/apply", [this](const httplib::Request& req, httplib::Response& res) {
            link_apply++; last_apply_to = req.body;
            res.set_content(R"({"gsApplied":true})", "application/json");
        });
        svr.Get("/air/config", [this](const httplib::Request&, httplib::Response& res) {
            air_get++; res.set_content(air_response, "application/json");
        });
        svr.Patch("/air/config", [this](const httplib::Request& req, httplib::Response& res) {
            air_patch++; last_air_patch_body = req.body;
            if (!air_patch_error.empty()) { res.status = 400; res.set_content(air_patch_error, "application/json"); }
            else res.set_content(air_response, "application/json");
        });
        svr.Post("/air/apply", [this](const httplib::Request&, httplib::Response& res) {
            air_apply++; res.set_content(R"({"applied":true})", "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    void stop() { svr.stop(); if (th.joinable()) th.join(); }
};

static void install_provider_pointing_at(int port) {
    ensure_lv_init();
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
}

static void wait_first_poll(GsMockServer& m) {
    for (int i = 0; i < 100 && m.link_get == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

TEST_CASE("integration: reads route to the right endpoint", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(m.link_get >= 1);
    REQUIRE(m.air_get  >= 1);
    REQUIRE(pp_settings_is_connected() == true);

    char *chan = pp_settings_get("gs", "wfbng", "gs_channel");   /* from /link */
    REQUIRE(std::string(chan) == "161"); free(chan);
    char *fps = pp_settings_get("air", "camera", "fps");         /* from /air/config */
    REQUIRE(std::string(fps) == "60"); free(fps);
    m.stop();
}

TEST_CASE("integration: air write hits /air/config + /air/apply only", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("air", "camera", "fps", "90", nullptr, nullptr);
    for (int i = 0; i < 200 && m.air_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.air_patch >= 1);
    REQUIRE(m.air_apply >= 1);
    REQUIRE(m.last_air_patch_body.find("\"fps\":90") != std::string::npos);
    REQUIRE(m.link_patch == 0);
    m.stop();
}

TEST_CASE("integration: shared link write hits /link + applyTo both", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("gs", "wfbng", "gs_channel", "149", nullptr, nullptr);
    for (int i = 0; i < 200 && m.link_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.link_patch >= 1);
    REQUIRE(m.last_link_patch_body.find("\"channel\":149") != std::string::npos);
    REQUIRE(m.last_apply_to.find("\"applyTo\":\"both\"") != std::string::npos);
    REQUIRE(m.air_patch == 0);
    m.stop();
}

TEST_CASE("integration: rx_power maps percent -> link.txpower, applyTo gs", "[fpvd][network]") {
    /* NIC fixture: one wlx interface with the rtl88x2eu driver. */
    fs::path root = fs::temp_directory_path() / ("ppnic_" + std::to_string(::getpid()));
    fs::create_directories(root / "wlx_test" / "device");
    std::ofstream(root / "wlx_test" / "device" / "uevent") << "DRIVER=rtl88x2eu\n";
    setenv("PP_GS_SYS_CLASS_NET", root.c_str(), 1);

    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("gs", "link", "rx_power", "50", nullptr, nullptr);
    for (int i = 0; i < 200 && m.link_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.link_patch >= 1);
    REQUIRE(m.last_link_patch_body.find("\"txpower\":1950") != std::string::npos);  /* 50% rtl88x2eu */
    REQUIRE(m.last_apply_to.find("\"applyTo\":\"gs\"") != std::string::npos);

    unsetenv("PP_GS_SYS_CLASS_NET");
    fs::remove_all(root);
    m.stop();
}

TEST_CASE("integration: dynamic_link_locked rejected client-side (air snapshot)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"txpower":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "mcs_index") == true);

    int before = m.air_patch;
    pp_settings_set_async("air", "wfbng", "mcs_index", "5", nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(m.air_patch == before);   /* never sent */
    m.stop();
}

TEST_CASE("integration: PATCH validation error short-circuits apply", "[fpvd][network]") {
    GsMockServer m;
    m.air_patch_error =
      R"({"error":"validation","message":"schema validation failed",)"
      R"("details":[{"path":"link.mcs","message":"must be 0..7"}]})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("air", "wfbng", "mcs_index", "9", nullptr, nullptr);
    for (int i = 0; i < 200 && m.air_patch == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.air_patch >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(m.air_apply == 0);
    m.stop();
}

TEST_CASE("integration: offline -> reconnect tracks /link reachability", "[fpvd][network]") {
    setenv("PP_FPVD_URL", "http://127.0.0.1:1", 1);
    pp_settings_register_fpvd();
    REQUIRE(pp_settings_is_connected() == false);

    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 100 && pp_settings_is_connected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_is_connected() == true);
    m.stop();
}
```

- [ ] **Step 2: Run the integration tests to verify they fail**

Run: `cmake --build build-test --target fpvd_tests -j`
Expected: FAIL — the provider still calls `/config`/`/apply`, so the mock's `/link`/`/air/*` counters stay 0 and the read/route assertions fail (and `settings_gs_rxpower.c` symbols are unlinked).

- [ ] **Step 3: Add the rxpower include + the JSON post helper** — in `src/gsmenu/settings_fpvd.c`, add the include near the top (after `#include "settings_fpvd_internal.h"`):

```c
#include "settings_gs_rxpower.h"
```

and add the new HTTP helper next to the other `fpvd_http_*` definitions (after line 347):

```c
fpvd_http_result_t fpvd_http_post_json(const char *url, const char *body) {
    return http_do(url, "POST", body);
}
```

- [ ] **Step 4: Switch the state to dual snapshots** — in `src/gsmenu/settings_fpvd.c`, in `fpvd_state_t` (around line 39) replace `cJSON *snapshot;` with:

```c
    cJSON   *air_snapshot;        /* GET /air/config (drone), protected by mu */
    cJSON   *gs_snapshot;         /* GET /link wrapped {"link":...}, by mu */
```

and add the endpoint + applyTo to `fpvd_job_t` (around line 21):

```c
typedef struct fpvd_job {
    char     path[128];           /* dotted json path */
    char     value[128];          /* UI value string */
    fpvd_type_t type;
    fpvd_endpoint_t endpoint;     /* AIR or LINK */
    char     apply_to[8];         /* "both"|"gs" for LINK; "" for AIR */
    pp_settings_done_cb on_done;
    void    *user_data;
} fpvd_job_t;
```

- [ ] **Step 5: Rewrite the snapshot refresh** — replace `refresh_snapshot_unlocked` (lines 412-434) with:

```c
/* Called with G.mu HELD. Releases and re-acquires mutex around the HTTP calls.
 * Reachability tracks the GS daemon's own /link; /air/config is proxied to the
 * drone and may 502 while the GS is up — that leaves air_snapshot stale, not
 * a disconnect. */
static void refresh_snapshot_unlocked(void) {
    char *link_url = url_join(G.base_url, "/link");
    char *air_url  = url_join(G.base_url, "/air/config");
    if (!link_url || !air_url) { free(link_url); free(air_url); G.connected = false; return; }
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t lr = fpvd_http_get(link_url);
    fpvd_http_result_t ar = fpvd_http_get(air_url);
    pthread_mutex_lock(&G.mu);
    free(link_url); free(air_url);

    bool was_connected = G.connected;
    if (lr.status == 200 && lr.body) {
        cJSON *flat = cJSON_Parse(lr.body);
        if (flat) {
            cJSON *wrap = cJSON_CreateObject();
            cJSON_AddItemToObject(wrap, "link", flat);   /* wrap so paths resolve */
            if (G.gs_snapshot) cJSON_Delete(G.gs_snapshot);
            G.gs_snapshot = wrap;
            G.connected = true;
        }
    } else {
        G.connected = false;
    }
    if (ar.status == 200 && ar.body) {
        cJSON *a = cJSON_Parse(ar.body);
        if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
    }
    fpvd_http_result_free(&lr);
    fpvd_http_result_free(&ar);
    if (was_connected != G.connected) notify_listener();
    else if (G.connected) notify_listener();
}
```

- [ ] **Step 6: Rewrite the job runner** — replace `run_job_unlocked` (lines 472-527) with:

```c
/* Mutex must be RELEASED on entry. */
static void run_job_unlocked(fpvd_job_t job) {
    /* Build the PATCH body. rx-power converts percent -> driver mBm first. */
    cJSON *body = NULL;
    if (job.type == FPVD_T_RXPOWER) {
        pp_nic_driver_t drv = pp_rxpower_primary_driver();
        int mbm = 0;
        if (!pp_rxpower_pct_to_driver_value(drv, atoi(job.value), &mbm)) {
            schedule_done(job.on_done, job.user_data, -1, "No supported NIC driver");
            return;
        }
        char mbm_s[16]; snprintf(mbm_s, sizeof mbm_s, "%d", mbm);
        body = fpvd_build_patch_body(job.path, mbm_s, FPVD_T_INT);
    } else {
        body = fpvd_build_patch_body(job.path, job.value, job.type);
    }
    char *body_s = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body) cJSON_Delete(body);

    const char *wpath = (job.endpoint == FPVD_EP_LINK) ? "/link"       : "/air/config";
    const char *apath = (job.endpoint == FPVD_EP_LINK) ? "/link/apply" : "/air/apply";
    char *patch_url = url_join(G.base_url, wpath);
    char *apply_url = url_join(G.base_url, apath);
    if (!patch_url || !apply_url) {
        free(patch_url); free(apply_url); if (body_s) free(body_s);
        schedule_done(job.on_done, job.user_data, -1, "Out of memory");
        return;
    }

    fpvd_http_result_t r = fpvd_http_patch_json(patch_url, body_s ? body_s : "{}");
    int rc = 0;
    char err[160] = {0};
    if (r.status == 0) {
        rc = -1; snprintf(err, sizeof err, "GS unreachable");
        pthread_mutex_lock(&G.mu);
        bool was = G.connected; G.connected = false;
        pthread_mutex_unlock(&G.mu);
        if (was) notify_listener();
    } else if (r.status >= 400) {
        rc = -1;
        const char *m = parse_error_message(r.body);
        snprintf(err, sizeof err, "%s", m ? m : "Request rejected");
    }
    fpvd_http_result_free(&r);

    if (rc == 0) {
        if (job.endpoint == FPVD_EP_LINK) {
            char apply_body[40];
            snprintf(apply_body, sizeof apply_body, "{\"applyTo\":\"%s\"}",
                     job.apply_to[0] ? job.apply_to : "both");
            r = fpvd_http_post_json(apply_url, apply_body);
        } else {
            r = fpvd_http_post(apply_url);
        }
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "GS unreachable");
            pthread_mutex_lock(&G.mu);
            bool was = G.connected; G.connected = false;
            pthread_mutex_unlock(&G.mu);
            if (was) notify_listener();
        } else if (r.status >= 400) {
            rc = -1;
            const char *m = parse_error_message(r.body);
            snprintf(err, sizeof err, "%s", m ? m : "Apply failed");
        }
        fpvd_http_result_free(&r);
    }

    if (rc == 0) {
        pthread_mutex_lock(&G.mu);
        refresh_snapshot_unlocked();
        pthread_mutex_unlock(&G.mu);
    }

    schedule_done(job.on_done, job.user_data, rc, err[0] ? err : NULL);
    free(patch_url); free(apply_url); if (body_s) free(body_s);
}
```

> Note: `parse_error_message` already falls back to the `"message"` field and to `"Request rejected"`, so the GS `{"error":"..."}` shape is surfaced verbatim via that fallback. No change needed there.

- [ ] **Step 7: Rewrite enqueue (carry endpoint, coalesce by path+endpoint)** — replace `enqueue_locked` (lines 569-593) with:

```c
static void enqueue_locked(const fpvd_keymap_entry_t *e, const char *value,
                           pp_settings_done_cb cb, void *ud) {
    for (size_t i = 0; i < G.queue_n; i++) {
        /* Two rows share path "link.txpower" on different endpoints (drone TX
         * power vs GS card power) — coalesce only within the same endpoint. */
        if (strcmp(G.queue[i].path, e->path) == 0 &&
            G.queue[i].endpoint == e->endpoint) {
            strncpy(G.queue[i].value, value, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            G.queue[i].on_done = cb;
            G.queue[i].user_data = ud;
            return;
        }
    }
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    strncpy(j->path,  e->path, sizeof j->path  - 1); j->path [sizeof j->path -1] = '\0';
    strncpy(j->value, value,   sizeof j->value - 1); j->value[sizeof j->value-1] = '\0';
    j->type     = e->type;
    j->endpoint = e->endpoint;
    if (e->apply_to) { strncpy(j->apply_to, e->apply_to, sizeof j->apply_to - 1); j->apply_to[sizeof j->apply_to - 1] = '\0'; }
    else j->apply_to[0] = '\0';
    j->on_done   = cb;
    j->user_data = ud;
}
```

- [ ] **Step 8: Route reads + gate the lock to AIR** — replace `prov_get` (lines 595-604), `prov_set_async`'s lock check (lines 606-620), and `prov_is_locked` (lines 632-642) with:

```c
static char *prov_get(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return strdup("");
    pthread_mutex_lock(&G.mu);
    cJSON *snap = (e->endpoint == FPVD_EP_LINK) ? G.gs_snapshot : G.air_snapshot;
    char *out;
    if (e->type == FPVD_T_RXPOWER) {
        char *raw = snap ? fpvd_snapshot_read_string(snap, e->path, FPVD_T_INT) : strdup("");
        out = strdup("");
        if (raw && raw[0]) {
            int pct = 0;
            if (pp_rxpower_driver_value_to_pct(pp_rxpower_primary_driver(), atoi(raw), &pct)) {
                char buf[16]; snprintf(buf, sizeof buf, "%d", pct);
                free(out); out = strdup(buf);
            }
        }
        free(raw);
    } else {
        out = snap ? fpvd_snapshot_read_string(snap, e->path, e->type) : strdup("");
    }
    pthread_mutex_unlock(&G.mu);
    return out;
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) { schedule_done(cb, ud, -1, "Unknown setting"); return; }
    /* Dynamic-link lock only governs drone-owned (AIR) fields. */
    if (e->endpoint == FPVD_EP_AIR && fpvd_is_locked_path(e->path)) {
        pthread_mutex_lock(&G.mu);
        cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
        cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
        bool dlink_on = en && cJSON_IsTrue(en);
        pthread_mutex_unlock(&G.mu);
        if (dlink_on) { schedule_done(cb, ud, -1, "Locked by Dynamic Link"); return; }
    }
    pthread_mutex_lock(&G.mu);
    enqueue_locked(e, v, cb, ud);
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    if (e->endpoint != FPVD_EP_AIR) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on = en && cJSON_IsTrue(en);
    pthread_mutex_unlock(&G.mu);
    return dl_on;
}
```

- [ ] **Step 9: Default URL + dual-snapshot reset in registration** — in `src/gsmenu/settings_fpvd.c`:

Change the default URL macro (line 18):

```c
#define FPVD_DEFAULT_URL "http://127.0.0.1:8080"
```

In `pp_settings_register_fpvd` (around lines 699-707) replace the single-snapshot reset line `if (G.snapshot) { cJSON_Delete(G.snapshot); G.snapshot = NULL; }` with:

```c
    if (G.air_snapshot) { cJSON_Delete(G.air_snapshot); G.air_snapshot = NULL; }
    if (G.gs_snapshot)  { cJSON_Delete(G.gs_snapshot);  G.gs_snapshot  = NULL; }
```

- [ ] **Step 10: Add the rxpower source to the fpvd test target** — in `CMakeLists.txt`, in the `add_executable(fpvd_tests ...)` list (lines 233-239) add `src/gsmenu/settings_gs_rxpower.c`:

```cmake
    add_executable(fpvd_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_fpvd.c
      src/gsmenu/settings_gs_rxpower.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_fpvd.cpp
      tests/test_settings_fpvd_integration.cpp)
```

- [ ] **Step 11: Run all fpvd tests**

Run: `cmake --build build-test --target fpvd_tests -j && ./build-test/fpvd_tests`
Expected: PASS — every `[fpvd]` case including the rewritten `[network]` integration tests.

- [ ] **Step 12: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd_integration.cpp CMakeLists.txt
git commit -m "gsmenu/fpvd: route writes/reads via /air and /link; dual snapshot; rx-power"
```

---

## Task 4: Delete the router + GS-local writers; repoint registration

Now that the unified provider owns both endpoints, remove the router/file-writer layer, point `main.cpp` at `register_fpvd`, and clean CMake + `settings.h`.

**Files:**
- Delete: `src/gsmenu/settings_router.c`, `settings_router_internal.h`, `settings_gs_local.c`, `settings_gs_local_internal.h`, `settings_gs_writers.c`, `settings_gs_writers.h`, `tests/test_settings_router.cpp`, `tests/test_settings_gs_writers.cpp`
- Modify: `src/main.cpp:1442`, `src/gsmenu/settings.h`, `CMakeLists.txt`

- [ ] **Step 1: Repoint the device registration** — in `src/main.cpp` line 1442:

```cpp
	pp_settings_register_fpvd();
```

- [ ] **Step 2: Trim the public header** — in `src/gsmenu/settings.h`: update the `register_fpvd` doc comment (lines 93-95) and **delete** the `register_router` declaration (lines 97-100):

```c
/* Registers the unified ground-station fpvd HTTP provider. Talks only to the
 * local GS fpvd; drone settings go through its /air/* proxy, GS radio through
 * /link. URL defaults to http://127.0.0.1:8080, overridable via PP_FPVD_URL. */
void pp_settings_register_fpvd(void);
```

- [ ] **Step 3: Delete the router + writer files and their tests**

```bash
git rm src/gsmenu/settings_router.c src/gsmenu/settings_router_internal.h \
       src/gsmenu/settings_gs_local.c src/gsmenu/settings_gs_local_internal.h \
       src/gsmenu/settings_gs_writers.c src/gsmenu/settings_gs_writers.h \
       tests/test_settings_router.cpp tests/test_settings_gs_writers.cpp
```

- [ ] **Step 3b: Remove `pp_rxpower_build_json`** — its only caller (`settings_gs_local.c`) is now gone, so drop the obsolete per-NIC dict builder here. (This was deferred from Task 2 to keep the device build coherent.)
  - In `src/gsmenu/settings_gs_rxpower.h`: delete the `pp_rxpower_build_json` declaration (the doc comment "Build the `wifi_txpower = { ... }` JSON-ish dict body ..." + prototype). Keep `pp_rxpower_driver_value_to_pct` and `pp_rxpower_primary_driver`.
  - In `src/gsmenu/settings_gs_rxpower.c`: delete the `pp_rxpower_build_json` function definition.
  - In `tests/test_settings_gs_rxpower.cpp`: delete the three build_json cases (`"rxpower: json single NIC"`, `"rxpower: json skips unknown driver NIC"`, `"rxpower: json all-unknown returns NULL"`). Keep all other cases.
  - Verify: `nix-shell shell-sim.nix --run "cmake --build build-test --target gs_rxpower_tests -j && ./build-test/gs_rxpower_tests"` → PASS (6 cases remain). Confirm `git grep pp_rxpower_build_json` returns nothing.

- [ ] **Step 4: Update CMake — device sources** — in `CMakeLists.txt`, replace the GS-side append block (lines 313-319) with the kept sources only:

```cmake
  # GS-side settings backend (enumeration + rx-power mapping helpers)
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_gs_rxpower.c
      src/gsmenu/settings_gs_enum.c)
```

- [ ] **Step 5: Update CMake — remove dead test targets** — in `CMakeLists.txt`, **delete** the `add_executable(gs_writers_tests ...)` block (lines 248-254) and the `add_executable(router_tests ...)` block (lines 274-284), including their `target_*` lines.

- [ ] **Step 6: Verify the remaining test targets still build + pass**

Run:
```bash
cmake -S . -B build-test -DUSE_SIMULATOR=ON   # re-run configure: CMakeLists changed
cmake --build build-test --target fpvd_tests gs_rxpower_tests gs_enum_tests settings_tests -j
./build-test/fpvd_tests && ./build-test/gs_rxpower_tests && ./build-test/gs_enum_tests && ./build-test/settings_tests
```
Expected: PASS. Confirm `router_tests`/`gs_writers_tests` targets no longer exist (`cmake --build build-test --target router_tests` → error "unknown target").

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "gsmenu: remove router + gs_local + gs_writers; register unified fpvd provider"
```

---

## Task 5: Drop the non-radio GS rows + HDMI-mode enumeration

Remove the HDMI Mode and Restart-PixelPilot rows and the now-unused HDMI-mode enumerator. Channel enumeration (`pp_gs_enum_channels`) stays.

**Files:**
- Modify: `src/gsmenu/pages/display.c`, `src/gsmenu/pages/system.c`
- Modify: `src/gsmenu/settings.c`, `src/gsmenu/settings_gs_enum.h`, `src/gsmenu/settings_gs_enum.c`
- Test: `tests/test_settings_gs_enum.cpp`

- [ ] **Step 1: Remove the HDMI Mode row** — in `src/gsmenu/pages/display.c`, delete the HDMI block (lines 13-18) so the Output section starts at Video Scale:

```c
    pp_section_header(page, "Output");
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 200);
```

(Remove the now-unused `#include <stdlib.h>` only if nothing else needs `free`; `free(hdmi_opts)` is the sole user, so delete that include too.)

- [ ] **Step 2: Remove the Restart PixelPilot row + handlers** — in `src/gsmenu/pages/system.c`:
  - Delete the three handlers `on_restart_confirm_yes`, `on_restart_confirm_no`, `build_restart_drilldown`, `on_open_restart` (lines 34-61).
  - In `build_system_tab`, delete the Restart PixelPilot row (lines 100-101) so the Actions section begins with "Reboot air":

```c
    pp_section_header(page, "Actions");
    lv_obj_t *r;
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot air", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"reboot_air");
```

  (`pp_drilldown.h` is still used elsewhere? It is not after this change — but leave the include; it is harmless and `build_wifi_drilldown` still uses `pp_drilldown_open`. Keep `#include "../widgets/pp_drilldown.h"`.)

- [ ] **Step 3: Drop the hdmi_mode options branch** — in `src/gsmenu/settings.c`, `pp_settings_get_options` (lines 112-119), remove the hdmi branch:

```c
char *pp_settings_get_options(const char *domain, const char *page, const char *key) {
    if (!domain || !page || !key) return NULL;
    if (strcmp(domain, "gs") == 0 && strcmp(page, "wfbng") == 0 && strcmp(key, "gs_channel") == 0)
        return pp_gs_enum_channels();
    return NULL;
}
```

- [ ] **Step 4: Remove the HDMI-mode enumerator** — in `src/gsmenu/settings_gs_enum.h`, delete the two HDMI decls:

```c
/* (removed) pp_gs_parse_drm_info_modes / pp_gs_enum_hdmi_modes */
```

i.e. delete the lines declaring `pp_gs_parse_drm_info_modes` (line 14) and `pp_gs_enum_hdmi_modes` (line 19). In `src/gsmenu/settings_gs_enum.c`, delete the `pp_gs_parse_drm_info_modes` function (lines 77-123) and the `pp_gs_enum_hdmi_modes` function (lines 153-163). The `#include "cJSON.h"` is now unused — remove it.

- [ ] **Step 5: Trim the enum tests** — in `tests/test_settings_gs_enum.cpp`, delete the two `drm_info` test cases (lines 37-62). Keep both `iw_list` cases.

- [ ] **Step 6: Run the enum + dispatcher tests**

Run:
```bash
cmake --build build-test --target gs_enum_tests settings_tests -j
./build-test/gs_enum_tests && ./build-test/settings_tests
```
Expected: PASS.

- [ ] **Step 7: Build the simulator to confirm the pages compile** — display/system are in `SIMULATOR_SOURCES`:

Run: `cmake --build build-test --target pixelpilot -j`
Expected: links successfully (the sim uses the dummy provider; dropped rows just disappear from the menu).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "gsmenu: drop HDMI-mode + Restart-PixelPilot rows and HDMI enumeration"
```

---

## Task 6: Tidy the simulator dummy seeds (optional cleanup)

The in-memory dummy still seeds `hdmi_mode` and `restart_pixelpilot`; with their rows gone the seeds are dead. Remove them for clarity. No behavior change.

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`

- [ ] **Step 1: Remove dead seeds** — in `src/gsmenu/settings_dummy.c`, delete the `{ "hdmi_mode", ... }` entry (line ~92) and the `{ "restart_pixelpilot", ... }` entry (line ~120). Leave `codec`, `gs_channel`, `bandwidth`, `rx_power` seeds intact.

- [ ] **Step 2: Build the simulator + dispatcher tests**

Run: `cmake --build build-test --target pixelpilot settings_tests -j && ./build-test/settings_tests`
Expected: PASS / links cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/settings_dummy.c
git commit -m "gsmenu/dummy: drop dead hdmi_mode + restart_pixelpilot seeds"
```

---

## Self-review

**Spec coverage**

| Spec requirement | Task |
|---|---|
| Single local front door `127.0.0.1:8080`, `PP_FPVD_URL` override | Task 3 (Step 9) |
| Drone via `/air/config` + `/air/apply` | Task 1 (helpers), Task 3 (Steps 6, 8) |
| Shared GS radio via `/link` + `/link/apply applyTo:"both"` | Task 1, Task 3 (Steps 6, 7) |
| rx-power via `/link` + `applyTo:"gs"`, pct→`link.txpower` | Task 1 (keymap), Task 2 (mapping), Task 3 (Steps 6, 8) |
| Two snapshots (`GET /air/config` + wrapped `GET /link`) | Task 3 (Steps 4, 5, 8) |
| Drop client-side fan-out | Task 4 (router deleted) |
| Lock gated to AIR entries; uses air snapshot | Task 3 (Step 8) |
| Dual error-shape parsing (`{error,message,details}` + `{error}`) | Task 3 (Step 6 note — existing `parse_error_message` fallback) |
| Delete router/gs_local/gs_writers; drop `pp_rxpower_build_json` | Task 2, Task 4 |
| Keep rxpower pct↔value + driver/NIC helpers; keep channel enum | Task 2, Task 5 |
| Drop HDMI mode, decoder codec, restart rows + HDMI enum | Task 5 (codec decoder row had no standalone UI — only the deleted fan-out/gs_local path) |
| Repoint `main.cpp` registration | Task 4 (Step 1) |
| Tests: remove router/writers; trim enum/rxpower; add routing tests | Tasks 2, 3, 4, 5 |

**Type consistency:** `fpvd_endpoint_t`/`FPVD_EP_AIR`/`FPVD_EP_LINK`, `FPVD_T_RXPOWER`, and the `endpoint`/`apply_to` fields are introduced in Task 1 (header + struct) and used identically in Task 3 (`run_job_unlocked`, `enqueue_locked`, `prov_get`, `prov_set_async`, `prov_is_locked`). `fpvd_http_post_json` is declared in Task 1 Step 3 (internal header) and defined in Task 3 Step 3. `pp_rxpower_driver_value_to_pct` / `pp_rxpower_primary_driver` are declared+defined in Task 2 and consumed in Task 3. `fpvd_write_path`/`fpvd_apply_path`/`fpvd_read_path` are defined in Task 1; `run_job_unlocked` inlines the equivalent path literals (kept consistent with the helpers — both map LINK→`/link*`, AIR→`/air/*`).

**Placeholders:** none — every code step shows complete content; every run step shows the command and expected outcome.

**Note on `/air/apply` body:** `fpvd_http_post(url)` posts with no body (drone `/apply` takes none); `/link/apply` uses `fpvd_http_post_json` with `{"applyTo":...}`. Both verified against `docs/api.md`.
