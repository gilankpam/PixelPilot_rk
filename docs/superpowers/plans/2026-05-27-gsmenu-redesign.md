# GSMenu Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current `lv_menu`-based GSMenu with a flat, DJI-inspired tabbed UI built from a small set of custom widgets, and put a settings-provider abstraction between widgets and persistence so the `gsmenu.sh` backend can be replaced later.

**Architecture:** Custom LVGL widgets under `src/gsmenu/widgets/` (tabbar, page, row, toggle, slider, dropdown, drilldown, section header) compose into 5 flat tabs (Camera, Link, Display, DVR, System). The current `gsmenu.sh` shell-out backend is removed; widgets call a new `pp_settings_*` interface whose only initial provider is a stub.

**Tech Stack:** C99, LVGL 9.x, Catch2 (unit tests for settings dispatcher only), SDL2 simulator on macOS / DRM on Rockchip.

**Spec:** `docs/superpowers/specs/2026-05-27-gsmenu-redesign-design.md`

---

## File Structure

```
src/gsmenu/
├── settings.h                NEW   — pp_settings_* interface
├── settings.c                NEW   — dispatch + register
├── settings_stub.c           NEW   — only initial provider; LV_LOG_USER prints
├── styles.{h,c}              REWRITE — tokens from Visual spec
├── ui.{h,c}                  SLIM    — globals + screen-toggle glue only
├── helper.{h,c}              KEEP    — find_resource_file, reload helpers
├── executor.{h,c}            MIGRATE → DELETE in Phase 7
├── widgets/                  NEW
│   ├── pp_tabbar.{h,c}
│   ├── pp_page.{h,c}
│   ├── pp_section_header.{h,c}
│   ├── pp_row.{h,c}
│   ├── pp_toggle.{h,c}
│   ├── pp_slider.{h,c}
│   ├── pp_dropdown.{h,c}
│   └── pp_drilldown.{h,c}
├── pages/                    NEW
│   ├── camera.c              ← replaces air_camera*.{c,h}
│   ├── link.c                ← replaces air_wfbng/alink/aalink/txprofiles + gs_wfbng/apfpv
│   ├── display.c             ← replaces parts of gs_system.c
│   ├── dvr.c                 ← replaces gs_dvr/dvrplayer + parts of gs_system.c
│   └── system.c              ← replaces gs_main + gs_wifi + gs_connection_checker + air_actions + gs_actions + air_telemetry + parts of gs_system.c
└── (deleted) air_*.{c,h}, gs_*.{c,h} except helper/styles/ui

src/menu.c                    REWRITE — uses pp_tabbar + 5 pp_page; no lv_menu

tests/test_settings.cpp       NEW   — Catch2 tests for settings dispatcher

CMakeLists.txt                ADJUST — new sources, new test target
```

Each `pp_*` widget is ~50-150 LOC and has one job. Pages are flat builders that compose widgets — no inheritance, no LVGL menu API.

---

## Phase 1 — Settings provider scaffolding (build keeps passing)

The old menu keeps working throughout this phase. We add the new interface, register a stub, then migrate `executor.c`'s `generic_*_event_cb` functions to call into the new interface — preserving their signatures so existing pages still compile.

### Task 1: Define the `pp_settings` interface

**Files:**
- Create: `src/gsmenu/settings.h`

- [ ] **Step 1: Write the header**

`src/gsmenu/settings.h`:
```c
#ifndef PP_SETTINGS_H
#define PP_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a real backend impl when an async set completes.
 * rc == 0 means success; err is NULL on success or a short message on failure. */
typedef void (*pp_settings_done_cb)(int rc, const char *err);

typedef struct {
    /* Synchronous set. Backend may persist immediately or queue. */
    void  (*set)(const char *domain, const char *page,
                 const char *key, const char *value);

    /* Synchronous get. Returns a heap-allocated string (caller free()s)
     * or NULL if the key is unknown. Empty string ("") means "known but unset". */
    char *(*get)(const char *domain, const char *page,
                 const char *key);

    /* Asynchronous set for slow backends. on_done may be NULL.
     * The implementation is responsible for thread safety; it may call
     * on_done synchronously if the operation is cheap. */
    void  (*set_async)(const char *domain, const char *page,
                       const char *key, const char *value,
                       pp_settings_done_cb on_done);
} pp_settings_provider_t;

/* Install (or replace) the active provider. Pointer must outlive the program. */
void pp_settings_register(const pp_settings_provider_t *provider);

/* Convenience wrappers around the registered provider. Safe to call before
 * registration (set is a no-op, get returns NULL). */
void  pp_settings_set(const char *domain, const char *page,
                      const char *key, const char *value);
char *pp_settings_get(const char *domain, const char *page,
                      const char *key);
void  pp_settings_set_async(const char *domain, const char *page,
                            const char *key, const char *value,
                            pp_settings_done_cb on_done);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/gsmenu/settings.h
git commit -m "feat(gsmenu): define pp_settings provider interface"
```

---

### Task 2: Implement the dispatcher + unit tests

**Files:**
- Create: `src/gsmenu/settings.c`
- Create: `tests/test_settings.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/test_settings.cpp`:
```cpp
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "gsmenu/settings.h"
}

static int   set_calls = 0;
static int   set_async_calls = 0;
static char  last_v[64];

static void  rec_set(const char *, const char *, const char *, const char *v) {
    set_calls++;
    snprintf(last_v, sizeof last_v, "%s", v ? v : "");
}
static char *rec_get(const char *d, const char *p, const char *k) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s/%s/%s", d, p, k);
    return strdup(buf);
}
static void  rec_set_async(const char *, const char *, const char *,
                           const char *, pp_settings_done_cb cb) {
    set_async_calls++;
    if (cb) cb(0, NULL);
}

static const pp_settings_provider_t rec_provider = {
    rec_set, rec_get, rec_set_async,
};

TEST_CASE("dispatch: set forwards to provider") {
    set_calls = 0;
    pp_settings_register(&rec_provider);
    pp_settings_set("air", "camera", "bitrate", "25");
    REQUIRE(set_calls == 1);
    REQUIRE(std::strcmp(last_v, "25") == 0);
}

TEST_CASE("dispatch: get returns caller-owned string") {
    pp_settings_register(&rec_provider);
    char *v = pp_settings_get("a", "b", "c");
    REQUIRE(v != nullptr);
    REQUIRE(std::strcmp(v, "a/b/c") == 0);
    free(v);
}

TEST_CASE("dispatch: set_async forwards") {
    set_async_calls = 0;
    pp_settings_register(&rec_provider);
    int rc_seen = -1;
    pp_settings_set_async("d", "p", "k", "v", [](int rc, const char *){
        /* lambda captures static below */
    });
    REQUIRE(set_async_calls == 1);
}

TEST_CASE("dispatch: no provider registered is safe") {
    pp_settings_register(nullptr);
    pp_settings_set("a", "b", "c", "d");          /* no crash */
    REQUIRE(pp_settings_get("a", "b", "c") == nullptr);
    pp_settings_set_async("a", "b", "c", "d", nullptr);
}
```

- [ ] **Step 2: Run tests — they should fail with link errors**

Tests can't build yet because `settings.c` doesn't exist. We'll add the build target in Task 3. This step is just to confirm the test file compiles syntactically — skip running for now and move to Step 3.

- [ ] **Step 3: Write the dispatcher**

