# Hot-reloadable DVR + Color-Correction Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move DVR (mode, max-size, re-encode bitrate) and color-correction (enable, gain, offset) config out of CLI flags into a persistent JSON that pixelpilot reads at startup and the in-process menu applies live (no restart).

**Architecture:** A new LVGL-free C module `settings_runtime_cfg` owns the JSON file (read at startup, atomic write on change) and dispatches each committed change through a registerable **apply-ops vtable**. The device build registers the real `extern "C"` runtime setters from `main.cpp` (`dvr_set_mode`, `dvr_set_max_size`, `dvr_reenc_set_bitrate`, plus a new `pp_colortrans_apply` and `dvr_is_recording`); the simulator/tests leave the ops unregistered so calls become no-ops. The fpvd settings provider routes the six keys to this module instead of staging them; the PP menu page greys the three DVR rows while recording.

**Tech Stack:** C (module, gsmenu), C++ (`main.cpp`), cJSON (`third_party/cjson`, already vendored), Catch2 (host tests via `USE_SIMULATOR=ON`), CMake, LVGL.

## Global Constraints

- The module `settings_runtime_cfg.c` MUST NOT depend on LVGL or on `main.cpp` symbols directly — all device calls go through the registered `pp_runtime_cfg_ops_t` vtable. (It is linked into the simulator and host tests, which do not link `main.cpp`.)
- JSON file format (menu-scale values, written by the menu, read by `main.cpp` at startup):
  ```json
  { "dvr": { "mode": "raw", "maxSizeMb": 4000, "reencBitrateKbps": 8000 },
    "colorCorrection": { "enabled": false, "gain": 25, "offset": -15 } }
  ```
- Value↔float mapping: shader `gain = cc_gain / 10.0` (default cc_gain 25 → 2.5); shader `offset = cc_offset / 100.0` (default cc_offset −15 → −0.15).
- Built-in defaults: dvr mode `raw` (0), maxSizeMb 4000, reencBitrateKbps 8000, colorCorrection disabled, gain 25, offset −15.
- The six keys and their widget value-string formats:
  | domain | page | key | widget | value string |
  |---|---|---|---|---|
  | gs | dvr | dvr_mode | dropdown | `raw` / `reencode` (tolerate `both`) |
  | gs | dvr | dvr_max_size | slider | decimal int (MB) |
  | gs | dvr | dvr_reenc_bitrate | dropdown | decimal int (kbps) |
  | gs | display | color_correction | toggle | `on` / `off` |
  | gs | display | cc_gain | slider | decimal int |
  | gs | display | cc_offset | slider | decimal int |
- Default JSON path: `/etc/pixelpilot/runtime.json` (overridable in tests via `pp_runtime_cfg_set_path`). **Open item from spec:** confirm this path is writable+persistent on the GS image before deploy; change the one `#define` if not.
- Atomic write: write `<path>.tmp`, `fflush`+`fclose`, then `rename()` over `<path>`.
- DVR enum mapping: `raw`=0, `reencode`=1, `both`=2 (matches `enum DvrMode` in `src/dvr.h:14`).
- End every commit message with the Co-Authored-By trailer used in this repo.

---

### Task 1: Runtime-config module — header, defaults, and JSON load

**Files:**
- Create: `src/gsmenu/settings_runtime_cfg.h`
- Create: `src/gsmenu/settings_runtime_cfg.c`
- Create: `tests/test_settings_runtime_cfg.cpp`
- Modify: `CMakeLists.txt` (new `runtime_cfg_tests` target, in the test section)