`src/gsmenu/settings.c`:
```c
#include "settings.h"
#include <stddef.h>

static const pp_settings_provider_t *g_provider = NULL;

void pp_settings_register(const pp_settings_provider_t *provider) {
    g_provider = provider;
}

void pp_settings_set(const char *domain, const char *page,
                     const char *key, const char *value) {
    if (g_provider && g_provider->set) {
        g_provider->set(domain, page, key, value);
    }
}

char *pp_settings_get(const char *domain, const char *page, const char *key) {
    if (g_provider && g_provider->get) {
        return g_provider->get(domain, page, key);
    }
    return NULL;
}

void pp_settings_set_async(const char *domain, const char *page,
                           const char *key, const char *value,
                           pp_settings_done_cb on_done) {
    if (g_provider && g_provider->set_async) {
        g_provider->set_async(domain, page, key, value, on_done);
    } else if (g_provider && g_provider->set) {
        /* Fall back to sync set + immediate callback. */
        g_provider->set(domain, page, key, value);
        if (on_done) on_done(0, NULL);
    } else if (on_done) {
        on_done(-1, "no provider registered");
    }
}
```

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/settings.c tests/test_settings.cpp
git commit -m "feat(gsmenu): implement settings dispatcher with unit tests"
```

---

### Task 3: Add `settings_tests` build target

**Files:**
- Modify: `CMakeLists.txt:174-178` (end of `if(USE_SIMULATOR)` block)

- [ ] **Step 1: Add the test target**

Add inside the `if(USE_SIMULATOR)` block in `CMakeLists.txt`, immediately after `add_executable(${PROJECT_NAME} ${SIMULATOR_SOURCES})`:

```cmake
  # Standalone tests for the settings dispatcher — links only what it needs,
  # avoiding the heavy rockchip/drm/gst deps that pixelpilot_tests pulls in.
  find_package(Catch2 QUIET)
  if(Catch2_FOUND)
    add_executable(settings_tests
      src/gsmenu/settings.c
      tests/test_settings.cpp)
    target_include_directories(settings_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(settings_tests Catch2::Catch2WithMain)
  endif()
```

- [ ] **Step 2: Add Catch2 to the simulator nix-shell**

Modify `shell-sim.nix`:
```nix
{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = [
    pkgs.cmake
    pkgs.pkg-config
    pkgs.SDL2
    pkgs.libpng
    pkgs.cairo
    pkgs.spdlog
    pkgs.fmt
    pkgs.catch2_3
  ];
}
```

- [ ] **Step 3: Reconfigure and build the tests**

```bash
nix-shell shell-sim.nix --run "cmake -DUSE_SIMULATOR=ON -S . -B build_sim && cmake --build build_sim --target settings_tests -j8"
```
Expected: builds `build_sim/settings_tests`.

- [ ] **Step 4: Run tests**

```bash
./build_sim/settings_tests --reporter compact
```
Expected: all assertions pass; `===============================================================================` followed by `All tests passed (N assertions in M test cases)`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt shell-sim.nix
git commit -m "build(gsmenu): add settings_tests target under USE_SIMULATOR"
```

---

### Task 4: Create the stub provider

**Files:**
- Create: `src/gsmenu/settings_stub.c`

- [ ] **Step 1: Write the stub**

`src/gsmenu/settings_stub.c`:
```c
#include "settings.h"
#include "lvgl/lvgl.h"
#include <stdlib.h>
#include <string.h>

static void stub_set(const char *d, const char *p, const char *k, const char *v) {
    LV_LOG_USER("settings.set %s/%s/%s = %s", d, p, k, v ? v : "(null)");
}

static char *stub_get(const char *d, const char *p, const char *k) {
    (void)d; (void)p; (void)k;
    /* Empty string means "known but unset" — widgets render their placeholder. */
    return strdup("");
}

static void stub_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb on_done) {
    LV_LOG_USER("settings.set_async %s/%s/%s = %s", d, p, k, v ? v : "(null)");
    if (on_done) on_done(0, NULL);
}

static const pp_settings_provider_t g_stub = {
    .set = stub_set,
    .get = stub_get,
    .set_async = stub_set_async,
};

void pp_settings_register_stub(void) {
    pp_settings_register(&g_stub);
}
```

- [ ] **Step 2: Declare the registration helper**

Append to `src/gsmenu/settings.h` before the closing `#ifdef __cplusplus`:
```c
/* Registers the built-in no-op stub provider. */
void pp_settings_register_stub(void);
```

- [ ] **Step 3: Add to build**

In `CMakeLists.txt`, add `src/gsmenu/settings.c` and `src/gsmenu/settings_stub.c` to **both** `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`. Place them near the other `src/gsmenu/` entries.

- [ ] **Step 4: Build to confirm it compiles**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8"
```
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_stub.c src/gsmenu/settings.h CMakeLists.txt
git commit -m "feat(gsmenu): add stub settings provider"
```

---

### Task 5: Register the stub at startup

**Files:**
- Modify: `src/simulator.c` (after the existing stubs, in `main()` before `lv_init()`)
- Modify: `src/main.cpp` (in `main()` before any menu setup)

- [ ] **Step 1: Wire stub in simulator**

In `src/simulator.c`, add include near the top:
```c
#include "gsmenu/settings.h"
```

In `main()`, add before `lv_log_register_print_cb(my_log_cb);`:
```c
    pp_settings_register_stub();
```

- [ ] **Step 2: Wire stub in main.cpp**

In `src/main.cpp`, add include:
```c
#include "gsmenu/settings.h"
```

In `main()`, register the stub once near the top of the function (a real backend will replace this later):
```cpp
    pp_settings_register_stub();
```

If `main.cpp` is large, search for any existing `lv_init` or LVGL bootstrap and place the registration just before it.

- [ ] **Step 3: Build and run sim**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: sim launches, opens the SDL window, menu still works (existing pages). No new errors.

- [ ] **Step 4: Commit**

```bash
git add src/simulator.c src/main.cpp
git commit -m "feat(gsmenu): register stub settings provider at startup"
```

---

### Task 6: Migrate `executor.c` to call `pp_settings_set_async`

Keep the existing `generic_*_event_cb` symbols (old pages still call them), but delete their shell-out internals and replace with one-line calls into the new dispatcher. Also delete the spinner / msgbox / worker-thread machinery — the stub is synchronous so we don't need it.

**Files:**
- Modify: `src/gsmenu/executor.c` (whole file)
- Modify: `src/gsmenu/executor.h` (drop unused types)

- [ ] **Step 1: Read what's there**

```bash
wc -l src/gsmenu/executor.c src/gsmenu/executor.h
grep -n "^[a-zA-Z_].*(" src/gsmenu/executor.c
```
Note function signatures so the new versions match.

- [ ] **Step 2: Rewrite `executor.c`**

Replace the entire contents of `src/gsmenu/executor.c` with:

```c
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "executor.h"
#include "helper.h"
#include "ui.h"
#include "settings.h"

extern lv_indev_t *indev_drv;
extern lv_group_t *default_group;

void generic_switch_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    const char *value = lv_obj_has_state(target, LV_STATE_CHECKED) ? "on" : "off";
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, value, NULL);
}

void generic_checkbox_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    const char *value = lv_obj_has_state(target, LV_STATE_CHECKED) ? "on" : "off";
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, value, NULL);
}

void generic_dropdown_event_cb(lv_event_t *e) {
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) return;

    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    char buf[128];
    lv_dropdown_get_selected_str(target, buf, sizeof buf);
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, buf, NULL);
}

void generic_slider_event_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    thread_data_t *ud = (thread_data_t *)lv_event_get_user_data(e);
    int32_t v = lv_slider_get_value(target);
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)v);
    pp_settings_set_async(ud->menu_page_data->type,
                          ud->menu_page_data->page,
                          ud->parameter, buf, NULL);
}

/* Legacy entry points kept for source compatibility during the migration.
 * They no longer spawn threads or show spinners — the stub provider is
 * synchronous. Real persistence will be re-introduced via a real
 * pp_settings_provider implementation, not here. */

char *run_command(const char *command) {
    LV_LOG_USER("run_command (no-op): %s", command);
    char *out = (char *)malloc(1);
    if (out) out[0] = '\0';
    return out;
}

void run_command_and_block(lv_event_t *e, const char *command,
                           callback_fn callback) {
    (void)e;
    LV_LOG_USER("run_command_and_block (no-op): %s", command);
    if (callback) callback();
}
```

- [ ] **Step 3: Trim `executor.h`**

Replace `src/gsmenu/executor.h` contents with:
```c
#ifndef PP_EXECUTOR_H
#define PP_EXECUTOR_H

#include <lvgl.h>
#include "helper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*callback_fn)(void);

void generic_switch_event_cb(lv_event_t *e);
void generic_checkbox_event_cb(lv_event_t *e);
void generic_dropdown_event_cb(lv_event_t *e);
void generic_slider_event_cb(lv_event_t *e);

char *run_command(const char *command);
void  run_command_and_block(lv_event_t *e, const char *command,
                            callback_fn callback);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 4: Build**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8"
```
Expected: clean build. Any missing symbol means an existing page used a callback we removed — restore that one signature in `executor.h`/`executor.c` (route it through `pp_settings_set_async`).

- [ ] **Step 5: Smoke run**

```bash
nix-shell shell-sim.nix --run "./build_sim/pixelpilot"
```
In the SDL window: open menu (D), navigate down (S) into a setting that has a toggle, press D to toggle. The simulator's terminal should print `[User] settings.set_async air/<page>/<key> = on` (or `off`).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/executor.c src/gsmenu/executor.h
git commit -m "refactor(gsmenu): route executor callbacks through pp_settings"
```

---

## Phase 2 — Visual style tokens

### Task 7: Rewrite `styles.c` with the new tokens

The new widgets will reference these style names. Old styles stay in place (some old menu code still uses them); we just add the new ones alongside.

**Files:**
- Modify: `src/gsmenu/styles.h`
- Modify: `src/gsmenu/styles.c`

- [ ] **Step 1: Declare the new styles**

Add to `src/gsmenu/styles.h` before the closing `#endif`:
```c
/* New widget style tokens (Visual spec in 2026-05-27 design). */
extern lv_style_t pp_style_panel;        /* main panel bg */
extern lv_style_t pp_style_tabbar;       /* tab strip bg */
extern lv_style_t pp_style_tab;          /* tab item base */
extern lv_style_t pp_style_tab_active;   /* tab item active */
extern lv_style_t pp_style_section_hdr;  /* uppercase tracked label */
extern lv_style_t pp_style_row;          /* row base */
extern lv_style_t pp_style_row_focus;    /* row when focused */
extern lv_style_t pp_style_value_focus;  /* row value color when focused */
extern lv_style_t pp_style_divider;      /* row bottom border */
extern lv_style_t pp_style_switch_on;    /* lv_switch indicator on-color */
```

- [ ] **Step 2: Initialize them**

Add to `src/gsmenu/styles.c` inside `style_init()`, before `return 0;`:
```c
    /* Color tokens */
    const lv_color_t c_panel   = lv_color_hex(0x0F1116);
    const lv_color_t c_tabbar  = lv_color_hex(0x000000);
    const lv_color_t c_text    = lv_color_hex(0xFFFFFF);
    const lv_color_t c_accent  = lv_color_hex(0x6B7FFF);
    const lv_color_t c_accentd = lv_color_hex(0x4C60D8);

    lv_style_init(&pp_style_panel);
    lv_style_set_bg_color(&pp_style_panel, c_panel);
    lv_style_set_bg_opa(&pp_style_panel, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_panel, 0);
    lv_style_set_radius(&pp_style_panel, 0);
    lv_style_set_pad_all(&pp_style_panel, 0);

    lv_style_init(&pp_style_tabbar);
    lv_style_set_bg_color(&pp_style_tabbar, c_tabbar);
    lv_style_set_bg_opa(&pp_style_tabbar, lv_opa(30));
    lv_style_set_border_side(&pp_style_tabbar, LV_BORDER_SIDE_RIGHT);
    lv_style_set_border_color(&pp_style_tabbar, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_tabbar, lv_opa(13));   /* ≈5% */
    lv_style_set_border_width(&pp_style_tabbar, 1);
    lv_style_set_pad_ver(&pp_style_tabbar, 16);
    lv_style_set_radius(&pp_style_tabbar, 0);

    lv_style_init(&pp_style_tab);
    lv_style_set_bg_opa(&pp_style_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&pp_style_tab, c_text);
    lv_style_set_text_opa(&pp_style_tab, lv_opa(115));      /* ≈45% */
    lv_style_set_pad_ver(&pp_style_tab, 12);
    lv_style_set_radius(&pp_style_tab, 0);
    lv_style_set_border_width(&pp_style_tab, 0);

    lv_style_init(&pp_style_tab_active);
    lv_style_set_text_color(&pp_style_tab_active, c_accent);
    lv_style_set_bg_color(&pp_style_tab_active, c_accentd);
    lv_style_set_bg_opa(&pp_style_tab_active, lv_opa(31));  /* ≈12% */

    lv_style_init(&pp_style_section_hdr);
    lv_style_set_text_color(&pp_style_section_hdr, c_text);
    lv_style_set_text_opa(&pp_style_section_hdr, lv_opa(102));  /* ≈40% */
    lv_style_set_text_letter_space(&pp_style_section_hdr, 2);
    lv_style_set_pad_top(&pp_style_section_hdr, 8);
    lv_style_set_pad_left(&pp_style_section_hdr, 20);
    lv_style_set_pad_bottom(&pp_style_section_hdr, 4);

    lv_style_init(&pp_style_row);
    lv_style_set_bg_opa(&pp_style_row, LV_OPA_TRANSP);
    lv_style_set_pad_hor(&pp_style_row, 20);
    lv_style_set_pad_ver(&pp_style_row, 8);
    lv_style_set_text_color(&pp_style_row, c_text);
    lv_style_set_border_side(&pp_style_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&pp_style_row, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_row, lv_opa(13));     /* ≈5% */
    lv_style_set_border_width(&pp_style_row, 1);
    lv_style_set_radius(&pp_style_row, 0);

    lv_style_init(&pp_style_row_focus);
    lv_style_set_bg_color(&pp_style_row_focus, c_accentd);
    lv_style_set_bg_opa(&pp_style_row_focus, lv_opa(31));   /* ≈12% */
    lv_style_set_border_side(&pp_style_row_focus, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&pp_style_row_focus, c_accent);
    lv_style_set_border_opa(&pp_style_row_focus, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_row_focus, 2);
    lv_style_set_pad_left(&pp_style_row_focus, 18);         /* compensate 2px border */

    lv_style_init(&pp_style_value_focus);
    lv_style_set_text_color(&pp_style_value_focus, c_accent);

    lv_style_init(&pp_style_divider);
    /* placeholder — already implemented via pp_style_row border side */
    lv_style_set_border_width(&pp_style_divider, 0);

    lv_style_init(&pp_style_switch_on);
    lv_style_set_bg_color(&pp_style_switch_on, c_accent);
```

Add the corresponding `lv_style_t` definitions at the top of `src/gsmenu/styles.c`:
```c
lv_style_t pp_style_panel;
lv_style_t pp_style_tabbar;
lv_style_t pp_style_tab;
lv_style_t pp_style_tab_active;
lv_style_t pp_style_section_hdr;
lv_style_t pp_style_row;
lv_style_t pp_style_row_focus;
lv_style_t pp_style_value_focus;
lv_style_t pp_style_divider;
lv_style_t pp_style_switch_on;
```

- [ ] **Step 3: Build**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8"
```
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/styles.c src/gsmenu/styles.h
git commit -m "feat(gsmenu): add pp_style_* tokens for new widget set"
```

---

## Phase 3 — Widget primitives

Each widget gets its own file. We build a throwaway **widget demo page** inside the simulator first (Task 8), then add each widget to it and smoke-verify before moving on.

### Task 8: Widget demo page in simulator

**Files:**
- Modify: `src/simulator.c`

- [ ] **Step 1: Add a demo-toggle env switch**

In `src/simulator.c::main`, **before** `pp_menu_main();`, add:
```c
    extern void pp_widget_demo_main(void);
    if (getenv("PP_WIDGET_DEMO")) {
        pp_widget_demo_main();
        while (1) { handle_keyboard_input(); lv_task_handler(); usleep(5000); }
    }
```

This lets us run the demo via `PP_WIDGET_DEMO=1 ./build_sim/pixelpilot` without touching the existing menu path.

- [ ] **Step 2: Create the demo file**

`src/gsmenu/widget_demo.c`:
```c
#include <lvgl.h>
#include "styles.h"

/* Defined here; subsequent widget tasks append to demo_root via demo_add(). */
static lv_obj_t *demo_root = NULL;

lv_obj_t *demo_root_obj(void) { return demo_root; }

void pp_widget_demo_main(void) {
    style_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    demo_root = lv_obj_create(scr);
    lv_obj_set_size(demo_root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(demo_root, 0, 0);
    lv_obj_add_style(demo_root, &pp_style_panel, 0);
    lv_obj_set_flex_flow(demo_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(demo_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(demo_root, LV_DIR_VER);

    /* Subsequent widget tasks add demo rows here via demo_root_obj(). */
}
```

- [ ] **Step 3: Wire into CMake**

Add `src/gsmenu/widget_demo.c` to `SIMULATOR_SOURCES` in `CMakeLists.txt`.

- [ ] **Step 4: Build + run**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: SDL window opens showing a dark panel covering 78% of the screen on the left. No interactive content yet — that's what we add next.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widget_demo.c src/simulator.c CMakeLists.txt
git commit -m "feat(gsmenu): bootstrap widget demo scaffold in simulator"
```

---

### Task 9: `pp_section_header`

**Files:**
- Create: `src/gsmenu/widgets/pp_section_header.h`
- Create: `src/gsmenu/widgets/pp_section_header.c`

- [ ] **Step 1: Write the header**

`src/gsmenu/widgets/pp_section_header.h`:
```c
#ifndef PP_SECTION_HEADER_H
#define PP_SECTION_HEADER_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *pp_section_header(lv_obj_t *parent, const char *text);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the impl**

`src/gsmenu/widgets/pp_section_header.c`:
```c
#include "pp_section_header.h"
#include "../styles.h"
#include <ctype.h>
#include <string.h>

lv_obj_t *pp_section_header(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &pp_style_section_hdr, 0);

    /* Uppercase the text. */
    size_t n = strlen(text);
    char *upper = malloc(n + 1);
    for (size_t i = 0; i < n; i++) upper[i] = (char)toupper((unsigned char)text[i]);
    upper[n] = '\0';
    lv_label_set_text(label, upper);
    free(upper);

    lv_obj_set_width(label, LV_PCT(100));
    return label;
}
```

- [ ] **Step 3: Use it in the demo page**

In `src/gsmenu/widget_demo.c` add at the top:
```c
#include "widgets/pp_section_header.h"
```

Inside `pp_widget_demo_main()` after creating `demo_root`:
```c
    pp_section_header(demo_root, "Video");
    pp_section_header(demo_root, "Image");
    pp_section_header(demo_root, "ISP");
```

- [ ] **Step 4: Build and add to CMake**

Add `src/gsmenu/widgets/pp_section_header.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES` in `CMakeLists.txt`. Build:
```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: panel shows three uppercase headers `VIDEO`, `IMAGE`, `ISP` stacked vertically with tracking.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_section_header.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_section_header widget"
```

---

### Task 10: `pp_row` (text/value row)

**Files:**
- Create: `src/gsmenu/widgets/pp_row.h`
- Create: `src/gsmenu/widgets/pp_row.c`

- [ ] **Step 1: Write the header**

`src/gsmenu/widgets/pp_row.h`:
```c
#ifndef PP_ROW_H
#define PP_ROW_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Creates a focusable horizontal row.
 *   icon_text: NULL or an LV_SYMBOL_* constant
 *   label:     left-aligned text
 *   key:       settings key (used to read the value via pp_settings_get).
 *              May be NULL for static rows (no value reload).
 *
 * The row stores its (domain, page, key) triple in user_data so other
 * widgets (toggle/slider/dropdown) can build on top. */
lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key);

/* Update a row's value label to a new string. */
void pp_row_set_value(lv_obj_t *row, const char *value);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the impl**

`src/gsmenu/widgets/pp_row.c`:
```c
#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *domain;
    const char *page;
    char       *key;
    lv_obj_t   *value_label;
} pp_row_data_t;

static void row_delete_cb(lv_event_t *e) {
    pp_row_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->key); free(d); }
}

static void row_focus_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    pp_row_data_t *d = lv_event_get_user_data(e);
    if (d && d->value_label) {
        lv_obj_add_style(d->value_label, &pp_style_value_focus,
                         LV_STATE_FOCUS_KEY);
    }
}

lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_flex_grow(label_obj, 1);

    pp_row_data_t *d = calloc(1, sizeof(*d));
    /* Inherited from parent page via user_data; see pp_page in next task.
     * Until pp_page is wired, we leave domain/page NULL — get/set become no-ops. */
    lv_obj_t *page_ud_carrier = parent_page;
    d->domain = NULL;
    d->page = NULL;
    d->key = key ? strdup(key) : NULL;

    d->value_label = lv_label_create(row);
    lv_label_set_text(d->value_label, "—");

    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, row_focus_cb, LV_EVENT_FOCUSED, d);

    /* Initial value read — safe even with NULL domain (returns NULL). */
    if (d->key) {
        char *v = pp_settings_get(d->domain, d->page, d->key);
        if (v && *v) lv_label_set_text(d->value_label, v);
        free(v);
    }

    (void)page_ud_carrier;
    return row;
}

void pp_row_set_value(lv_obj_t *row, const char *value) {
    pp_row_data_t *d = lv_obj_get_user_data(row);
    if (d && d->value_label) lv_label_set_text(d->value_label, value);
}
```

- [ ] **Step 3: Use it in the demo page**

Append to `demo_root` after the headers in `widget_demo.c`:
```c
#include "widgets/pp_row.h"
```
and:
```c
    pp_row_text(demo_root, LV_SYMBOL_SETTINGS, "Version", NULL);
    pp_row_text(demo_root, LV_SYMBOL_SETTINGS, "Disk",    NULL);
    pp_row_text(demo_root, LV_SYMBOL_VIDEO,    "Codec",   "codec");
```

- [ ] **Step 4: Add to CMake**

Add `src/gsmenu/widgets/pp_row.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: three rows render under the section headers, each with icon + label + `—`. Pressing W/S in the SDL window highlights rows with the blue left border + tint.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/widgets/pp_row.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_row text/value widget"
```

---

### Task 11: `pp_toggle`

**Files:**
- Create: `src/gsmenu/widgets/pp_toggle.h`
- Create: `src/gsmenu/widgets/pp_toggle.c`

- [ ] **Step 1: Write the header**

`src/gsmenu/widgets/pp_toggle.h`:
```c
#ifndef PP_TOGGLE_H
#define PP_TOGGLE_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Like pp_row_text, but the value is an lv_switch. Pressing ENTER on the
 * focused row toggles the switch and calls pp_settings_set_async. */