**Interfaces:**
- Produces (consumed by all later tasks):
  - `typedef struct { int dvr_mode; int dvr_max_size_mb; int dvr_reenc_kbps; int cc_enabled; int cc_gain; int cc_offset; } pp_runtime_cfg_t;`
  - `typedef struct { void (*dvr_set_mode)(int); void (*dvr_set_max_size)(int); void (*dvr_reenc_set_bitrate)(int); void (*colortrans_apply)(int,float,float); int (*is_recording)(void); } pp_runtime_cfg_ops_t;`
  - `void pp_runtime_cfg_set_path(const char *path);`
  - `void pp_runtime_cfg_defaults(pp_runtime_cfg_t *out);`
  - `bool pp_runtime_cfg_load(pp_runtime_cfg_t *out);`  (also primes the module's internal cache)
  - `void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops);`  (defined in Task 3; declared here)
  - `bool pp_runtime_cfg_owns(const char*,const char*,const char*);`  (defined in Task 2)
  - `char *pp_runtime_cfg_get(const char*,const char*,const char*);`  (defined in Task 2)
  - `void pp_runtime_cfg_set(const char*,const char*,const char*,const char*);`  (defined in Task 3)
  - `bool pp_runtime_cfg_is_recording(void);`  (defined in Task 3)

- [ ] **Step 1: Write the header**

Create `src/gsmenu/settings_runtime_cfg.h`:

```c
#ifndef PP_SETTINGS_RUNTIME_CFG_H
#define PP_SETTINGS_RUNTIME_CFG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The six hot-reloadable settings, in menu-scale units. */
typedef struct {
    int dvr_mode;        /* 0=raw, 1=reencode, 2=both */
    int dvr_max_size_mb; /* megabytes */
    int dvr_reenc_kbps;  /* kbps */
    int cc_enabled;      /* 0/1 */
    int cc_gain;         /* 0..50   (shader gain   = cc_gain / 10.0)  */
    int cc_offset;       /* -50..50 (shader offset = cc_offset / 100.0) */
} pp_runtime_cfg_t;

/* Apply backend. The device build registers real functions; the simulator and
 * host tests leave it NULL so calls become no-ops and is_recording reads 0. */
typedef struct {
    void (*dvr_set_mode)(int mode);
    void (*dvr_set_max_size)(int mb);
    void (*dvr_reenc_set_bitrate)(int kbps);
    void (*colortrans_apply)(int enabled, float gain, float offset);
    int  (*is_recording)(void);
} pp_runtime_cfg_ops_t;

/* Override the JSON path (default "/etc/pixelpilot/runtime.json"). */
void pp_runtime_cfg_set_path(const char *path);

/* Fill *out with the built-in defaults. */
void pp_runtime_cfg_defaults(pp_runtime_cfg_t *out);

/* Read the JSON file into *out (and prime the internal cache). Missing file or
 * fields fall back to defaults. Returns true if the file existed and parsed. */
bool pp_runtime_cfg_load(pp_runtime_cfg_t *out);

/* Register apply ops (NULL clears). */
void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops);

/* True if (domain,page,key) is one of the six runtime-config keys. */
bool pp_runtime_cfg_owns(const char *domain, const char *page, const char *key);

/* Heap string (caller free()s) of the current value for a runtime key, or NULL
 * if not owned. Strings match the widget value formats. */
char *pp_runtime_cfg_get(const char *domain, const char *page, const char *key);

/* Apply (via ops) + persist (atomic JSON write) a runtime key. No-op if not owned. */
void pp_runtime_cfg_set(const char *domain, const char *page,
                        const char *key, const char *value);

/* True while DVR is recording (ops->is_recording); false if ops unregistered. */
bool pp_runtime_cfg_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif /* PP_SETTINGS_RUNTIME_CFG_H */
```

- [ ] **Step 2: Write the failing test for defaults + load**

Create `tests/test_settings_runtime_cfg.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>   // rand, free, remove, atoi
#include <cstring>   // strlen
#include <string>
extern "C" {
#include "gsmenu/settings_runtime_cfg.h"
}

static std::string write_tmp(const char *body) {
    std::string path = std::string("/tmp/pp_rtcfg_test_") + std::to_string(::rand()) + ".json";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    return path;
}

TEST_CASE("defaults are the documented built-ins") {
    pp_runtime_cfg_t c;
    pp_runtime_cfg_defaults(&c);
    REQUIRE(c.dvr_mode == 0);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    REQUIRE(c.cc_enabled == 0);
    REQUIRE(c.cc_gain == 25);
    REQUIRE(c.cc_offset == -15);
}

TEST_CASE("load missing file yields defaults and returns false") {
    pp_runtime_cfg_set_path("/tmp/pp_rtcfg_does_not_exist_12345.json");
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.cc_gain == 25);
}

TEST_CASE("load reads all fields") {
    std::string p = write_tmp(
        "{\"dvr\":{\"mode\":\"both\",\"maxSizeMb\":8000,\"reencBitrateKbps\":12000},"
        "\"colorCorrection\":{\"enabled\":true,\"gain\":30,\"offset\":-20}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 2);
    REQUIRE(c.dvr_max_size_mb == 8000);
    REQUIRE(c.dvr_reenc_kbps == 12000);
    REQUIRE(c.cc_enabled == 1);
    REQUIRE(c.cc_gain == 30);
    REQUIRE(c.cc_offset == -20);
    remove(p.c_str());
}

TEST_CASE("load tolerates partial json — absent fields keep defaults") {
    std::string p = write_tmp("{\"dvr\":{\"mode\":\"reencode\"}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 1);
    REQUIRE(c.dvr_max_size_mb == 4000);   /* default */
    REQUIRE(c.cc_gain == 25);             /* default */
    remove(p.c_str());
}

TEST_CASE("load on malformed json returns false and fills defaults") {
    std::string p = write_tmp("{ this is not json ");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    remove(p.c_str());
}
```

- [ ] **Step 3: Add the `runtime_cfg_tests` CMake target**

In `CMakeLists.txt`, immediately after the `gs_enum_tests` target block (the one defined around line 266, which links `${PP_CJSON_SOURCES}`), add:

```cmake
    add_executable(runtime_cfg_tests
      src/gsmenu/settings_runtime_cfg.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_runtime_cfg.cpp)
    target_include_directories(runtime_cfg_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/gsmenu
      ${PP_CJSON_INC})
    target_link_libraries(runtime_cfg_tests
      Catch2::Catch2WithMain)
```

- [ ] **Step 4: Run the test to verify it fails (link error: functions undefined)**

Run:
```bash
nix-shell --run "cmake -S . -B build-test -DUSE_SIMULATOR=ON >/dev/null && cmake --build build-test --target runtime_cfg_tests"
```
Expected: FAIL — undefined references to `pp_runtime_cfg_defaults` / `pp_runtime_cfg_load` / `pp_runtime_cfg_set_path` (the `.c` has no bodies yet).

- [ ] **Step 5: Implement defaults + load in the module**

Create `src/gsmenu/settings_runtime_cfg.c`:

```c
#include "settings_runtime_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- module state ---- */
static char g_path[512] = "/etc/pixelpilot/runtime.json";
static pp_runtime_cfg_t g_state;
static bool g_state_primed = false;
static const pp_runtime_cfg_ops_t *g_ops = NULL;   /* set in Task 3 */

/* ---- enum <-> string ---- */
static int mode_str_to_int(const char *s, int fallback) {
    if (!s) return fallback;
    if (!strcmp(s, "raw"))      return 0;
    if (!strcmp(s, "reencode")) return 1;
    if (!strcmp(s, "both"))     return 2;
    return fallback;
}
static const char *mode_int_to_str(int m) {
    return m == 1 ? "reencode" : m == 2 ? "both" : "raw";
}

void pp_runtime_cfg_set_path(const char *path) {
    snprintf(g_path, sizeof g_path, "%s", path);
    g_state_primed = false;   /* path changed — re-read on next access */
}

void pp_runtime_cfg_defaults(pp_runtime_cfg_t *out) {
    out->dvr_mode        = 0;
    out->dvr_max_size_mb = 4000;
    out->dvr_reenc_kbps  = 8000;
    out->cc_enabled      = 0;
    out->cc_gain         = 25;
    out->cc_offset       = -15;
}

/* Parse the file at g_path into *out. Returns true on a successful read+parse. */
static bool read_file(pp_runtime_cfg_t *out) {
    pp_runtime_cfg_defaults(out);

    FILE *f = fopen(g_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 16)) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *dvr = cJSON_GetObjectItemCaseSensitive(root, "dvr");
    if (dvr) {
        cJSON *m  = cJSON_GetObjectItemCaseSensitive(dvr, "mode");
        if (cJSON_IsString(m)) out->dvr_mode = mode_str_to_int(m->valuestring, out->dvr_mode);
        cJSON *ms = cJSON_GetObjectItemCaseSensitive(dvr, "maxSizeMb");
        if (cJSON_IsNumber(ms)) out->dvr_max_size_mb = (int)ms->valuedouble;
        cJSON *rb = cJSON_GetObjectItemCaseSensitive(dvr, "reencBitrateKbps");
        if (cJSON_IsNumber(rb)) out->dvr_reenc_kbps = (int)rb->valuedouble;
    }
    cJSON *cc = cJSON_GetObjectItemCaseSensitive(root, "colorCorrection");
    if (cc) {
        cJSON *en = cJSON_GetObjectItemCaseSensitive(cc, "enabled");
        if (cJSON_IsBool(en)) out->cc_enabled = cJSON_IsTrue(en) ? 1 : 0;
        cJSON *g  = cJSON_GetObjectItemCaseSensitive(cc, "gain");
        if (cJSON_IsNumber(g)) out->cc_gain = (int)g->valuedouble;
        cJSON *o  = cJSON_GetObjectItemCaseSensitive(cc, "offset");
        if (cJSON_IsNumber(o)) out->cc_offset = (int)o->valuedouble;
    }
    cJSON_Delete(root);
    return true;
}

bool pp_runtime_cfg_load(pp_runtime_cfg_t *out) {
    bool ok = read_file(&g_state);
    g_state_primed = true;
    *out = g_state;
    return ok;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests && ./build-test/runtime_cfg_tests"
```
Expected: PASS (all 5 test cases).

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/settings_runtime_cfg.h src/gsmenu/settings_runtime_cfg.c tests/test_settings_runtime_cfg.cpp CMakeLists.txt
git commit -m "runtime-cfg: module scaffold, defaults, and JSON load

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Runtime-config module — `owns` and `get`

**Files:**
- Modify: `src/gsmenu/settings_runtime_cfg.c`
- Test: `tests/test_settings_runtime_cfg.cpp`

**Interfaces:**
- Consumes: `pp_runtime_cfg_t`, `read_file`, `g_state`, `g_state_primed`, `mode_int_to_str` (Task 1).
- Produces: `pp_runtime_cfg_owns(d,p,k)` and `pp_runtime_cfg_get(d,p,k)` returning the widget-format value strings (Global Constraints table).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_settings_runtime_cfg.cpp`:

```cpp
TEST_CASE("owns matches exactly the six keys") {
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_mode"));
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_max_size"));
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_reenc_bitrate"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "color_correction"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "cc_gain"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "cc_offset"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", "display", "screen_mode"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", "dvr", "rec_enabled"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("air", "camera", "fps"));
}

TEST_CASE("get returns widget-format strings from the loaded file") {
    std::string p = write_tmp(
        "{\"dvr\":{\"mode\":\"reencode\",\"maxSizeMb\":8000,\"reencBitrateKbps\":12000},"
        "\"colorCorrection\":{\"enabled\":true,\"gain\":30,\"offset\":-20}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c; pp_runtime_cfg_load(&c);

    auto chk = [](const char *d, const char *pg, const char *k, const char *want) {
        char *v = pp_runtime_cfg_get(d, pg, k);
        REQUIRE(v != nullptr);
        REQUIRE(std::string(v) == want);
        free(v);
    };
    chk("gs", "dvr", "dvr_mode", "reencode");
    chk("gs", "dvr", "dvr_max_size", "8000");
    chk("gs", "dvr", "dvr_reenc_bitrate", "12000");
    chk("gs", "display", "color_correction", "on");
    chk("gs", "display", "cc_gain", "30");
    chk("gs", "display", "cc_offset", "-20");
    REQUIRE(pp_runtime_cfg_get("gs", "display", "screen_mode") == nullptr);
    remove(p.c_str());
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests"
```
Expected: FAIL — undefined references to `pp_runtime_cfg_owns` / `pp_runtime_cfg_get`.

- [ ] **Step 3: Implement `owns` and `get`**

Append to `src/gsmenu/settings_runtime_cfg.c`:

```c
static void ensure_primed(void) {
    if (!g_state_primed) { read_file(&g_state); g_state_primed = true; }
}

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

bool pp_runtime_cfg_owns(const char *domain, const char *page, const char *key) {
    if (!eq(domain, "gs")) return false;
    if (eq(page, "dvr"))
        return eq(key, "dvr_mode") || eq(key, "dvr_max_size") || eq(key, "dvr_reenc_bitrate");
    if (eq(page, "display"))
        return eq(key, "color_correction") || eq(key, "cc_gain") || eq(key, "cc_offset");
    return false;
}

char *pp_runtime_cfg_get(const char *domain, const char *page, const char *key) {
    if (!pp_runtime_cfg_owns(domain, page, key)) return NULL;
    ensure_primed();

    char buf[32];
    if (eq(page, "dvr")) {
        if (eq(key, "dvr_mode"))          return strdup(mode_int_to_str(g_state.dvr_mode));
        if (eq(key, "dvr_max_size"))      { snprintf(buf, sizeof buf, "%d", g_state.dvr_max_size_mb); return strdup(buf); }
        if (eq(key, "dvr_reenc_bitrate")) { snprintf(buf, sizeof buf, "%d", g_state.dvr_reenc_kbps);  return strdup(buf); }
    } else { /* display */
        if (eq(key, "color_correction"))  return strdup(g_state.cc_enabled ? "on" : "off");
        if (eq(key, "cc_gain"))           { snprintf(buf, sizeof buf, "%d", g_state.cc_gain);   return strdup(buf); }
        if (eq(key, "cc_offset"))         { snprintf(buf, sizeof buf, "%d", g_state.cc_offset); return strdup(buf); }
    }
    return NULL;
}
```

- [ ] **Step 4: Run to verify pass**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests && ./build-test/runtime_cfg_tests"
```
Expected: PASS (all cases incl. the two new ones).

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_runtime_cfg.c tests/test_settings_runtime_cfg.cpp
git commit -m "runtime-cfg: owns predicate and widget-format get

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Runtime-config module — ops vtable, `set` (apply + persist), `is_recording`

**Files:**
- Modify: `src/gsmenu/settings_runtime_cfg.c`
- Test: `tests/test_settings_runtime_cfg.cpp`

**Interfaces:**
- Consumes: module state + `read_file` + `mode_str_to_int`/`mode_int_to_str` + `g_ops` (Task 1), `ensure_primed`/`eq` (Task 2).
- Produces:
  - `void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops)`
  - `void pp_runtime_cfg_set(d,p,k,v)` — updates cache, calls the matching op, then atomically writes the file.
  - `bool pp_runtime_cfg_is_recording(void)`
  - Color-correction `set` of any of the three cc keys calls `ops->colortrans_apply(cc_enabled, cc_gain/10.0f, cc_offset/100.0f)` (all three current cc fields).

- [ ] **Step 1: Write the failing tests (fake ops capture calls; persistence round-trips)**

Append to `tests/test_settings_runtime_cfg.cpp`:

```cpp
/* ---- fake apply ops ---- */
struct FakeOps {
    int mode = -1, max_mb = -1, kbps = -1;
    int cc_enabled = -1; float cc_gain = -1, cc_offset = -1;
    int recording = 0;
};
static FakeOps g_fake;
static void f_mode(int m)      { g_fake.mode = m; }
static void f_max(int mb)      { g_fake.max_mb = mb; }
static void f_kbps(int k)      { g_fake.kbps = k; }
static void f_cc(int e, float g, float o) { g_fake.cc_enabled = e; g_fake.cc_gain = g; g_fake.cc_offset = o; }
static int  f_rec(void)        { return g_fake.recording; }

static void install_fake_ops() {
    static pp_runtime_cfg_ops_t ops = { f_mode, f_max, f_kbps, f_cc, f_rec };
    pp_runtime_cfg_set_ops(&ops);
}

TEST_CASE("set applies via ops and persists to disk") {
    g_fake = FakeOps{};
    install_fake_ops();
    std::string p = std::string("/tmp/pp_rtcfg_set_") + std::to_string(::rand()) + ".json";
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c; pp_runtime_cfg_load(&c);   /* primes from (missing) file -> defaults */

    pp_runtime_cfg_set("gs", "dvr", "dvr_mode", "both");
    REQUIRE(g_fake.mode == 2);

    pp_runtime_cfg_set("gs", "dvr", "dvr_max_size", "9000");
    REQUIRE(g_fake.max_mb == 9000);

    pp_runtime_cfg_set("gs", "dvr", "dvr_reenc_bitrate", "16000");
    REQUIRE(g_fake.kbps == 16000);

    pp_runtime_cfg_set("gs", "display", "cc_gain", "30");
    /* colortrans_apply gets all three cc fields; gain mapped /10 */
    REQUIRE(g_fake.cc_gain == 3.0f);
    REQUIRE(g_fake.cc_offset == -0.15f);   /* default -15 / 100 */
    REQUIRE(g_fake.cc_enabled == 0);

    pp_runtime_cfg_set("gs", "display", "color_correction", "on");
    REQUIRE(g_fake.cc_enabled == 1);

    /* Persisted: reload from a fresh path-reset and confirm round-trip. */
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t r; REQUIRE(pp_runtime_cfg_load(&r) == true);
    REQUIRE(r.dvr_mode == 2);
    REQUIRE(r.dvr_max_size_mb == 9000);
    REQUIRE(r.dvr_reenc_kbps == 16000);
    REQUIRE(r.cc_gain == 30);
    REQUIRE(r.cc_enabled == 1);
    remove(p.c_str());
    pp_runtime_cfg_set_ops(NULL);
}

TEST_CASE("set on a non-owned key is a no-op") {
    g_fake = FakeOps{};
    install_fake_ops();
    pp_runtime_cfg_set("gs", "display", "screen_mode", "1280x720@60");
    REQUIRE(g_fake.mode == -1);
    pp_runtime_cfg_set_ops(NULL);
}

TEST_CASE("is_recording reflects ops; false when unregistered") {
    pp_runtime_cfg_set_ops(NULL);
    REQUIRE(pp_runtime_cfg_is_recording() == false);
    g_fake = FakeOps{}; g_fake.recording = 1;
    install_fake_ops();
    REQUIRE(pp_runtime_cfg_is_recording() == true);
    pp_runtime_cfg_set_ops(NULL);
}
```

- [ ] **Step 2: Run to verify failure**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests"
```
Expected: FAIL — undefined references to `pp_runtime_cfg_set_ops` / `pp_runtime_cfg_set` / `pp_runtime_cfg_is_recording`.

- [ ] **Step 3: Implement ops, set, persist, is_recording**

Append to `src/gsmenu/settings_runtime_cfg.c`:

```c
void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops) { g_ops = ops; }

bool pp_runtime_cfg_is_recording(void) {
    return (g_ops && g_ops->is_recording) ? (g_ops->is_recording() != 0) : false;
}

/* Atomic write of g_state to g_path via "<path>.tmp" + rename(). */
static void persist(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *dvr  = cJSON_AddObjectToObject(root, "dvr");
    cJSON_AddStringToObject(dvr, "mode", mode_int_to_str(g_state.dvr_mode));
    cJSON_AddNumberToObject(dvr, "maxSizeMb", g_state.dvr_max_size_mb);
    cJSON_AddNumberToObject(dvr, "reencBitrateKbps", g_state.dvr_reenc_kbps);
    cJSON *cc = cJSON_AddObjectToObject(root, "colorCorrection");
    cJSON_AddBoolToObject(cc, "enabled", g_state.cc_enabled ? 1 : 0);
    cJSON_AddNumberToObject(cc, "gain", g_state.cc_gain);
    cJSON_AddNumberToObject(cc, "offset", g_state.cc_offset);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;

    char tmp[540];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fflush(f);
        fclose(f);
        rename(tmp, g_path);   /* atomic replace */
    }
    free(txt);
}

static void apply_colortrans(void) {
    if (g_ops && g_ops->colortrans_apply)
        g_ops->colortrans_apply(g_state.cc_enabled,
                                g_state.cc_gain   / 10.0f,
                                g_state.cc_offset / 100.0f);
}

void pp_runtime_cfg_set(const char *domain, const char *page,
                        const char *key, const char *value) {
    if (!pp_runtime_cfg_owns(domain, page, key)) return;
    ensure_primed();

    if (eq(page, "dvr")) {
        if (eq(key, "dvr_mode")) {
            g_state.dvr_mode = mode_str_to_int(value, g_state.dvr_mode);
            if (g_ops && g_ops->dvr_set_mode) g_ops->dvr_set_mode(g_state.dvr_mode);
        } else if (eq(key, "dvr_max_size")) {
            g_state.dvr_max_size_mb = atoi(value);
            if (g_ops && g_ops->dvr_set_max_size) g_ops->dvr_set_max_size(g_state.dvr_max_size_mb);
        } else if (eq(key, "dvr_reenc_bitrate")) {
            g_state.dvr_reenc_kbps = atoi(value);
            if (g_ops && g_ops->dvr_reenc_set_bitrate) g_ops->dvr_reenc_set_bitrate(g_state.dvr_reenc_kbps);
        }
    } else { /* display */
        if (eq(key, "color_correction")) {
            g_state.cc_enabled = (strcmp(value, "on") == 0) ? 1 : 0;
            apply_colortrans();
        } else if (eq(key, "cc_gain")) {
            g_state.cc_gain = atoi(value);
            apply_colortrans();
        } else if (eq(key, "cc_offset")) {
            g_state.cc_offset = atoi(value);
            apply_colortrans();
        }
    }
    persist();
}
```

- [ ] **Step 4: Run to verify pass**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests && ./build-test/runtime_cfg_tests"
```
Expected: PASS (all cases).

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_runtime_cfg.c tests/test_settings_runtime_cfg.cpp
git commit -m "runtime-cfg: ops vtable, set (apply+atomic persist), is_recording

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: pixelpilot side — `extern "C"` apply wrapper, startup seeding, ops registration, deprecate flags

**Files:**
- Modify: `src/main.cpp` (add `pp_colortrans_apply` + `dvr_is_recording` in the existing `extern "C"` block ~main.cpp:564-722; seed globals + register ops at startup; turn the 4 flags into deprecated no-ops; help text)
- Modify: `CMakeLists.txt` (compile `settings_runtime_cfg.c` into the simulator and device builds)

**Interfaces:**
- Consumes: `pp_runtime_cfg_load`, `pp_runtime_cfg_set_ops`, `pp_runtime_cfg_set_path`, `pp_runtime_cfg_ops_t` (Tasks 1–3); existing `dvr_set_mode/dvr_set_max_size/dvr_reenc_set_bitrate`, `gamma_lut_enable/gamma_lut_disable`, `set_color_correction/set_color_correction_enabled`, globals `enable_live_colortrans/live_colortrans_gain/live_colortrans_offset/lut_ctrl/drm_fd/frame_proc/dvr_enabled`.
- Produces (consumed by ops registration): `extern "C" void pp_colortrans_apply(int,float,float)` and `extern "C" int dvr_is_recording(void)`.

> **No host test:** `main.cpp` is not linked by any test or the simulator. This task is verified by a successful **device** build and the manual checks in Task 8. The apply *logic* is already covered host-side in Tasks 1–3 via the fake ops.

- [ ] **Step 1: Add the two `extern "C"` functions**

In `src/main.cpp`, inside the existing `extern "C" { ... }` block (after `dvr_reenc_notify_colortrans`, near line 571), add:

```c
    // Unified live color-correction apply: live display (DRM gamma LUT) + the
    // re-encode pipeline. gain/offset are the shader-space floats.
    void pp_colortrans_apply(int enabled, float gain, float offset) {
        enable_live_colortrans = enabled ? true : false;
        live_colortrans_gain   = gain;
        live_colortrans_offset = offset;
        // Live display: push/clear the CRTC gamma LUT. The display thread reads
        // enable_live_colortrans per frame (main.cpp ~424) and swaps the OSD
        // compositing path automatically.
        if (enabled) gamma_lut_enable(&lut_ctrl, offset, gain);
        else         gamma_lut_disable(&lut_ctrl);
        // Re-encode pipeline (only exists when a reencode/both mode is active).
        if (frame_proc) {
            if (enabled) frame_proc->set_color_correction(gain, offset, drm_fd);
            else         frame_proc->set_color_correction_enabled(false);
        }
    }

    int dvr_is_recording(void) { return dvr_enabled; }
```

- [ ] **Step 2: Include the module header in `main.cpp`**

Near the other `#include` lines at the top of `src/main.cpp`, add:

```cpp
#include "gsmenu/settings_runtime_cfg.h"
```

- [ ] **Step 3: Seed globals from the JSON at startup**

In `src/main.cpp`, after the YAML config block that reads `config["gsmenu"]["enabled"]` (around main.cpp:1336) and **before** the DVR/encoder/gamma initialization (the `gamma_lut_controller_init` call is at ~1421 and DVR init at ~1466), insert:

```cpp
    // Seed DVR + color-correction globals from the persistent runtime JSON.
    // JSON is authoritative; the matching CLI flags are deprecated no-ops.
    {
        pp_runtime_cfg_t rc;
        bool loaded = pp_runtime_cfg_load(&rc);
        dvr_mode               = (DvrMode)rc.dvr_mode;
        dvr_max_file_size      = (int64_t)rc.dvr_max_size_mb * 1000000LL;
        reenc_params.bitrate_kbps = rc.dvr_reenc_kbps;
        enable_live_colortrans = rc.cc_enabled ? true : false;
        live_colortrans_gain   = rc.cc_gain   / 10.0f;
        live_colortrans_offset = rc.cc_offset / 100.0f;
        spdlog::info("runtime.json {}: dvr_mode={} maxMB={} reencKbps={} cc={} gain={} offset={}",
                     loaded ? "loaded" : "defaulted", rc.dvr_mode, rc.dvr_max_size_mb,
                     rc.dvr_reenc_kbps, rc.cc_enabled, live_colortrans_gain, live_colortrans_offset);
    }
```

- [ ] **Step 4: Register the apply ops after the DVR/gamma subsystems are up**

In `src/main.cpp`, after `gamma_lut_controller_init(&lut_ctrl, drm_fd, output_list);` (main.cpp:1421) — so `lut_ctrl` is valid — add:

```cpp
    static const pp_runtime_cfg_ops_t k_runtime_cfg_ops = {
        dvr_set_mode,
        dvr_set_max_size,
        dvr_reenc_set_bitrate,
        pp_colortrans_apply,
        dvr_is_recording,
    };
    pp_runtime_cfg_set_ops(&k_runtime_cfg_ops);
```

- [ ] **Step 5: Turn the four CLI flags into deprecated no-ops**

In `src/main.cpp` argument parsing, replace the bodies of the four `__OnArgument` blocks so they accept-and-ignore (do **not** delete the blocks — fpvd may still pass them via `EXTRA_OPTS`). Replace:

`--dvr-max-size` (main.cpp ~1137):
```c
	__OnArgument("--dvr-max-size") {
		(void)__ArgValue;
		spdlog::warn("--dvr-max-size is deprecated and ignored (set via runtime.json)");
		continue;
	}
```
`--dvr-mode` (main.cpp ~1152):
```c
	__OnArgument("--dvr-mode") {
		(void)__ArgValue;
		spdlog::warn("--dvr-mode is deprecated and ignored (set via runtime.json)");
		continue;
	}
```
`--dvr-reenc-bitrate` (main.cpp ~1170):
```c
	__OnArgument("--dvr-reenc-bitrate") {
		(void)__ArgValue;
		spdlog::warn("--dvr-reenc-bitrate is deprecated and ignored (set via runtime.json)");
		continue;
	}
```
`--live-colortrans` (main.cpp ~1274):
```c
	__OnArgument("--live-colortrans") {
		spdlog::warn("--live-colortrans is deprecated and ignored (set via runtime.json)");
		continue;
	}
```

- [ ] **Step 6: Update the `--help` text**

In the help string block (main.cpp ~1023-1043), append "(deprecated; set via runtime.json)" to the `--dvr-max-size`, `--dvr-mode`, `--dvr-reenc-bitrate`, and `--live-colortrans` lines. Example for `--dvr-mode`:
```c
    "    --dvr-mode <mode>      - DEPRECATED, ignored (set via runtime.json)\n"
```

- [ ] **Step 7: Add the module to the simulator and device source lists**

In `CMakeLists.txt`:
- In `SIMULATOR_SOURCES` (the list starting ~line 50, which already lists `src/gsmenu/pages/pixelpilot.c` at ~85 and `third_party/cjson/cJSON.c` at ~99), add a line:
  ```cmake
        src/gsmenu/settings_runtime_cfg.c
  ```
- In the device `list(APPEND SOURCE_FILES ...)` fpvd block (~line 414-419, which lists `src/gsmenu/settings_fpvd.c` and `third_party/cjson/cJSON.c`), add:
  ```cmake
      src/gsmenu/settings_runtime_cfg.c
  ```
- Ensure `src/gsmenu` is on the include path for both builds (it is reached via `${PROJECT_SOURCE_DIR}/src` since the header includes use `gsmenu/...`; the module's own `#include "cJSON.h"` resolves through the existing cJSON include dir already configured for `settings_fpvd.c`).

- [ ] **Step 8: Build the simulator (proves the module links without `main.cpp`)**

Run:
```bash
nix-shell --run "cmake -S . -B build-test -DUSE_SIMULATOR=ON >/dev/null && cmake --build build-test --target pixelpilot"
```
Expected: PASS — the simulator binary links (ops unregistered → no-ops). This is the key check that the module has no hard dependency on `main.cpp`.

- [ ] **Step 9: Sanity-build the host test suite**

Run:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests && ./build-test/runtime_cfg_tests"
```
Expected: PASS.

> Device cross-build (the real target that links `main.cpp`) is exercised in Task 8 per the GS deploy workflow; do it there to keep this task's loop fast.

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp CMakeLists.txt
git commit -m "main: seed runtime.json at startup, register apply ops, deprecate DVR/colortrans flags

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: fpvd provider — route the six keys to runtime-cfg; drop `gs/dvr/*` from the keymap

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (KEYMAP + `prov_get`, `prov_set_async`, `prov_is_available`, `prov_is_locked`)
- Modify: `tests/test_settings_fpvd.cpp` (keymap assertions)
- Modify: `CMakeLists.txt` (`fpvd_tests` links `settings_runtime_cfg.c`)

**Interfaces:**
- Consumes: `pp_runtime_cfg_owns`, `pp_runtime_cfg_get`, `pp_runtime_cfg_set`, `pp_runtime_cfg_is_recording` (Tasks 2–3).
- Produces: the menu's generic `pp_settings_get/set/is_available/is_locked` now transparently serve the six runtime keys, and `gs/dvr/*` keys no longer resolve in the fpvd KEYMAP.

- [ ] **Step 1: Update the failing keymap test**

In `tests/test_settings_fpvd.cpp`, find the existing assertions about the DVR keys and color-correction keys. Replace/confirm them so they assert the new routing. Add (or adjust) this test:

```cpp
TEST_CASE("runtime-config keys are no longer fpvd-staged but are available") {
    // gs/dvr/* dropped from the fpvd keymap (now owned by runtime-cfg)
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_mode") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_max_size") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_reenc_bitrate") == nullptr);
    // color-correction keys were never in the keymap
    REQUIRE(fpvd_keymap_lookup("gs", "display", "color_correction") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "display", "cc_gain") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "display", "cc_offset") == nullptr);
    // ...but the provider reports all six as available (so rows are not greyed)
    pp_settings_register_fpvd();
    REQUIRE(pp_settings_is_available("gs", "dvr", "dvr_mode"));
    REQUIRE(pp_settings_is_available("gs", "display", "cc_gain"));
    // a still-staged display row remains available too
    REQUIRE(pp_settings_is_available("gs", "display", "screen_mode"));
}
```

> If `test_settings_fpvd.cpp` already includes `settings.h`, no new include is needed; otherwise add `#include "gsmenu/settings.h"` and `#include "gsmenu/settings_runtime_cfg.h"`.

- [ ] **Step 2: Link `settings_runtime_cfg.c` into `fpvd_tests`**

In `CMakeLists.txt`, in the `add_executable(fpvd_tests ...)` source list (~line 251-256), add:
```cmake
      src/gsmenu/settings_runtime_cfg.c
```

- [ ] **Step 3: Run to verify failure**

Run:
```bash
nix-shell --run "cmake -S . -B build-test -DUSE_SIMULATOR=ON >/dev/null && cmake --build build-test --target fpvd_tests && ./build-test/fpvd_tests"
```
Expected: FAIL — `fpvd_keymap_lookup("gs","dvr","dvr_mode")` still returns non-null (keymap not yet edited).

- [ ] **Step 4: Remove the three `gs/dvr/*` rows from the KEYMAP**

In `src/gsmenu/settings_fpvd.c`, delete these three lines (currently main.cpp keymap lines 121-123):
```c
    { "gs",  "dvr",     "dvr_mode",         "pixelpilot.dvr.mode",            FPVD_T_ENUM,            FPVD_EP_GS, FPVD_ROW_STAGED },
    { "gs",  "dvr",     "dvr_max_size",     "pixelpilot.dvr.maxSizeMb",       FPVD_T_INT,             FPVD_EP_GS, FPVD_ROW_STAGED },
    { "gs",  "dvr",     "dvr_reenc_bitrate","pixelpilot.dvr.reencBitrate",    FPVD_T_INT,             FPVD_EP_GS, FPVD_ROW_STAGED },
```

- [ ] **Step 5: Add the include and route the four provider entry points**

At the top of `src/gsmenu/settings_fpvd.c`, add:
```c
#include "settings_runtime_cfg.h"
```

In `prov_get` (settings_fpvd.c:855), make the runtime keys answer from the module — add at the very start of the function body:
```c
    if (pp_runtime_cfg_owns(d, p, k)) return pp_runtime_cfg_get(d, p, k);
```

In `prov_set_async` (settings_fpvd.c:878), add at the very start of the function body:
```c
    if (pp_runtime_cfg_owns(d, p, k)) {
        pp_runtime_cfg_set(d, p, k, v);
        schedule_done(cb, ud, 0, NULL);   /* applied + persisted synchronously */
        return;
    }
```

In `prov_is_available` (settings_fpvd.c:921), make the runtime keys available regardless of the keymap:
```c
static bool prov_is_available(const char *d, const char *p, const char *k) {
    if (pp_runtime_cfg_owns(d, p, k)) return true;
    return fpvd_keymap_lookup(d, p, k) != NULL;
}
```

In `prov_is_locked` (settings_fpvd.c:932), lock the three DVR rows while recording (color-correction rows stay editable):
```c
static bool prov_is_locked(const char *d, const char *p, const char *k) {
    if (pp_runtime_cfg_owns(d, p, k)) {
        /* DVR rows are read-only mid-recording; cc rows are always live. */
        bool is_dvr = (strcmp(p, "dvr") == 0);
        return is_dvr && pp_runtime_cfg_is_recording();
    }
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    /* ... existing dynamic-link lock logic unchanged ... */
```
(Leave the rest of the existing `prov_is_locked` body intact below the inserted block.)

- [ ] **Step 6: Run to verify pass**

Run:
```bash
nix-shell --run "cmake --build build-test --target fpvd_tests && ./build-test/fpvd_tests"
```
Expected: PASS. Also re-run `runtime_cfg_tests` to confirm no regression:
```bash
nix-shell --run "cmake --build build-test --target runtime_cfg_tests && ./build-test/runtime_cfg_tests"
```
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp CMakeLists.txt
git commit -m "fpvd: route DVR+colortrans keys to runtime-cfg; drop gs/dvr/* from keymap

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: PP page — re-apply lock state so DVR rows grey out live while recording

**Files:**
- Modify: `src/gsmenu/pages/pixelpilot.c` (poll recording state on a timer; re-apply lock state)
- Test: `tests/test_page_nav_lock.cpp` exists for lock behavior — extend it if it covers the PP page; otherwise verify manually in Task 8.

**Interfaces:**
- Consumes: `pp_runtime_cfg_is_recording` (Task 3), `pp_page_reapply_lock_state` (helper.c:33), `pp_page_group`.
- Produces: the three DVR rows transition to `PP_ROW_LOCKED_DYNAMIC` within ~0.5 s of recording starting and back to `PP_ROW_UNLOCKED` when it stops, because `prov_is_locked` (Task 5) now reports them locked while recording and the timer drives `pp_page_reapply_lock_state`.

> Why a timer: recording is toggled out-of-band (SIGUSR1), not through the settings snapshot, so nothing else re-evaluates row lock state. A lightweight 500 ms page timer that calls the existing `pp_page_reapply_lock_state` only when the recording flag changes is the minimal hook. `pp_page_reapply_lock_state` already skips rows mid-edit and rescues focus.

- [ ] **Step 1: Add the include and a recording-watch timer to the PP page**

In `src/gsmenu/pages/pixelpilot.c`, add near the top includes:
```c
#include "../settings_runtime_cfg.h"
#include "../helper.h"
```

Add this timer callback above `build_pixelpilot_tab`:
```c
/* Re-apply row lock state when DVR recording starts/stops. Recording is toggled
 * out-of-band (SIGUSR1), so poll the in-process flag and refresh only on change.
 * prov_is_locked() reports the three DVR rows locked while recording. */
static void rec_watch_timer_cb(lv_timer_t *t) {
    lv_obj_t *page = (lv_obj_t *)lv_timer_get_user_data(t);
    int *last = (int *)lv_obj_get_user_data(page);   /* tiny heap int, see builder */
    int now = pp_runtime_cfg_is_recording() ? 1 : 0;
    if (last && *last != now) {
        *last = now;
        pp_page_reapply_lock_state(page);
    }
}
```

> **Note on `lv_obj_get_user_data(page)`:** confirm the page object's user_data is otherwise unused (the row widgets store their own user_data on the row objects, not the page). If the page user_data is already used, instead allocate a small context struct `{ lv_obj_t *page; int last; }`, pass it as the timer user_data, and free it on page delete via `LV_EVENT_DELETE`. Pick whichever matches the codebase; the struct form is safer.

- [ ] **Step 2: Create the timer in the page builder**

In `build_pixelpilot_tab` (pixelpilot.c:42), just before `return page;`, add:
```c
    /* Drive recording-aware lock state for the DVR rows. */
    int *rec_last = (int *)lv_malloc(sizeof(int));
    *rec_last = pp_runtime_cfg_is_recording() ? 1 : 0;
    lv_obj_set_user_data(page, rec_last);
    lv_timer_create(rec_watch_timer_cb, 500, page);
    pp_page_reapply_lock_state(page);   /* initial state */
```

> If you chose the context-struct variant in Step 1, allocate that struct here instead and register an `LV_EVENT_DELETE` cb that frees it and deletes the timer.

- [ ] **Step 3: Build the simulator to confirm it compiles**

Run:
```bash
nix-shell --run "cmake --build build-test --target pixelpilot"
```
Expected: PASS. (In the simulator, `pp_runtime_cfg_is_recording()` returns false — ops unregistered — so DVR rows stay unlocked, which is correct for the sim.)

- [ ] **Step 4: If `tests/test_page_nav_lock.cpp` constructs the PP page, add a lock assertion**

Only if that test already builds `build_pixelpilot_tab` with a registered fake recording op: assert the three DVR rows report `PP_ROW_LOCKED_DYNAMIC` after `pp_runtime_cfg_set_ops` installs an op returning recording=1 and `pp_page_reapply_lock_state` runs; and `PP_ROW_UNLOCKED` when recording=0. If the test does not already exercise this page, skip — Task 8 covers it on hardware. Do not invent new harness scaffolding here.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/pages/pixelpilot.c tests/test_page_nav_lock.cpp
git commit -m "pp-page: grey out DVR rows live while recording via reapply-lock timer

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Ship a default `runtime.json` and install it with the package

**Files:**
- Create: `debian/runtime.json` (the default config)
- Modify: packaging install rule (`debian/pixelpilot-rk.install` if present, else CMake `install(...)`) to place it at `/etc/pixelpilot/runtime.json`

**Interfaces:**
- Consumes: the JSON schema from Global Constraints.
- Produces: a default file at the runtime path so first boot always has a valid config (decision 6 requires the file to exist).

- [ ] **Step 1: Create the default config file**

Create `debian/runtime.json`:
```json
{
  "dvr": { "mode": "raw", "maxSizeMb": 4000, "reencBitrateKbps": 8000 },
  "colorCorrection": { "enabled": false, "gain": 25, "offset": -15 }
}
```

- [ ] **Step 2: Install it to `/etc/pixelpilot/`**

Inspect how the package installs config files:
```bash
ls debian/*.install 2>/dev/null; grep -rn "etc/pixelpilot\|\.install\|install(" debian/ CMakeLists.txt | head
```
Then add the install rule matching the existing pattern. If a `debian/pixelpilot-rk.install` file exists, add a line:
```
debian/runtime.json etc/pixelpilot
```
Otherwise add to `CMakeLists.txt` in the device (non-simulator) install section:
```cmake
  install(FILES debian/runtime.json DESTINATION /etc/pixelpilot)
```

> Use `conffiles` semantics if the packaging supports it so a user-edited config is not clobbered on upgrade — follow whatever the repo already does for `*.default`/config files. If unclear, leave a plain install and note it for review rather than guessing.

- [ ] **Step 3: Verify the file is valid JSON and matches the loader**

Run:
```bash
nix-shell --run "python3 -c 'import json;print(json.load(open(\"debian/runtime.json\")))'"
```
Expected: prints the dict without error.

- [ ] **Step 4: Commit**

```bash
git add debian/runtime.json debian/pixelpilot-rk.install CMakeLists.txt
git commit -m "packaging: ship default /etc/pixelpilot/runtime.json

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Device build + on-hardware verification

**Files:** none (verification only).

**Interfaces:** Consumes the full feature. Produces evidence the feature works end-to-end on the GS.

> Reference the GS deploy workflow memory note for the exact cross-build + deploy commands and the temp `/tmp` vs persistent `/usr` deploy choice. The drone is `root@192.168.10.152`, the GS is `root@10.18.0.1` (passwordless SSH).

- [ ] **Step 1: Cross-build the device binary**

Build the real (non-simulator) target per the GS deploy workflow (nix stdin-only cross-build). Expected: clean build — confirms `main.cpp` links `pp_colortrans_apply`, `dvr_is_recording`, and the runtime-cfg module on the device toolchain.

- [ ] **Step 2: Deploy and confirm the default config loads**

Deploy to the GS, ensure `/etc/pixelpilot/runtime.json` exists (from the package or copy the default), launch pixelpilot, and check the log line `runtime.json loaded: dvr_mode=... gain=... offset=...`.

- [ ] **Step 3: Color-correction live tuning (display + recording)**

In the PP menu, toggle **Color correction** on and adjust **Gain**/**Offset**. Expected: the live feed visibly changes immediately (no restart). Confirm `/etc/pixelpilot/runtime.json` updates (`colorCorrection.enabled/gain/offset`). Start a recording (`kill -USR1 $(pidof pixelpilot)`), confirm the re-encoded DVR file also shows the correction (per the DVR recording test memory note: find the new file by sequence, not `ls -t`).

- [ ] **Step 4: DVR settings apply without restart, and lock while recording**

Change **Mode** / **Max file size** / **Re-encode bitrate**; confirm each persists to the JSON and takes effect on the next recording. Start recording, open the PP menu, and confirm the three DVR rows are **greyed out** (locked) and editing them is refused; stop recording and confirm they re-enable within ~0.5 s. Confirm color-correction rows stay editable during recording.

- [ ] **Step 5: Persistence across restart**

Set non-default values, restart pixelpilot, and confirm the values are restored from the JSON (menu shows them; log line reflects them).

- [ ] **Step 6: Record the results**

Note the outcomes (pass/fail per step, with the JSON contents and any ffprobe/geq evidence) in the PR description. If the chosen JSON path was not writable/persistent on the image, update the `#define` in `settings_runtime_cfg.c` and the install destination, then re-verify.

---

## Notes for the implementer

- Run the full host test sweep before opening the PR:
  ```bash
  nix-shell --run "cmake -S . -B build-test -DUSE_SIMULATOR=ON >/dev/null && \
    cmake --build build-test --target runtime_cfg_tests fpvd_tests settings_tests && \
    ./build-test/runtime_cfg_tests && ./build-test/fpvd_tests && ./build-test/settings_tests"
  ```
- The `gs/dvr/dvr_mode` dropdown in the PP page still offers only `raw\nreencode` (pixelpilot also supports `both`, reachable by editing the JSON). Leaving the dropdown as-is is intentional and in scope; do not expand it without a separate decision.
- The DVR **Enabled** toggle row (`gs/dvr/rec_enabled`) is explicitly out of scope — leave it as the existing SIGUSR1-driven runtime toggle.