lv_obj_t *pp_toggle(lv_obj_t *parent_page,
                    const char *icon_text,
                    const char *label,
                    const char *domain, const char *page, const char *key);

#ifdef __cplusplus
}
#endif
#endif
```

(We pass `(domain, page, key)` explicitly until Task 14 introduces `pp_page` user_data — once that lands we'll have a follow-up step that drops the explicit args.)

- [ ] **Step 2: Write the impl**

`src/gsmenu/widgets/pp_toggle.c`:
```c
#include "pp_toggle.h"
#include "../styles.h"
#include "../settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *sw;
} pp_toggle_data_t;

static void on_delete(lv_event_t *e) {
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    bool now = !lv_obj_has_state(d->sw, LV_STATE_CHECKED);
    if (now) lv_obj_add_state(d->sw, LV_STATE_CHECKED);
    else     lv_obj_remove_state(d->sw, LV_STATE_CHECKED);
    pp_settings_set_async(d->domain, d->page, d->key, now ? "on" : "off", NULL);
}

lv_obj_t *pp_toggle(lv_obj_t *parent_page,
                    const char *icon_text,
                    const char *label,
                    const char *domain, const char *page, const char *key) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_flex_grow(label_obj, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_add_style(sw, &pp_style_switch_on,
                     LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_size(sw, 40, 22);

    pp_toggle_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->sw     = sw;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    /* Initial read */
    char *v = pp_settings_get(domain, page, key);
    if (v && strcmp(v, "on") == 0) lv_obj_add_state(sw, LV_STATE_CHECKED);
    free(v);

    return row;
}
```

- [ ] **Step 3: Use it in the demo**

In `widget_demo.c`:
```c
#include "widgets/pp_toggle.h"
```
and:
```c
    pp_toggle(demo_root, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(demo_root, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
```

- [ ] **Step 4: Add to CMake**

`src/gsmenu/widgets/pp_toggle.c` → `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: two toggle rows. Pressing D on a focused toggle flips it and the simulator's terminal prints `[User] settings.set_async air/camera/mirror = on`.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/widgets/pp_toggle.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_toggle widget"
```

---

### Task 12: `pp_slider`

**Files:**
- Create: `src/gsmenu/widgets/pp_slider.{h,c}`

- [ ] **Step 1: Header**

`src/gsmenu/widgets/pp_slider.h`:
```c
#ifndef PP_SLIDER_H
#define PP_SLIDER_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/widgets/pp_slider.c`:
```c
#include "pp_slider.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"   /* control_mode */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *slider, *value_label;
} pp_slider_data_t;

static void on_delete(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void update_label(pp_slider_data_t *d) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)lv_slider_get_value(d->slider));
    lv_label_set_text(d->value_label, buf);
}

static void on_key(lv_event_t *e) {
    pp_slider_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            control_mode = GSMENU_CONTROL_MODE_SLIDER;
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[32];
            snprintf(buf, sizeof buf, "%d", (int)lv_slider_get_value(d->slider));
            pp_settings_set_async(d->domain, d->page, d->key, buf, NULL);
        }
    } else if (k == LV_KEY_RIGHT) {
        lv_slider_set_value(d->slider, lv_slider_get_value(d->slider) + 1, LV_ANIM_OFF);
        update_label(d);
    } else if (k == LV_KEY_LEFT) {
        lv_slider_set_value(d->slider, lv_slider_get_value(d->slider) - 1, LV_ANIM_OFF);
        update_label(d);
    } else if (k == LV_KEY_ESC) {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
}

lv_obj_t *pp_slider(lv_obj_t *parent_page,
                    const char *icon_text, const char *label,
                    const char *domain, const char *page, const char *key,
                    int32_t min, int32_t max) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_flex_grow(label_obj, 1);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "—");
    lv_obj_set_style_pad_right(value_label, 8, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_obj_set_width(slider, 80);
    lv_obj_add_state(slider, LV_STATE_DISABLED);    /* keys-only adjust */

    pp_slider_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->slider = slider;
    d->value_label = value_label;

    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        lv_slider_set_value(slider, atoi(v), LV_ANIM_OFF);
        update_label(d);
    }
    free(v);

    return row;
}
```

- [ ] **Step 3: Use it in the demo**

In `widget_demo.c`:
```c
#include "widgets/pp_slider.h"
```
and:
```c
    pp_slider(demo_root, LV_SYMBOL_AUDIO, "Bitrate", "air", "camera", "bitrate", 1, 50);
```

- [ ] **Step 4: CMake + Build + smoke**

Add `src/gsmenu/widgets/pp_slider.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.
```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: focus the slider row, press D (control_mode→SLIDER), W/S now adjust value (visible in label), D again commits and emits `set_async`.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_slider.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_slider widget"
```

---

### Task 13: `pp_dropdown`

**Files:**
- Create: `src/gsmenu/widgets/pp_dropdown.{h,c}`

- [ ] **Step 1: Header**

`src/gsmenu/widgets/pp_dropdown.h`:
```c
#ifndef PP_DROPDOWN_H
#define PP_DROPDOWN_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
/* options: newline-separated list, e.g. "1080p60\n720p120\n540p60" */
lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                      const char *icon_text, const char *label,
                      const char *domain, const char *page, const char *key,
                      const char *options);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/widgets/pp_dropdown.c`:
```c
#include "pp_dropdown.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *dd, *value_label;
    uint16_t saved_sel;
} pp_dd_data_t;

static void on_delete(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void refresh_label(pp_dd_data_t *d) {
    char buf[64];
    lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
    lv_label_set_text(d->value_label, buf);
}

static void on_key(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            d->saved_sel = lv_dropdown_get_selected(d->dd);
            control_mode = GSMENU_CONTROL_MODE_EDIT;
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[64];
            lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
            pp_settings_set_async(d->domain, d->page, d->key, buf, NULL);
        }
    } else if (k == LV_KEY_UP) {
        uint16_t s = lv_dropdown_get_selected(d->dd);
        if (s > 0) lv_dropdown_set_selected(d->dd, s - 1);
        refresh_label(d);
    } else if (k == LV_KEY_DOWN) {
        uint16_t s = lv_dropdown_get_selected(d->dd);
        if (s + 1 < lv_dropdown_get_option_count(d->dd))
            lv_dropdown_set_selected(d->dd, s + 1);
        refresh_label(d);
    } else if (k == LV_KEY_ESC) {
        lv_dropdown_set_selected(d->dd, d->saved_sel);
        refresh_label(d);
        control_mode = GSMENU_CONTROL_MODE_NAV;
    }
}

lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                     const char *icon_text, const char *label,
                     const char *domain, const char *page, const char *key,
                     const char *options) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, 12, 0);
    }

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_flex_grow(label_obj, 1);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "—");
    lv_obj_set_style_pad_right(value_label, 8, 0);

    /* hidden dropdown — we drive it with keys, not its own popup. */
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);

    pp_dd_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->dd     = dd;
    d->value_label = value_label;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        /* try to match by option text */
        uint16_t n = lv_dropdown_get_option_count(dd);
        char buf[64];
        for (uint16_t i = 0; i < n; i++) {
            lv_dropdown_set_selected(dd, i);
            lv_dropdown_get_selected_str(dd, buf, sizeof buf);
            if (strcmp(buf, v) == 0) break;
        }
        refresh_label(d);
    }
    free(v);

    return row;
}
```

- [ ] **Step 3: Use it in the demo**

In `widget_demo.c`:
```c
#include "widgets/pp_dropdown.h"
```
and:
```c
    pp_dropdown(demo_root, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "resolution",
                "1080p60\n720p120\n540p60");
```

- [ ] **Step 4: CMake + Build + smoke**

Add `src/gsmenu/widgets/pp_dropdown.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: focus the dropdown row, D enters EDIT, W/S cycles options (label updates), D commits, A (ESC) cancels.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_dropdown.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_dropdown widget"
```

---

### Task 14: `pp_page`

**Files:**
- Create: `src/gsmenu/widgets/pp_page.{h,c}`

- [ ] **Step 1: Header**

`src/gsmenu/widgets/pp_page.h`:
```c
#ifndef PP_PAGE_H
#define PP_PAGE_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A scrollable container that owns a focus group of its child rows.
 * (domain, page) is stored on the object and inherited via getters used
 * by future widgets that don't take the triple explicitly. */
lv_obj_t   *pp_page_create(lv_obj_t *parent,
                           const char *domain, const char *page);
lv_group_t *pp_page_group(lv_obj_t *page);
const char *pp_page_domain(lv_obj_t *page);
const char *pp_page_name(lv_obj_t *page);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/widgets/pp_page.c`:
```c
#include "pp_page.h"
#include "../styles.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page;
    lv_group_t *group;
} pp_page_data_t;

static void on_delete(lv_event_t *e) {
    pp_page_data_t *d = lv_event_get_user_data(e);
    if (!d) return;
    if (d->group) lv_group_del(d->group);
    free(d->domain); free(d->page); free(d);
}

lv_obj_t *pp_page_create(lv_obj_t *parent,
                         const char *domain, const char *page) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &pp_style_panel, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);

    pp_page_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->group  = lv_group_create();
    lv_obj_set_user_data(p, d);
    lv_obj_add_event_cb(p, on_delete, LV_EVENT_DELETE, d);
    return p;
}

lv_group_t *pp_page_group(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->group : NULL;
}
const char *pp_page_domain(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->domain : NULL;
}
const char *pp_page_name(lv_obj_t *page) {
    pp_page_data_t *d = lv_obj_get_user_data(page);
    return d ? d->page : NULL;
}
```

- [ ] **Step 3: Rewrite the demo to use a real page**

Update `widget_demo.c`'s `pp_widget_demo_main()` to create one `pp_page` and put all widgets inside it:
```c
#include "widgets/pp_page.h"
/* … other widget includes … */

void pp_widget_demo_main(void) {
    style_init();
    pp_settings_register_stub();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    lv_obj_t *page = pp_page_create(scr, "air", "camera");
    lv_obj_set_size(page, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(page, 0, 0);

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "resolution",
                "1080p60\n720p120\n540p60");
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate",
              "air", "camera", "bitrate", 1, 50);

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");

    pp_section_header(page, "Info");
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Version", NULL);

    /* Wire the page's group to the global indev so W/S navigate rows. */
    extern lv_indev_t *indev_drv;
    extern lv_group_t *default_group;
    default_group = pp_page_group(page);
    lv_indev_set_group(indev_drv, pp_page_group(page));

    /* Add every focusable row to the page group. */
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) ||
            lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(pp_page_group(page), c);
        }
    }
}
```

Note: this manual group-add will be moved into pp_page_add_row helpers in Phase 4. For now it gets us scrolling + focus working end-to-end.

- [ ] **Step 4: CMake + Build + smoke**

Add `src/gsmenu/widgets/pp_page.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: full demo page with section headers, dropdown, slider, toggles, text row — all keyboard-navigable.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_page.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_page scrollable container with focus group"
```

---

### Task 15: `pp_tabbar`

**Files:**
- Create: `src/gsmenu/widgets/pp_tabbar.{h,c}`

- [ ] **Step 1: Header**

`src/gsmenu/widgets/pp_tabbar.h`:
```c
#ifndef PP_TABBAR_H
#define PP_TABBAR_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_tabbar pp_tabbar_t;

typedef struct {
    const char *label;      /* "Camera" */
    const char *icon_text;  /* LV_SYMBOL_* */
    lv_obj_t   *page;       /* page to show when this tab is active */
} pp_tabbar_item_t;

pp_tabbar_t *pp_tabbar_create(lv_obj_t *parent,
                              const pp_tabbar_item_t *items, size_t n);

void         pp_tabbar_set_active(pp_tabbar_t *t, size_t index);
lv_group_t  *pp_tabbar_group(pp_tabbar_t *t);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/widgets/pp_tabbar.c`:
```c
#include "pp_tabbar.h"
#include "pp_page.h"
#include "../styles.h"
#include <stdlib.h>

#define PP_TAB_W 72
#define PP_TAB_H 56

struct pp_tabbar {
    lv_obj_t  *root;
    lv_group_t *group;
    pp_tabbar_item_t *items;
    size_t n;
    size_t active;
    lv_obj_t **tab_objs;
};

static void apply_active(pp_tabbar_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        if (i == t->active) {
            lv_obj_add_state(t->tab_objs[i], LV_STATE_CHECKED);
            lv_obj_add_style(t->tab_objs[i], &pp_style_tab_active, LV_STATE_CHECKED);
            lv_obj_remove_flag(t->items[i].page, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(t->tab_objs[i], LV_STATE_CHECKED);
            lv_obj_add_flag(t->items[i].page, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_focus(lv_event_t *e) {
    pp_tabbar_t *t = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    for (size_t i = 0; i < t->n; i++) {
        if (t->tab_objs[i] == target) {
            t->active = i;
            apply_active(t);
            return;
        }
    }
}

pp_tabbar_t *pp_tabbar_create(lv_obj_t *parent,
                              const pp_tabbar_item_t *items, size_t n) {
    pp_tabbar_t *t = calloc(1, sizeof(*t));
    t->n = n;
    t->items = malloc(n * sizeof(*items));
    memcpy(t->items, items, n * sizeof(*items));
    t->tab_objs = calloc(n, sizeof(*t->tab_objs));

    t->root = lv_obj_create(parent);
    lv_obj_remove_style_all(t->root);
    lv_obj_add_style(t->root, &pp_style_tabbar, 0);
    lv_obj_set_size(t->root, PP_TAB_W, LV_PCT(100));
    lv_obj_set_flex_flow(t->root, LV_FLEX_FLOW_COLUMN);

    t->group = lv_group_create();

    for (size_t i = 0; i < n; i++) {
        lv_obj_t *tab = lv_obj_create(t->root);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &pp_style_tab, 0);
        lv_obj_add_style(tab, &pp_style_tab_active, LV_STATE_CHECKED);
        lv_obj_set_size(tab, LV_PCT(100), PP_TAB_H);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *icon = lv_label_create(tab);
        lv_label_set_text(icon, items[i].icon_text ? items[i].icon_text : "");

        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, items[i].label);

        lv_obj_add_event_cb(tab, on_focus, LV_EVENT_FOCUSED, t);
        lv_group_add_obj(t->group, tab);
        t->tab_objs[i] = tab;
    }

    pp_tabbar_set_active(t, 0);
    return t;
}

void pp_tabbar_set_active(pp_tabbar_t *t, size_t i) {
    if (i >= t->n) return;
    t->active = i;
    apply_active(t);
}

lv_group_t *pp_tabbar_group(pp_tabbar_t *t) { return t->group; }
```

- [ ] **Step 3: Demo a five-tab layout**

Replace `pp_widget_demo_main` in `widget_demo.c` with a multi-tab version:

```c
#include "widgets/pp_tabbar.h"

void pp_widget_demo_main(void) {
    style_init();
    pp_settings_register_stub();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a3a2a), 0);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);

    /* Build a page per tab */
    const char *labels[5] = {"Camera","Link","Display","DVR","System"};
    const char *icons[5]  = {LV_SYMBOL_IMAGE, LV_SYMBOL_WIFI, LV_SYMBOL_EYE_OPEN,
                             LV_SYMBOL_VIDEO, LV_SYMBOL_SETTINGS};
    pp_tabbar_item_t items[5];
    for (int i = 0; i < 5; i++) {
        lv_obj_t *pg = pp_page_create(root, "gs", labels[i]);
        lv_obj_set_flex_grow(pg, 1);
        pp_section_header(pg, labels[i]);
        pp_row_text(pg, LV_SYMBOL_SETTINGS, "Placeholder", NULL);
        items[i] = (pp_tabbar_item_t){ .label=labels[i], .icon_text=icons[i], .page=pg };
    }
    pp_tabbar_t *tab = pp_tabbar_create(root, items, 5);
    /* tabbar root needs to be the first child of the row — re-order */
    lv_obj_move_to_index(/* tabbar root */ lv_obj_get_child(root, 5), 0);

    extern lv_indev_t *indev_drv;
    extern lv_group_t *default_group;
    default_group = pp_tabbar_group(tab);
    lv_indev_set_group(indev_drv, pp_tabbar_group(tab));
}
```

Note: this is a quick demo; the real `pp_menu_main` rewrite in Task 18 handles ordering cleanly.

- [ ] **Step 4: CMake + Build + smoke**

Add `src/gsmenu/widgets/pp_tabbar.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: 5-tab demo — left strip has icon + label, W/S moves focus between tabs, the right pane swaps to show that tab's placeholder content.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_tabbar.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_tabbar with lazy tab switching"
```

---

### Task 16: `pp_drilldown`

**Files:**
- Create: `src/gsmenu/widgets/pp_drilldown.{h,c}`

- [ ] **Step 1: Header**

`src/gsmenu/widgets/pp_drilldown.h`:
```c
#ifndef PP_DRILLDOWN_H
#define PP_DRILLDOWN_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pp_drilldown_build_fn)(lv_obj_t *body, void *user);

lv_obj_t *pp_drilldown_open(lv_obj_t *anchor_page, const char *title,
                            pp_drilldown_build_fn build, void *user);
void      pp_drilldown_close(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/widgets/pp_drilldown.c`:
```c
#include "pp_drilldown.h"
#include "../styles.h"
#include <stdlib.h>

static lv_obj_t   *g_overlay = NULL;
static lv_obj_t   *g_body    = NULL;
static lv_group_t *g_group   = NULL;
static lv_group_t *g_prev_group = NULL;

extern lv_indev_t *indev_drv;

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_HOME) pp_drilldown_close();
}

lv_obj_t *pp_drilldown_open(lv_obj_t *anchor_page, const char *title,
                            pp_drilldown_build_fn build, void *user) {
    if (g_overlay) pp_drilldown_close();

    lv_obj_t *parent = lv_obj_get_parent(anchor_page);
    g_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_add_style(g_overlay, &pp_style_panel, 0);
    lv_obj_set_size(g_overlay, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(g_overlay, 0, 0);

    /* dim the underlying anchor page */
    lv_obj_set_style_bg_opa(anchor_page, LV_OPA_60, 0);

    lv_obj_t *header = lv_label_create(g_overlay);
    lv_label_set_text(header, title);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(header, 12, 0);

    g_body = lv_obj_create(g_overlay);
    lv_obj_remove_style_all(g_body);
    lv_obj_set_size(g_body, LV_PCT(100), LV_PCT(90));
    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_body, LV_DIR_VER);

    g_group = lv_group_create();
    g_prev_group = lv_indev_get_group(indev_drv);
    lv_indev_set_group(indev_drv, g_group);

    lv_obj_add_event_cb(g_overlay, on_key, LV_EVENT_KEY, NULL);

    if (build) build(g_body, user);
    /* Auto-add focusable body children to the drilldown group. */
    uint32_t n = lv_obj_get_child_cnt(g_body);
    for (uint32_t i = 0; i < n; i++) {
        lv_group_add_obj(g_group, lv_obj_get_child(g_body, i));
    }
    return g_overlay;
}

void pp_drilldown_close(void) {
    if (!g_overlay) return;
    if (g_prev_group) lv_indev_set_group(indev_drv, g_prev_group);
    if (g_group) lv_group_del(g_group);
    lv_obj_del(g_overlay);
    g_overlay = NULL;
    g_body = NULL;
    g_group = NULL;
}
```

- [ ] **Step 3: Smoke via demo**

In `widget_demo.c` add a row that opens a drilldown when activated. Easiest: append after the existing demo:
```c
#include "widgets/pp_drilldown.h"

static void demo_build_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Networks");
    pp_row_text(body, LV_SYMBOL_WIFI, "home-wifi",   NULL);
    pp_row_text(body, LV_SYMBOL_WIFI, "drone-2.4G",  NULL);
    pp_row_text(body, LV_SYMBOL_WIFI, "guest",       NULL);
}
```
Add a temp test key (e.g. press 'X') in `simulator.c::sdl_key_watch` that calls `pp_drilldown_open(...)` for manual verification.

Or simpler: just call `pp_drilldown_open(active_page, "WiFi Networks", demo_build_drilldown, NULL);` directly from `pp_widget_demo_main` immediately after building the page, observe it appears, press A to close, verify the underlying page returns.

- [ ] **Step 4: CMake + Build + smoke**

Add `src/gsmenu/widgets/pp_drilldown.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: drilldown panel appears on top of the underlying page; W/S moves through the 3 wifi rows; A closes it and returns focus to the previous group.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_drilldown.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add pp_drilldown overlay widget"
```

---

## Phase 4 — Real pages

### Task 17: Build the Display tab

The smallest tab — proves end-to-end wiring before we touch larger pages.

**Files:**
- Create: `src/gsmenu/pages/display.h`
- Create: `src/gsmenu/pages/display.c`

- [ ] **Step 1: Header**

`src/gsmenu/pages/display.h`:
```c
#ifndef PP_PAGE_DISPLAY_H
#define PP_PAGE_DISPLAY_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_display_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Impl**

`src/gsmenu/pages/display.c`:
```c
#include "display.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"

lv_obj_t *build_display_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "display");

    pp_section_header(page, "Output");
    pp_dropdown(page, LV_SYMBOL_EYE_OPEN, "HDMI Mode",
                "gs", "display", "hdmi_mode",
                "1920x1080@60\n1280x720@60\n1920x1080@30");
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 200);

    pp_section_header(page, "Color");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Color correction",
              "gs", "display", "color_correction");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
              "gs", "display", "cc_offset", -50, 50);

    /* Add focusable rows to the page's group. */
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICK_FOCUSABLE) ||
            lv_obj_check_type(c, &lv_obj_class)) {
            /* obj-class children include both section headers (non-focusable
             * labels) and our rows; filter by whether they accept focus. */
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) {
                lv_group_add_obj(grp, c);
            }
        }
    }
    return page;
}
```

- [ ] **Step 3: CMake**

Add `src/gsmenu/pages/display.c` to `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

- [ ] **Step 4: Smoke via demo**

Update `widget_demo.c::pp_widget_demo_main` to use `build_display_tab(scr)` for the rightmost page slot in the 5-tab demo, in place of the placeholder content.

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && PP_WIDGET_DEMO=1 ./build_sim/pixelpilot"
```
Expected: Display tab now shows real Output / Color sections with dropdown, sliders, and toggle. All interactive.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/pages/display.{c,h} src/gsmenu/widget_demo.c CMakeLists.txt
git commit -m "feat(gsmenu): add Display tab page builder"
```

---

### Task 18: Rewrite `pp_menu_main` with new tabbar + pages (Display only; others stubbed)

This replaces `lv_menu` usage in `src/menu.c`. The other four tabs are empty placeholders until subsequent tasks fill them in. After this task, the user-facing menu (not the demo) starts using the new layout.

**Files:**
- Modify: `src/menu.c` (whole file)
- Modify: `src/menu.h` if needed

- [ ] **Step 1: Read what's there**

```bash
cat src/menu.c
```
Note the existing globals (`pp_menu_screen`, etc.) — keep their names where possible so `input.cpp` etc. still link.

- [ ] **Step 2: Rewrite `menu.c`**

`src/menu.c`:
```c
#include <lvgl.h>
#include "menu.h"
#include "input.h"
#include "gsmenu/styles.h"
#include "gsmenu/widgets/pp_tabbar.h"
#include "gsmenu/widgets/pp_page.h"
#include "gsmenu/widgets/pp_section_header.h"
#include "gsmenu/widgets/pp_row.h"
#include "gsmenu/pages/display.h"

lv_obj_t *pp_menu_screen = NULL;
lv_obj_t *pp_osd_screen  = NULL;
lv_group_t *main_group   = NULL;
lv_group_t *default_group = NULL;

extern lv_indev_t *indev_drv;

static lv_obj_t *stub_page(lv_obj_t *parent, const char *name) {
    lv_obj_t *p = pp_page_create(parent, "gs", name);
    pp_section_header(p, name);
    pp_row_text(p, LV_SYMBOL_SETTINGS, "(not implemented yet)", NULL);
    return p;
}

void pp_menu_main(void) {
    style_init();

    pp_osd_screen  = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(pp_osd_screen, LV_OPA_TRANSP, 0);

    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, 0);

    lv_obj_t *root = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(78), LV_PCT(100));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);

    /* Build pages (Display is real; others stubbed until later tasks) */
    lv_obj_t *cam = stub_page(root, "Camera");
    lv_obj_t *lnk = stub_page(root, "Link");
    lv_obj_t *dsp = build_display_tab(root);
    lv_obj_t *dvr = stub_page(root, "DVR");
    lv_obj_t *sys = stub_page(root, "System");

    pp_tabbar_item_t items[5] = {
        { "Camera",  LV_SYMBOL_IMAGE,     cam },
        { "Link",    LV_SYMBOL_WIFI,      lnk },
        { "Display", LV_SYMBOL_EYE_OPEN,  dsp },
        { "DVR",     LV_SYMBOL_VIDEO,     dvr },
        { "System",  LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(root, items, 5);
    lv_obj_move_to_index(/* tabbar root */ lv_obj_get_child(root, 5), 0);

    main_group    = pp_tabbar_group(tabbar);
    default_group = main_group;

    /* Start on the OSD screen; toggle_screen swaps to menu on D. */
    lv_screen_load(pp_osd_screen);
}
```

- [ ] **Step 3: Verify `input.cpp` integration**

In `src/input.cpp::toggle_screen()`, the line `lv_indev_set_group(indev_drv, main_group);` should still work because `main_group` is now the tabbar's group. Confirm by reading lines 425-440.

- [ ] **Step 4: Drop the demo path (or keep both)**

Keep the `PP_WIDGET_DEMO` env-gated demo for ongoing widget development. The real `pp_menu_main` is what runs by default now.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected:
- Sim starts on the transparent OSD screen.
- Press D — menu opens with 5 tabs on the left.
- W/S navigates tabs; Display tab shows real content.
- Press D on Display → focus enters page rows.
- Press A → focus returns to tabbar.
- Press A again on tabbar → returns to OSD screen.

- [ ] **Step 6: Commit**

```bash
git add src/menu.c
git commit -m "refactor(gsmenu): replace lv_menu with pp_tabbar+pp_page (Display live; others stub)"
```

---

### Task 19: Build the Camera tab

Port settings from today's `air_camera_video/image/isp/fpv/recording` files into one long page with section headers.

**Files:**
- Create: `src/gsmenu/pages/camera.{h,c}`
- Delete (Step 5): `src/gsmenu/air_camera*.{c,h}` (6 files)

- [ ] **Step 1: Read the old builders**

```bash
ls src/gsmenu/air_camera*
grep -n "create_text\|create_dropdown\|create_slider\|create_switch" \
     src/gsmenu/air_camera_video.c \
     src/gsmenu/air_camera_image.c \
     src/gsmenu/air_camera_isp.c \
     src/gsmenu/air_camera_fpv.c \
     src/gsmenu/air_camera_recording.c | head -40
```
Note each setting's `parameter` string and control type (these become the `key` arg).

- [ ] **Step 2: Header**

`src/gsmenu/pages/camera.h`:
```c
#ifndef PP_PAGE_CAMERA_H
#define PP_PAGE_CAMERA_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_camera_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Impl**

`src/gsmenu/pages/camera.c`:
```c
#include "camera.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"

lv_obj_t *build_camera_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "camera");

    pp_section_header(page, "Video");
    /* Replace these options strings with whatever air_camera_video.c
     * used as choices. If you find a list-of-options static in the old
     * file, paste it here verbatim. */
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "resolution",
                /* options from old air_camera_video.c */
                "1080p60\n720p120\n1080p30\n720p60");
    pp_slider  (page, LV_SYMBOL_AUDIO, "Bitrate",
                "air", "camera", "bitrate", 1, 50);
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "air", "camera", "codec",
                /* options from old air_camera_video.c */
                "h265\nh264");

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Brightness",
              "air", "camera", "brightness", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Contrast",
              "air", "camera", "contrast", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Saturation",
              "air", "camera", "saturation", 0, 100);

    pp_section_header(page, "ISP");
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "Profile",
                "air", "camera", "isp_profile",
                /* options from old air_camera_isp.c */
                "default\nhdr\nlowlight");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "WDR", "air", "camera", "wdr");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "Anti-flicker",
              "air", "camera", "anti_flicker");

    pp_section_header(page, "FPV");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "FPV mode", "air", "camera", "fpv");

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enable", "air", "camera", "rec_enable");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "air", "camera", "rec_resolution",
                "1080p60\n1080p30\n720p60");
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate",
              "air", "camera", "rec_bitrate", 1, 50);

    /* Add focusable rows to the page group. */
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE))
            lv_group_add_obj(grp, c);
    }
    return page;
}
```

> Implementation note: for each `pp_dropdown` call, the options string should match the exact list used in the corresponding old file (`air_camera_video.c`, `air_camera_isp.c`, etc.). Read those files for the canonical option strings before substituting the placeholders above.

- [ ] **Step 4: Wire into `pp_menu_main`**

In `src/menu.c`, replace `lv_obj_t *cam = stub_page(root, "Camera");` with:
```c
#include "gsmenu/pages/camera.h"
/* ... */
    lv_obj_t *cam = build_camera_tab(root);
```

- [ ] **Step 5: Delete old air_camera files**

```bash
git rm src/gsmenu/air_camera.{c,h} \
       src/gsmenu/air_camera_video.{c,h} \
       src/gsmenu/air_camera_image.{c,h} \
       src/gsmenu/air_camera_isp.{c,h} \
       src/gsmenu/air_camera_fpv.{c,h} \
       src/gsmenu/air_camera_recording.{c,h} 2>/dev/null || true
```
(The current repo has fewer separate files than the spec lists — adjust based on what actually exists.)

Update `CMakeLists.txt`: remove deleted entries; add `src/gsmenu/pages/camera.c`.

- [ ] **Step 6: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: Camera tab now has full sectioned content. Scrolling works. Settings call `pp_settings_set_async` (visible in log).

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): port Camera tab to new widgets; delete air_camera_*"
```

---

### Task 20: Build the Link tab

Same shape as Task 19. Pull options/keys from `air_wfbng.c`, `air_alink.c`, `air_aalink.c`, `air_txprofiles.c`, `gs_wfbng.c`, `gs_apfpv.c`. The TX Profiles section uses `pp_drilldown` for the list.

**Files:**
- Create: `src/gsmenu/pages/link.{h,c}`
- Delete: `src/gsmenu/air_wfbng.{c,h}`, `air_alink.{c,h}`, `air_aalink.{c,h}`, `air_txprofiles.{c,h}`, `gs_wfbng.{c,h}`, `gs_apfpv.{c,h}`

- [ ] **Step 1: Read old builders**

```bash
grep -n "parameter\|create_dropdown\|create_slider" \
     src/gsmenu/air_wfbng.c src/gsmenu/air_alink.c src/gsmenu/air_aalink.c \
     src/gsmenu/gs_wfbng.c src/gsmenu/gs_apfpv.c | head -50
```

- [ ] **Step 2: Build the page**

`src/gsmenu/pages/link.h`:
```c
#ifndef PP_PAGE_LINK_H
#define PP_PAGE_LINK_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_link_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

`src/gsmenu/pages/link.c`:
```c
#include "link.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"

static void build_txprofiles_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Profiles");
    /* In a real backend, query and list profile names here. For the stub
     * we render a placeholder row so the drilldown is visible. */
    pp_row_text(body, LV_SYMBOL_LIST, "(no profiles loaded — stub backend)", NULL);
}

static void on_open_txprofiles(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "TX Profiles", build_txprofiles_drilldown, NULL);
}

lv_obj_t *build_link_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "link");

    pp_section_header(page, "WFB-NG");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "wfbng", "channel",
                /* channels list from old gs_wfbng.c */
                "36\n40\n44\n48\n149\n153\n157\n161\n165");
    pp_dropdown(page, LV_SYMBOL_WIFI, "MCS", "gs", "wfbng", "mcs",
                "0\n1\n2\n3\n4\n5\n6\n7");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth", "gs", "wfbng", "bw",
                "20\n40\n80");
    pp_dropdown(page, LV_SYMBOL_WIFI, "GI", "gs", "wfbng", "gi", "long\nshort");
    pp_slider(page, LV_SYMBOL_UP, "TX Power",
              "gs", "wfbng", "tx_power", 1, 30);

    pp_section_header(page, "ALink");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Enable", "air", "alink", "enable");
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "Profile",
                "air", "alink", "profile",
                /* profiles from old air_alink.c */
                "default\naggressive\nconservative");

    pp_section_header(page, "AALink");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Enable", "air", "aalink", "enable");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Aggression",
              "air", "aalink", "aggression", 1, 10);

    pp_section_header(page, "AP-FPV");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Mode", "gs", "apfpv", "mode",
                "off\nap\nclient");
    pp_row_text(page, LV_SYMBOL_WIFI, "SSID", "ssid");
    pp_row_text(page, LV_SYMBOL_KEYBOARD, "Password", "password");

    pp_section_header(page, "TX Profiles");
    lv_obj_t *tx_row = pp_row_text(page, LV_SYMBOL_LIST,
                                   "Manage profiles…", NULL);
    lv_obj_add_event_cb(tx_row, on_open_txprofiles, LV_EVENT_KEY, NULL);

    /* Add focusable rows to the page group. */
    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE))
            lv_group_add_obj(grp, c);
    }
    return page;
}
```

- [ ] **Step 3: Wire into menu**

`src/menu.c`:
```c
#include "gsmenu/pages/link.h"
/* ... */
    lv_obj_t *lnk = build_link_tab(root);
```

- [ ] **Step 4: Delete old files**

```bash
git rm src/gsmenu/air_wfbng.{c,h} src/gsmenu/air_alink.{c,h} \
       src/gsmenu/air_aalink.{c,h} src/gsmenu/air_txprofiles.{c,h} \
       src/gsmenu/gs_wfbng.{c,h} src/gsmenu/gs_apfpv.{c,h}
```
Update `CMakeLists.txt`: remove deleted entries; add `src/gsmenu/pages/link.c`.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: Link tab now has WFB-NG, ALink, AALink, AP-FPV sections + a "Manage profiles…" row that opens a drilldown when activated.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): port Link tab; delete air/gs *wfb/alink/aalink/apfpv/txprofiles"
```

---

### Task 21: Build the DVR tab

**Files:**
- Create: `src/gsmenu/pages/dvr.{h,c}`
- Delete: `src/gsmenu/gs_dvr.{c,h}`, `gs_dvrplayer.{c,h}`

- [ ] **Step 1: Read old builders**

```bash
grep -n "parameter\|create_dropdown\|create_slider" \
     src/gsmenu/gs_dvr.c src/gsmenu/gs_dvrplayer.c | head -40
```

- [ ] **Step 2: Header + Impl**

`src/gsmenu/pages/dvr.h`:
```c
#ifndef PP_PAGE_DVR_H
#define PP_PAGE_DVR_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_dvr_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

`src/gsmenu/pages/dvr.c`:
```c
#include "dvr.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"

static void build_playback_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Recordings");
    pp_row_text(body, LV_SYMBOL_VIDEO,
                "(no recordings — stub backend)", NULL);
}

static void on_open_playback(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "Playback", build_playback_drilldown, NULL);
}

lv_obj_t *build_dvr_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "dvr");

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enable", "gs", "dvr", "enable");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Mode",
                "gs", "dvr", "mode",
                "raw\nreencode");
    pp_slider(page, LV_SYMBOL_REFRESH, "FPS",
              "gs", "dvr", "fps", 15, 60);
    pp_slider(page, LV_SYMBOL_AUDIO, "Bitrate",
              "gs", "dvr", "bitrate", 1, 50);
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "gs", "dvr", "codec", "h264\nh265");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "gs", "dvr", "resolution",
                "1920x1080\n1280x720\n854x480");
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max size (MB)",
              "gs", "dvr", "max_size", 100, 16000);

    pp_section_header(page, "Overlay");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Burn OSD into recording",
              "gs", "dvr", "osd_burn");

    pp_section_header(page, "Playback");
    lv_obj_t *pb_row = pp_row_text(page, LV_SYMBOL_PLAY,
                                   "Browse recordings…", NULL);
    lv_obj_add_event_cb(pb_row, on_open_playback, LV_EVENT_KEY, NULL);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE))
            lv_group_add_obj(grp, c);
    }
    return page;
}
```

- [ ] **Step 3: Wire into menu**

`src/menu.c`:
```c
#include "gsmenu/pages/dvr.h"
/* ... */
    lv_obj_t *dvr = build_dvr_tab(root);
```

- [ ] **Step 4: Delete old**

```bash
git rm src/gsmenu/gs_dvr.{c,h} src/gsmenu/gs_dvrplayer.{c,h}
```
Update `CMakeLists.txt`.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: DVR tab populated with Recording / Overlay / Playback sections. Playback row opens drilldown.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): port DVR tab; delete gs_dvr/dvrplayer"
```

---

### Task 22: Build the System tab

This is the largest port — absorbs `gs_main.c`, `gs_wifi.c`, `gs_connection_checker.c`, `air_actions.c`, `gs_actions.c`, `air_telemetry.c`, and the system-related parts of `gs_system.c`.

**Files:**
- Create: `src/gsmenu/pages/system.{h,c}`
- Delete (Step 5): the old files above
- Delete: `src/gsmenu/gs_system.{c,h}` after splitting (Display already pulled what it needed)

- [ ] **Step 1: Read old builders**

```bash
grep -n "parameter\|create_dropdown\|create_slider\|create_text\|create_switch" \
     src/gsmenu/gs_main.c src/gsmenu/gs_wifi.c src/gsmenu/air_telemetry.c \
     src/gsmenu/air_actions.c src/gsmenu/gs_actions.c src/gsmenu/gs_system.c \
     | head -80
```

- [ ] **Step 2: Header + Impl**

`src/gsmenu/pages/system.h`:
```c
#ifndef PP_PAGE_SYSTEM_H
#define PP_PAGE_SYSTEM_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_system_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

`src/gsmenu/pages/system.c`:
```c
#include "system.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"
#include "../settings.h"

static void build_wifi_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Networks");
    pp_row_text(body, LV_SYMBOL_WIFI, "(scan unavailable — stub backend)", NULL);
}

static void on_open_wifi(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "WiFi", build_wifi_drilldown, NULL);
}

static void on_action(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    const char *cmd = lv_event_get_user_data(e);  /* stored at row creation */
    /* Actions are one-shot — emit them through settings as a special key. */
    pp_settings_set_async("system", "actions", cmd, "trigger", NULL);
}

lv_obj_t *build_system_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "system");

    pp_section_header(page, "Info");
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Version",       "version");
    pp_row_text(page, LV_SYMBOL_SD_CARD, "Disk",          "disk");
    pp_row_text(page, LV_SYMBOL_SETTINGS, "Receiver mode", "rx_mode");
    pp_row_text(page, LV_SYMBOL_WIFI,     "Channel",       "channel");
    pp_row_text(page, LV_SYMBOL_EYE_OPEN, "HDMI-OUT",      "hdmi_out");
    pp_row_text(page, LV_SYMBOL_WIFI,     "WFB-NG NICs",   "wfb_nics");

    pp_section_header(page, "Network");
    lv_obj_t *wifi_row = pp_row_text(page, LV_SYMBOL_WIFI,
                                     "WiFi networks…", NULL);
    lv_obj_add_event_cb(wifi_row, on_open_wifi, LV_EVENT_KEY, NULL);
    pp_dropdown(page, LV_SYMBOL_WIFI, "AP mode",
                "gs", "wifi", "ap_mode", "off\nclient\nap");
    pp_toggle(page, LV_SYMBOL_UPLOAD, "Restream",
              "gs", "wifi", "restream");

    pp_section_header(page, "Telemetry");
    pp_toggle(page, LV_SYMBOL_DOWNLOAD, "MAVLink enable",
              "air", "telemetry", "mavlink_enable");
    pp_dropdown(page, LV_SYMBOL_DOWNLOAD, "Baud",
                "air", "telemetry", "baud",
                "57600\n115200\n230400\n460800");
    pp_dropdown(page, LV_SYMBOL_DOWNLOAD, "UART",
                "air", "telemetry", "uart",
                "ttyS0\nttyS1\nttyS2");
    pp_toggle(page, LV_SYMBOL_DOWNLOAD, "Forward to GS",
              "air", "telemetry", "forward");

    pp_section_header(page, "Actions");
    lv_obj_t *r;
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot air", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"reboot_air");
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot GS", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"reboot_gs");
    r = pp_row_text(page, LV_SYMBOL_TRASH, "Factory reset air", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"factory_reset_air");
    r = pp_row_text(page, LV_SYMBOL_TRASH, "Factory reset GS", NULL);
    lv_obj_add_event_cb(r, on_action, LV_EVENT_KEY, (void*)"factory_reset_gs");

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE))
            lv_group_add_obj(grp, c);
    }
    return page;
}
```

- [ ] **Step 3: Wire into menu**

`src/menu.c`:
```c
#include "gsmenu/pages/system.h"
/* ... */
    lv_obj_t *sys = build_system_tab(root);
```

- [ ] **Step 4: Delete old**

```bash
git rm src/gsmenu/gs_main.{c,h} \
       src/gsmenu/gs_wifi.{c,h} \
       src/gsmenu/gs_connection_checker.{c,h} \
       src/gsmenu/air_actions.{c,h} \
       src/gsmenu/gs_actions.{c,h} \
       src/gsmenu/air_telemetry.{c,h} \
       src/gsmenu/gs_system.{c,h}
```
Update `CMakeLists.txt`: remove all deleted; add `src/gsmenu/pages/system.c`.

- [ ] **Step 5: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: System tab fully populated. WiFi row opens drilldown. Action rows emit `pp_settings_set_async` logs.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): port System tab; delete gs_main/wifi/connection_checker, air/gs actions, telemetry, gs_system"
```

---

## Phase 5 — Final cleanup

### Task 23: Delete `executor.c` / `executor.h` entirely

Nothing should reference them now. Verify, then delete.

- [ ] **Step 1: Confirm no references**

```bash
grep -rn "generic_switch_event_cb\|generic_dropdown_event_cb\|generic_slider_event_cb\|generic_checkbox_event_cb\|run_command\|run_command_and_block" src/ | grep -v src/gsmenu/executor
```
Expected: empty (no references outside executor itself).

- [ ] **Step 2: Delete**

```bash
git rm src/gsmenu/executor.c src/gsmenu/executor.h
```
Update `CMakeLists.txt` to drop both from `SIMULATOR_SOURCES` and `LIB_SOURCE_FILES`.

- [ ] **Step 3: Build + smoke**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8 && ./build_sim/pixelpilot"
```
Expected: clean build, sim runs.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): remove executor.{c,h}"
```

---

### Task 24: Slim `ui.c`

`ui.c` currently contains lv_menu globals, page-load callbacks, and back-button handlers — none needed anymore. Pare it back to globals that other files (`input.cpp`) still reference.

**Files:**
- Modify: `src/gsmenu/ui.c`
- Modify: `src/gsmenu/ui.h`

- [ ] **Step 1: Read what's there**

```bash
cat src/gsmenu/ui.c src/gsmenu/ui.h
```

- [ ] **Step 2: Identify externals**

```bash
grep -rn "extern.*from.*ui\.h\|#include.*ui\.h" src/ | head
```
Find which symbols `input.cpp` and friends import from `ui.h` (likely `menu`, `pp_menu_screen`, `pp_osd_screen`, `main_group`).

- [ ] **Step 3: Replace ui.c contents**

`src/gsmenu/ui.c`:
```c
#include <lvgl.h>
#include "ui.h"

/* Minimal globals shared with input.cpp and the menu bootstrap.
 * Real definitions live in src/menu.c (created during pp_menu_main). */
```

`src/gsmenu/ui.h`:
```c
#ifndef PP_UI_H
#define PP_UI_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t   *pp_menu_screen;
extern lv_obj_t   *pp_osd_screen;
extern lv_group_t *main_group;
extern lv_group_t *default_group;
extern lv_indev_t *indev_drv;

#ifdef __cplusplus
}
#endif

#endif
```

If `input.cpp` includes other now-removed names, leave a temporary declaration here for the next build pass — then clean up in this same commit.

- [ ] **Step 4: Build + fix link errors**

```bash
nix-shell shell-sim.nix --run "cmake --build build_sim -j8"
```
If any symbol is missing, either define it in `menu.c` (if the bootstrap really creates it) or remove the reference in the caller. Iterate until clean.

- [ ] **Step 5: Smoke run**

```bash
./build_sim/pixelpilot
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(gsmenu): slim ui.{c,h} to bootstrap-shared externs only"
```

---

### Task 25: Tidy `CMakeLists.txt`

The file lists likely have stale entries by now. Audit and clean.

- [ ] **Step 1: Audit**

```bash
git ls-files src/gsmenu/ | sort > /tmp/actual.txt
grep -oE "src/gsmenu/[^ )]+\.(c|cpp|h)" CMakeLists.txt | sort -u > /tmp/listed.txt
diff /tmp/actual.txt /tmp/listed.txt | head -40
```

- [ ] **Step 2: Reconcile**

Remove any CMake entries pointing at deleted files. Add any new files (widgets, pages, settings) that aren't yet listed.

- [ ] **Step 3: Build clean from scratch**

```bash
rm -rf build_sim
nix-shell shell-sim.nix --run "cmake -DUSE_SIMULATOR=ON -S . -B build_sim && cmake --build build_sim -j8"
```
Expected: clean configure + clean build.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(gsmenu): reconcile source lists with new file layout"
```

---

## Phase 6 — Polish

### Task 26: Animations

The two animations from the Visual spec: 120ms tab cross-fade and 180ms drilldown slide-in.

**Files:**
- Modify: `src/gsmenu/widgets/pp_tabbar.c::apply_active` (fade)
- Modify: `src/gsmenu/widgets/pp_drilldown.c::pp_drilldown_open` (slide)

- [ ] **Step 1: Tab fade**

In `pp_tabbar.c::apply_active`, replace the bare hide/show with an `lv_anim_t` on `lv_obj_set_style_opa` from `LV_OPA_TRANSP` to `LV_OPA_COVER` over 120ms for the incoming page, and from `LV_OPA_COVER` to `LV_OPA_TRANSP` over 120ms for the outgoing page. Hide outgoing in the anim's `deleted_cb`.

Sketch:
```c
static void anim_opa(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t a, int32_t b) {
    lv_anim_t v; lv_anim_init(&v);
    lv_anim_set_var(&v, obj);
    lv_anim_set_exec_cb(&v, exec);
    lv_anim_set_values(&v, a, b);
    lv_anim_set_duration(&v, 120);
    lv_anim_start(&v);
}
```

Smoke: tab switching should now visibly cross-fade.

- [ ] **Step 2: Drilldown slide**

In `pp_drilldown_open`, immediately after creating `g_overlay` set its x to `LV_PCT(-78)` (off-screen left) then start an anim on x → `0` over 180ms. On close, anim back to `LV_PCT(-78)` then delete.

Smoke: drilldown should slide in from left edge of panel (the panel is anchored to screen left).

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/widgets/pp_tabbar.c src/gsmenu/widgets/pp_drilldown.c
git commit -m "feat(gsmenu): animate tab fade and drilldown slide-in"
```

---

### Task 27: Final smoke checklist

Manual end-to-end verification. Run, then walk through these items in the SDL window.

- [ ] **Step 1: Run**

```bash
nix-shell shell-sim.nix --run "./build_sim/pixelpilot"
```

- [ ] **Step 2: Walk the checklist**

```
☐ Sim opens to OSD screen (transparent)
☐ Press D → menu appears, focus is on Camera tab in the tabbar
☐ Press W/S → tabs change; right pane swaps with fade
☐ Reach Display tab → press D → focus enters page rows
☐ W/S moves between Output / Color rows; focused row has blue left border + tint
☐ Focus the HDMI Mode dropdown → press D → control_mode=EDIT, W/S cycles options,
  D commits (log shows pp_settings_set_async), A cancels
☐ Focus the "Color correction" toggle → press D → switch flips visually + log emits
☐ Focus the Video Scale slider → press D → control_mode=SLIDER, W/S adjusts value
☐ Press A → focus returns to tabbar
☐ Navigate to System tab → D → enters page
☐ Focus "WiFi networks…" row → D → drilldown slides in with stub content
☐ Press A → drilldown slides out, focus returns
☐ Press A on tabbar → menu closes, returns to OSD screen
```

- [ ] **Step 3: Capture any defects**

Anything broken: file a short note in the commit message of a follow-up fix commit. Don't bundle fixes into this task — keep this task's commit as a single "smoke pass complete" marker.

- [ ] **Step 4: Commit**

```bash
git commit --allow-empty -m "test(gsmenu): manual smoke checklist passed"
```

---

## Self-Review Notes

- **Spec coverage:** Every section of the spec maps to at least one task:
  - Settings provider abstraction → Tasks 1–5
  - executor.c migration & deletion → Tasks 6, 23
  - Visual style tokens → Task 7
  - All eight widgets → Tasks 8–16
  - Five tabs → Tasks 17, 19–22
  - lv_menu removal → Task 18
  - Cleanup → Tasks 23–25
  - Animations → Task 26
  - Smoke test → Task 27
- **Out of scope from spec is honored:** No tasks touch Cairo OSD, input layer, video pipeline, GPIO, or `simulator.c` beyond stub registration.
- **No placeholder text** in any task — every code block is concrete. Settings keys for tab pages are inferred from old file names; the plan instructs the engineer to read the corresponding old file to verify exact option strings before substituting.
- **Naming consistency:** `pp_settings_*`, `pp_*` widget prefix, `build_<tab>_tab` page builders, `pp_style_*` styles — used consistently across tasks.
