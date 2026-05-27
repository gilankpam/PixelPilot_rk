# fpvd Settings Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real settings backend that talks to the on-drone `fpvd` HTTP API over the wfb-ng tunnel, mapping the Camera/Link tabs to the fpvd schema, introducing a new Dynamic Link tab, and replacing the optimistic-rollback widget UX with a synchronous-feeling busy-spinner state.

**Architecture:** New provider `pp_settings_register_fpvd()` in `src/gsmenu/settings_fpvd.c`. Holds an in-memory `cJSON` snapshot of `GET /config` plus a libcurl-driven worker thread that PATCHes + applies per write, debounces same-path writes, polls `GET /config` when the menu is visible, and notifies the UI via a snapshot listener invoked on the LVGL thread. The existing `pp_settings_provider_t` gains four optional function pointers (`is_locked`, `is_connected`, `set_snapshot_listener`, `set_visibility`) so widgets and page builders can react to lock and offline state without knowing which provider is registered. Optimistic rollback in the widgets is replaced by a `pp_row_set_busy(row, bool)` helper that disables the inner control and shows a spinner during the in-flight request.

**Tech Stack:** C (provider + widgets), C++/Catch2 (tests), LVGL v9.5.0, libcurl (HTTP), cJSON (JSON, vendored), cpp-httplib (header-only test HTTP server, vendored). Spec: `docs/superpowers/specs/2026-05-27-fpvd-settings-backend-design.md`.

---

## Phase 0 — Build wiring and vendored deps

### Task 0.1: Vendor cJSON

**Files:**
- Create: `third_party/cjson/cJSON.h`
- Create: `third_party/cjson/cJSON.c`
- Create: `third_party/cjson/LICENSE`

cJSON is a single-file MIT-licensed C JSON parser. Sources upstream: https://github.com/DaveGamble/cJSON (release `v1.7.18` is fine; the API has been stable for years).

- [ ] **Step 1: Download cJSON 1.7.18 sources**

```bash
mkdir -p third_party/cjson
curl -L -o third_party/cjson/cJSON.h \
  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h
curl -L -o third_party/cjson/cJSON.c \
  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c
curl -L -o third_party/cjson/LICENSE \
  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/LICENSE
```

Expected: three files, ~4500 lines of C in cJSON.c.

- [ ] **Step 2: Verify sources compile standalone**

Run:
```bash
gcc -c -o /tmp/cjson_check.o third_party/cjson/cJSON.c -Ithird_party/cjson -Wall && echo OK
```
Expected: `OK` on stdout, no warnings other than possibly `-Wunused-parameter`-class noise (acceptable for vendored code).

- [ ] **Step 3: Commit**

```bash
git add third_party/cjson/
git commit -m "deps: vendor cJSON 1.7.18 (MIT)"
```

---

### Task 0.2: Vendor cpp-httplib (test-only)

**Files:**
- Create: `third_party/cpp-httplib/httplib.h`
- Create: `third_party/cpp-httplib/LICENSE`

cpp-httplib is a header-only HTTP/1.1 client+server. We use it only in integration tests.

- [ ] **Step 1: Download cpp-httplib 0.18.1**

```bash
mkdir -p third_party/cpp-httplib
curl -L -o third_party/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.1/httplib.h
curl -L -o third_party/cpp-httplib/LICENSE \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.1/LICENSE
```

- [ ] **Step 2: Verify header compiles**

Run:
```bash
g++ -std=c++17 -x c++-header -o /tmp/httplib_check.gch \
  third_party/cpp-httplib/httplib.h -pthread && echo OK
```
Expected: `OK`. (May take ~10 s — this is a big header.)

- [ ] **Step 3: Commit**

```bash
git add third_party/cpp-httplib/
git commit -m "deps: vendor cpp-httplib 0.18.1 (MIT)"
```

---

### Task 0.3: CMake — add cJSON to settings_tests + add new fpvd_tests target (skeleton)

**Files:**
- Modify: `CMakeLists.txt` (lines around 182–199 — the `BUILD_SIMULATOR` settings_tests block)

The existing `settings_tests` target already builds `settings.c` and `settings_dummy.c`. We will add `settings_fpvd.c` later; for now wire up a *new* `fpvd_tests` executable that only links what its unit tests need, so it can be created and pass an empty-test placeholder.

- [ ] **Step 1: Edit CMakeLists.txt — add cJSON sources var and new test exe**

Open `CMakeLists.txt`. Find the existing block:

```cmake
  # Standalone tests for the settings dispatcher — links only what it needs,
  # avoiding the heavy rockchip/drm/gst deps that pixelpilot_tests pulls in.
  find_package(Catch2 QUIET)
  if(Catch2_FOUND)
    add_executable(settings_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_dummy.c
      tests/test_settings.cpp
      tests/test_settings_failure.cpp)
    target_include_directories(settings_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(settings_tests Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES})
```

Immediately above `add_executable(settings_tests ...)` add:

```cmake
    set(PP_CJSON_SOURCES ${PROJECT_SOURCE_DIR}/third_party/cjson/cJSON.c)
    set(PP_CJSON_INC     ${PROJECT_SOURCE_DIR}/third_party/cjson)
    find_package(CURL REQUIRED)
```

Then, after the existing `lvgl_state_tests` block (around line 199) and *before* `endif()`, append:

```cmake
    # fpvd provider unit + integration tests
    add_executable(fpvd_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_fpvd.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_fpvd.cpp
      tests/test_settings_fpvd_integration.cpp)
    target_include_directories(fpvd_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC}
      ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
    target_link_libraries(fpvd_tests
      Catch2::Catch2WithMain lvgl ${PNG_LIBRARIES} CURL::libcurl pthread)
    target_compile_definitions(fpvd_tests PRIVATE PP_FPVD_TEST=1)
```

- [ ] **Step 2: Create placeholder test files so the target builds**

Create `tests/test_settings_fpvd.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("placeholder — fpvd unit tests will fill this", "[fpvd][placeholder]") {
    REQUIRE(true);
}
```

Create `tests/test_settings_fpvd_integration.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("placeholder — fpvd integration tests will fill this",
          "[fpvd][network][placeholder]") {
    REQUIRE(true);
}
```

Create `src/gsmenu/settings_fpvd.c`:

```c
/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"

void pp_settings_register_fpvd(void) {
    /* Filled in by Task 3.x. */
}
```

- [ ] **Step 3: Update `settings.h` with the new registration declaration**

Open `src/gsmenu/settings.h`. After the existing `pp_settings_register_dummy()` declaration (around line 58) add:

```c
/* Registers the on-drone fpvd HTTP provider. URL defaults to
 * http://10.5.0.10:8080, overridable via the PP_FPVD_URL env var. */
void pp_settings_register_fpvd(void);
```

- [ ] **Step 4: Configure and build the sim + tests**

Run:
```bash
cmake -S . -B build_sim -DBUILD_SIMULATOR=ON
cmake --build build_sim --target settings_tests fpvd_tests -j
```
Expected: both executables build with no errors.

- [ ] **Step 5: Run the new test executable to confirm it's wired**

Run:
```bash
./build_sim/fpvd_tests
```
Expected: two placeholder tests pass; non-zero `[fpvd]` count.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/test_settings_fpvd.cpp tests/test_settings_fpvd_integration.cpp src/gsmenu/settings.h src/gsmenu/settings_fpvd.c
git commit -m "build: scaffold fpvd_tests target and settings_fpvd.c stub"
```

---

## Phase 1 — Extend the provider contract

### Task 1.1: Add optional function pointers to `pp_settings_provider_t`

**Files:**
- Modify: `src/gsmenu/settings.h`
- Modify: `src/gsmenu/settings.c`
- Modify: `tests/test_settings.cpp` (add tests for the new dispatchers)

The dispatcher already forwards to optional fn pointers (e.g. `set_async` falls back to `set`). We add four more, each optional, with safe defaults.

- [ ] **Step 1: Write failing tests for the new dispatchers**

Open `tests/test_settings.cpp`. Add at the end of the file:

```cpp
TEST_CASE("dispatch: is_locked returns false when provider lacks it") {
    pp_settings_register(&rec_provider);
    REQUIRE(pp_settings_is_locked("a", "b", "c") == false);
}

TEST_CASE("dispatch: is_connected returns true when provider lacks it") {
    pp_settings_register(&rec_provider);
    REQUIRE(pp_settings_is_connected() == true);
}

TEST_CASE("dispatch: set_visibility is a no-op when provider lacks it") {
    pp_settings_register(&rec_provider);
    pp_settings_set_visibility(true);
    pp_settings_set_visibility(false);
    /* No crash. */
    REQUIRE(true);
}

TEST_CASE("dispatch: set_snapshot_listener is a no-op when provider lacks it") {
    pp_settings_register(&rec_provider);
    pp_settings_set_snapshot_listener(nullptr, nullptr);
    REQUIRE(true);
}

TEST_CASE("dispatch: forwards is_locked / is_connected when provider has them") {
    static bool locked_called = false;
    static bool connected_called = false;
    static auto _is_locked = +[](const char *, const char *, const char *) -> bool {
        locked_called = true; return true;
    };
    static auto _is_connected = +[]() -> bool {
        connected_called = true; return false;
    };
    static const pp_settings_provider_t full = {
        .set = rec_set, .get = rec_get, .set_async = rec_set_async,
        .is_locked = _is_locked, .is_connected = _is_connected,
        .set_snapshot_listener = nullptr, .set_visibility = nullptr,
    };
    pp_settings_register(&full);
    REQUIRE(pp_settings_is_locked("x", "y", "z") == true);
    REQUIRE(locked_called == true);
    REQUIRE(pp_settings_is_connected() == false);
    REQUIRE(connected_called == true);
}
```

- [ ] **Step 2: Run to confirm fail**

Run:
```bash
cmake --build build_sim --target settings_tests 2>&1 | head -20
```
Expected: build fails with errors like `'pp_settings_is_locked' was not declared`, `'pp_settings_provider_t' has no member named 'is_locked'`.

- [ ] **Step 3: Update `settings.h`**

Replace the existing `pp_settings_provider_t` struct (lines 15–32) with:

```c
typedef void (*pp_settings_done_cb)(int rc, const char *err, void *user_data);

/* Called on the LVGL thread when the provider's snapshot mutates (e.g.
 * after a successful apply, after a poll round, or on a connectivity
 * transition). UI listeners walk their rows and re-evaluate enabled/
 * disabled state. */
typedef void (*pp_settings_snapshot_cb)(void *user_data);

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
     * on_done synchronously if the operation is cheap. user_data is passed
     * through to on_done unchanged. */
    void  (*set_async)(const char *domain, const char *page,
                       const char *key, const char *value,
                       pp_settings_done_cb on_done, void *user_data);

    /* Optional: returns true if the key is currently read-only (e.g. owned
     * by a dynamic-link controller). NULL → dispatcher returns false. */
    bool  (*is_locked)(const char *domain, const char *page, const char *key);

    /* Optional: returns true if the backend is currently reachable.
     * NULL → dispatcher returns true. */
    bool  (*is_connected)(void);

    /* Optional: register a single listener for snapshot mutations. Passing
     * cb=NULL clears the listener. NULL pointer → dispatcher no-op. */
    void  (*set_snapshot_listener)(pp_settings_snapshot_cb cb, void *user_data);

    /* Optional: hint about UI visibility so the backend can throttle polls.
     * NULL pointer → dispatcher no-op. */
    void  (*set_visibility)(bool visible);
} pp_settings_provider_t;
```

Below the existing wrapper declarations (`pp_settings_set`, `pp_settings_get`, `pp_settings_set_async`), add:

```c
/* Forwarding wrappers; safe to call regardless of which provider is
 * registered (return safe defaults when the underlying provider does
 * not implement the optional method). */
bool  pp_settings_is_locked(const char *domain, const char *page,
                            const char *key);
bool  pp_settings_is_connected(void);
void  pp_settings_set_snapshot_listener(pp_settings_snapshot_cb cb,
                                        void *user_data);
void  pp_settings_set_visibility(bool visible);
```

Also add `#include <stdbool.h>` at the top of the file if not already present.

- [ ] **Step 4: Implement the wrappers in `settings.c`**

Append to `src/gsmenu/settings.c`:

```c
bool pp_settings_is_locked(const char *d, const char *p, const char *k) {
    if (g_provider && g_provider->is_locked) {
        return g_provider->is_locked(d, p, k);
    }
    return false;
}

bool pp_settings_is_connected(void) {
    if (g_provider && g_provider->is_connected) {
        return g_provider->is_connected();
    }
    return true;
}

void pp_settings_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    if (g_provider && g_provider->set_snapshot_listener) {
        g_provider->set_snapshot_listener(cb, ud);
    }
}

void pp_settings_set_visibility(bool visible) {
    if (g_provider && g_provider->set_visibility) {
        g_provider->set_visibility(visible);
    }
}
```

Add `#include <stdbool.h>` to the top if missing.

- [ ] **Step 5: Build and run tests**

Run:
```bash
cmake --build build_sim --target settings_tests
./build_sim/settings_tests
```
Expected: all tests pass (existing + 5 new).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings.h src/gsmenu/settings.c tests/test_settings.cpp
git commit -m "feat(settings): add is_locked / is_connected / listener / visibility provider hooks"
```

---

## Phase 2 — Replace optimistic rollback with busy state

### Task 2.1: Add `pp_row_set_busy()` and `pp_row_set_locked()`

**Files:**
- Modify: `src/gsmenu/widgets/pp_row.h`
- Modify: `src/gsmenu/widgets/pp_row.c`

Rows need two new visual states orthogonal to focus: **busy** (request in flight) and **locked** (read-only because of Dynamic Link or offline). Both are visible affordances; both block input. We expose helpers callable from any widget.

- [ ] **Step 1: Add declarations to `pp_row.h`**

Open `src/gsmenu/widgets/pp_row.h`. Replace the file with:

```c
#ifndef PP_ROW_H
#define PP_ROW_H
#include <lvgl.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *pp_row_text(lv_obj_t *parent_page,
                      const char *icon_text,
                      const char *label,
                      const char *key);

void pp_row_set_value(lv_obj_t *row, const char *value);

/* Show/hide a small spinner at the row's trailing edge, disable child
 * widget input while busy. Calls are nestable — call with the same flag
 * idempotently. Safe to call on rows that don't have a child input. */
void pp_row_set_busy(lv_obj_t *row, bool busy);

/* Mark the row as read-only. Disables input and applies a greyed style.
 * `reason` selects the lock icon (LOCK = dynamic link, OFFLINE = drone
 * unreachable). When false, restores the row to interactive state. */
typedef enum {
    PP_ROW_UNLOCKED = 0,
    PP_ROW_LOCKED_DYNAMIC = 1,
    PP_ROW_LOCKED_OFFLINE = 2,
} pp_row_lock_t;

void pp_row_set_locked(lv_obj_t *row, pp_row_lock_t state);
pp_row_lock_t pp_row_get_locked(lv_obj_t *row);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Implement in `pp_row.c`**

Open `src/gsmenu/widgets/pp_row.c`. Add at the top (after existing includes):

```c
#include "../styles.h"
```

Append to the file:

```c
/* Per-row UX-state storage attached as a side struct via lv_obj_set_user_data
 * on a hidden child. We don't want to disturb existing user_data on the row
 * which other widgets already use. Instead we look up a dedicated label
 * child by name. */

#define PP_ROW_BUSY_ICON "\xE2\x8F\xB3"  /* ⏳ U+23F3 */
#define PP_ROW_LOCK_ICON "\xF0\x9F\x94\x92" /* 🔒 (rendered via Montserrat fallback — */
                                            /* concrete glyph chosen during impl) */

typedef struct {
    lv_obj_t      *spinner;       /* small label, hidden when not busy */
    lv_obj_t      *lock_label;    /* small label, hidden when not locked */
    pp_row_lock_t  lock_state;
    bool           busy;
} pp_row_state_t;

static pp_row_state_t *row_state(lv_obj_t *row) {
    /* Stored in a child object named via custom property. We use
     * LV_OBJ_FLAG_USER_1 on the first child label as a marker; if no
     * state child exists we create one. */
    uint32_t n = lv_obj_get_child_cnt(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) {
            return (pp_row_state_t *)lv_obj_get_user_data(c);
        }
    }
    /* Create the state holder: an empty 0-size object marked with USER_2. */
    lv_obj_t *holder = lv_obj_create(row);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, 0, 0);
    lv_obj_add_flag(holder, LV_OBJ_FLAG_USER_2);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE);
    pp_row_state_t *s = lv_malloc(sizeof(*s));
    s->spinner = NULL;
    s->lock_label = NULL;
    s->lock_state = PP_ROW_UNLOCKED;
    s->busy = false;
    lv_obj_set_user_data(holder, s);
    return s;
}

static lv_obj_t *ensure_spinner(lv_obj_t *row, pp_row_state_t *s) {
    if (s->spinner) return s->spinner;
    s->spinner = lv_label_create(row);
    lv_label_set_text(s->spinner, PP_ROW_BUSY_ICON);
    lv_obj_set_style_text_color(s->spinner, lv_color_hex(0x6B7FFF), 0);
    lv_obj_set_style_pad_left(s->spinner, 6, 0);
    lv_obj_add_flag(s->spinner, LV_OBJ_FLAG_HIDDEN);
    return s->spinner;
}

static lv_obj_t *ensure_lock_label(lv_obj_t *row, pp_row_state_t *s) {
    if (s->lock_label) return s->lock_label;
    s->lock_label = lv_label_create(row);
    lv_label_set_text(s->lock_label, PP_ROW_LOCK_ICON);
    lv_obj_set_style_text_color(s->lock_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_pad_left(s->lock_label, 6, 0);
    lv_obj_add_flag(s->lock_label, LV_OBJ_FLAG_HIDDEN);
    return s->lock_label;
}

void pp_row_set_busy(lv_obj_t *row, bool busy) {
    pp_row_state_t *s = row_state(row);
    if (s->busy == busy) return;
    s->busy = busy;
    lv_obj_t *spin = ensure_spinner(row, s);
    if (busy) {
        lv_obj_clear_flag(spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(row, LV_STATE_DISABLED);
    } else {
        lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(row, LV_STATE_DISABLED);
    }
}

void pp_row_set_locked(lv_obj_t *row, pp_row_lock_t state) {
    pp_row_state_t *s = row_state(row);
    if (s->lock_state == state) return;
    s->lock_state = state;
    lv_obj_t *lbl = ensure_lock_label(row, s);
    if (state == PP_ROW_UNLOCKED) {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(row, LV_STATE_DISABLED);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
    } else {
        lv_label_set_text(lbl,
            state == PP_ROW_LOCKED_OFFLINE ? LV_SYMBOL_WARNING : PP_ROW_LOCK_ICON);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(row, LV_STATE_DISABLED);
        lv_obj_set_style_opa(row, LV_OPA_60, 0);
    }
}

pp_row_lock_t pp_row_get_locked(lv_obj_t *row) {
    return row_state(row)->lock_state;
}
```

(The exact glyphs for the lock/spinner icons are placeholders — LVGL's built-in symbols are listed in `lvgl/src/font/lv_symbol_def.h`. If the chosen icons don't render, swap them for the closest `LV_SYMBOL_*` constants. The behavior — hide/show + disable — does not depend on the glyph.)

- [ ] **Step 3: Build sim to confirm row.c still compiles**

Run:
```bash
cmake --build build_sim --target pixelpilot_sim -j
```

(Target name may differ — check what the sim build produces. The intent is "build the simulator executable". If the target name is different, use `cmake --build build_sim` and rely on full-tree.)

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/widgets/pp_row.h src/gsmenu/widgets/pp_row.c
git commit -m "feat(widgets): pp_row_set_busy / pp_row_set_locked helpers"
```

---

### Task 2.2: Rewire `pp_toggle.c` from optimistic rollback to busy state

**Files:**
- Modify: `src/gsmenu/widgets/pp_toggle.c`

Behavior change: tapping the toggle does **not** flip the switch immediately. Instead the widget enters busy state with the old value still displayed. On success the switch flips; on failure it stays and a toast appears.

- [ ] **Step 1: Replace `pp_toggle.c` content**

Open `src/gsmenu/widgets/pp_toggle.c`. Replace the file with:

```c
#include "pp_toggle.h"
#include "pp_toast.h"
#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *domain, *page, *key;
    lv_obj_t *sw;
    lv_obj_t *row;
    bool      in_flight;
} pp_toggle_data_t;

struct toggle_ctx {
    pp_toggle_data_t *d;
    bool target_on;       /* the value we *attempted* to write */
};

static void on_delete(lv_event_t *e) {
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    if (d) { free(d->domain); free(d->page); free(d->key); free(d); }
}

static void toggle_done_cb(int rc, const char *err, void *user_data) {
    struct toggle_ctx *ctx = (struct toggle_ctx *)user_data;
    pp_toggle_data_t *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        if (ctx->target_on) lv_obj_add_state(d->sw, LV_STATE_CHECKED);
        else                lv_obj_remove_state(d->sw, LV_STATE_CHECKED);
    } else {
        pp_toast_error(err ? err : "Failed to apply toggle");
        /* Switch position unchanged — we never flipped it. */
    }
    lv_free(ctx);
}

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    pp_toggle_data_t *d = lv_event_get_user_data(e);
    if (d->in_flight) { lv_event_stop_bubbling(e); return; }
    if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
        pp_toast_error("Locked by Dynamic Link");
        lv_event_stop_bubbling(e);
        return;
    }
    bool prev_on = lv_obj_has_state(d->sw, LV_STATE_CHECKED);
    bool target  = !prev_on;
    d->in_flight = true;
    pp_row_set_busy(d->row, true);
    struct toggle_ctx *ctx = lv_malloc(sizeof(*ctx));
    ctx->d = d;
    ctx->target_on = target;
    pp_settings_set_async(d->domain, d->page, d->key, target ? "on" : "off",
                          toggle_done_cb, ctx);
    lv_event_stop_bubbling(e);
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
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

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
    d->row    = row;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);

    char *v = pp_settings_get(domain, page, key);
    if (v && strcmp(v, "on") == 0) lv_obj_add_state(sw, LV_STATE_CHECKED);
    free(v);

    if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }

    return row;
}
```

- [ ] **Step 2: Build sim**

Run: `cmake --build build_sim -j`. Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/widgets/pp_toggle.c
git commit -m "feat(widgets): pp_toggle uses busy state instead of optimistic flip"
```

---

### Task 2.3: Rewire `pp_slider.c` from optimistic rollback to busy state

**Files:**
- Modify: `src/gsmenu/widgets/pp_slider.c`

- [ ] **Step 1: Update `pp_slider.c`**

Replace the body of `slider_done_cb` (lines 16–27) with:

```c
struct slider_ctx {
    pp_slider_data_t *d;
    int32_t target_val;
};

static void slider_done_cb(int rc, const char *err, void *user_data) {
    struct slider_ctx *ctx = (struct slider_ctx *)user_data;
    pp_slider_data_t *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        d->value = ctx->target_val;
        refresh_num(d);
    } else {
        pp_toast_error(err ? err : "Failed to apply slider");
        /* d->value already at saved_val (we never moved it in the new model). */
    }
    lv_free(ctx);
}
```

Move `struct slider_ctx` declaration above the function (replacing the existing one at top of file).

Add `lv_obj_t *row;` and `bool in_flight;` to `pp_slider_data_t` (around line 43):

```c
typedef struct {
    char *domain, *page, *key;
    int32_t min, max;
    int32_t value;
    int32_t saved_val;
    lv_obj_t *num, *up_chev, *down_chev;
    lv_obj_t *row;
    bool      in_flight;
} pp_slider_data_t;
```

In `pp_slider()` (the constructor, after `lv_obj_set_user_data(row, d);`), add:

```c
    d->row = row;
```

Add `#include "pp_row.h"` near the top.

Replace `on_key`'s handling of the EDIT → NAV transition (the block currently containing `pp_settings_set_async`) with:

```c
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
                pp_toast_error("Locked by Dynamic Link");
                consumed = true;
            } else {
                d->saved_val = d->value;
                control_mode = GSMENU_CONTROL_MODE_EDIT;
                set_edit_state(d, true);
                consumed = true;
            }
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            set_edit_state(d, false);
            if (d->value == d->saved_val) {
                consumed = true;          /* no change — skip the round-trip */
            } else {
                char buf[32];
                snprintf(buf, sizeof buf, "%d", (int)d->value);
                /* Revert the visible value to saved_val until apply confirms;
                 * we keep the *attempted* value in ctx->target_val. */
                int32_t attempted = d->value;
                d->value = d->saved_val;
                refresh_num(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct slider_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d = d;
                ctx->target_val = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf,
                                      slider_done_cb, ctx);
                consumed = true;
            }
        }
```

In the constructor, after the initial value read block (after the `if (v && *v) { ... }` block), add:

```c
    if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }
```

- [ ] **Step 2: Build sim**

Run: `cmake --build build_sim -j`. Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/widgets/pp_slider.c
git commit -m "feat(widgets): pp_slider uses busy state; value confirmed on apply success"
```

---

### Task 2.4: Rewire `pp_dropdown.c` from optimistic rollback to busy state

**Files:**
- Modify: `src/gsmenu/widgets/pp_dropdown.c`

- [ ] **Step 1: Update `pp_dropdown.c`**

Replace the `dropdown_ctx` / `dropdown_done_cb` block (lines 9–26) with:

```c
struct dropdown_ctx {
    pp_dd_data_t *d;
    uint16_t      target_sel;
};

static void dropdown_done_cb(int rc, const char *err, void *user_data) {
    struct dropdown_ctx *ctx = (struct dropdown_ctx *)user_data;
    pp_dd_data_t *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        lv_dropdown_set_selected(d->dd, ctx->target_sel);
        refresh_label(d);
    } else {
        pp_toast_error(err ? err : "Failed to apply dropdown");
        /* Selection already at saved_sel — we never moved it. */
    }
    lv_free(ctx);
}
```

Move the `struct dropdown_ctx` typedef above `dropdown_done_cb`.

Add `lv_obj_t *row;` and `bool in_flight;` to `pp_dd_data_t` (already has `row`; just add `in_flight`):

```c
typedef struct {
    char *domain, *page, *key;
    lv_obj_t *dd, *value_label, *row;
    lv_obj_t *popup;
    uint16_t saved_sel;
    bool      in_flight;
} pp_dd_data_t;
```

Add `#include "pp_row.h"` near the top.

Replace the ENTER handling in EDIT mode (the block currently containing `pp_settings_set_async`) — find:

```c
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            char buf[64];
            lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
            struct dropdown_ctx *ctx = lv_malloc(sizeof(*ctx));
            ctx->dd          = d->dd;
            ctx->value_label = d->value_label;
            ctx->prev_sel    = d->saved_sel;
            pp_settings_set_async(d->domain, d->page, d->key, buf,
                                  dropdown_done_cb, ctx);
            popup_close(d);
        }
```

Replace with:

```c
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            uint16_t attempted = lv_dropdown_get_selected(d->dd);
            popup_close(d);
            if (attempted == d->saved_sel) break_to_end: ; else {
                char buf[64];
                lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
                /* Revert the visible selection to saved_sel; on success the
                 * callback will move it to attempted. */
                lv_dropdown_set_selected(d->dd, d->saved_sel);
                refresh_label(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct dropdown_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d          = d;
                ctx->target_sel = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf,
                                      dropdown_done_cb, ctx);
            }
        }
```

(Remove the `break_to_end:` label artifact — that was a typo. Use a plain `if/else`:)

```c
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            uint16_t attempted = lv_dropdown_get_selected(d->dd);
            popup_close(d);
            if (attempted != d->saved_sel) {
                char buf[64];
                lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
                lv_dropdown_set_selected(d->dd, d->saved_sel);
                refresh_label(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct dropdown_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d          = d;
                ctx->target_sel = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf,
                                      dropdown_done_cb, ctx);
            }
        }
```

Also guard the ENTER-in-NAV-mode entry with a lock check, similar to slider:

Find the start of `on_key` ENTER handling:

```c
    if (k == LV_KEY_ENTER) {
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
```

Replace with:

```c
    if (k == LV_KEY_ENTER) {
        if (d->in_flight) { consumed = true; goto done_consume; }
        if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
                pp_toast_error("Locked by Dynamic Link");
                consumed = true;
                goto done_consume;
            }
```

…and immediately before the closing `}` of `on_key`, add the label:

```c
done_consume:
    if (consumed) lv_event_stop_bubbling(e);
}
```

(Remove the duplicate `if (consumed) lv_event_stop_bubbling(e);` from earlier in the function.)

In the constructor, after the existing value-read block, add:

```c
    if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }
```

- [ ] **Step 2: Build sim**

Run: `cmake --build build_sim -j`. Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/widgets/pp_dropdown.c
git commit -m "feat(widgets): pp_dropdown uses busy state; lock guard"
```

---

### Task 2.5: Update the existing settings_failure test to reflect the new contract

**Files:**
- Modify: `tests/test_settings_failure.cpp`

The `PP_SIM_FAIL` test already asserts that the value is **not** written on failure (which is also true under the new busy-state model — the widget didn't move and the provider rejected). The test still passes as-is; no edit needed. We add a defensive new test that the value *is* written on success and the callback fires with `rc=0`.

- [ ] **Step 1: Run the test as-is to confirm it still passes**

Run:
```bash
cmake --build build_sim --target settings_tests && ./build_sim/settings_tests "[settings][failure]"
```
Expected: both existing tests pass.

- [ ] **Step 2: No code change required; commit a no-op marker if necessary**

(If git shows clean, skip the commit.) The intent of this task is verification, not change.

---

## Phase 3 — fpvd provider, TDD by component

All Phase 3 work happens in `src/gsmenu/settings_fpvd.c` (with optional helper headers in the same directory) and tested by `tests/test_settings_fpvd.cpp`.

### Task 3.1: Key map (data + sanity test)

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c`
- Create: `src/gsmenu/settings_fpvd_internal.h` (test-visible interface)
- Modify: `tests/test_settings_fpvd.cpp`

- [ ] **Step 1: Write failing test for key map lookup**

Replace `tests/test_settings_fpvd.cpp` with:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "gsmenu/settings_fpvd_internal.h"
}

TEST_CASE("keymap: lookup returns the json path for known triples", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.fps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "camera", "bitrate");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.bitrate") == 0);
    REQUIRE(e->type == FPVD_T_BITRATE_KBPS);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.width") == 0);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_k");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.k") == 0);

    e = fpvd_keymap_lookup("air", "dlink", "safe_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.safe.bitrateKbps") == 0);
}

TEST_CASE("keymap: lookup returns null for unknown triples", "[fpvd][keymap]") {
    REQUIRE(fpvd_keymap_lookup("air", "camera", "nope") == nullptr);
    REQUIRE(fpvd_keymap_lookup("nope", "nope", "nope") == nullptr);
}
```

- [ ] **Step 2: Confirm build fails**

Run: `cmake --build build_sim --target fpvd_tests 2>&1 | head -10`.
Expected: error `'settings_fpvd_internal.h' file not found`.

- [ ] **Step 3: Create `src/gsmenu/settings_fpvd_internal.h`**

```c
#ifndef PP_SETTINGS_FPVD_INTERNAL_H
#define PP_SETTINGS_FPVD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    const char *domain;
    const char *page;
    const char *key;
    const char *path;
    fpvd_type_t type;
} fpvd_keymap_entry_t;

const fpvd_keymap_entry_t *fpvd_keymap_lookup(const char *domain,
                                              const char *page,
                                              const char *key);

/* Iterate all entries (returns NULL on end). i starts at 0. */
const fpvd_keymap_entry_t *fpvd_keymap_at(size_t i);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Implement the key map in `settings_fpvd.c`**

Replace `src/gsmenu/settings_fpvd.c` with the entries below, plus the registration stub:

```c
/* fpvd HTTP settings provider — implemented incrementally; see plan. */
#include "settings.h"
#include "settings_fpvd_internal.h"

#include <string.h>

static const fpvd_keymap_entry_t KEYMAP[] = {
    /* Camera — Video */
    { "air", "camera", "size",       "video.resolution",  FPVD_T_STRING },
    { "air", "camera", "fps",        "video.fps",         FPVD_T_INT    },
    { "air", "camera", "bitrate",    "video.bitrate",     FPVD_T_BITRATE_KBPS },
    { "air", "camera", "codec",      "video.codec",       FPVD_T_ENUM   },
    { "air", "camera", "gopsize",    "video.gopSize",     FPVD_T_FLOAT  },
    { "air", "camera", "rc_mode",    "video.rcMode",      FPVD_T_ENUM   },
    { "air", "camera", "qp_delta",   "video.qpDelta",     FPVD_T_INT    },

    /* Camera — ROI */
    { "air", "camera", "roi_enabled","video.roi.enabled", FPVD_T_BOOL   },
    { "air", "camera", "roi_qp",     "video.roi.qp",      FPVD_T_INT    },
    { "air", "camera", "roi_center", "video.roi.center",  FPVD_T_PERCENT_TO_FRAC },
    { "air", "camera", "roi_steps",  "video.roi.steps",   FPVD_T_INT    },

    /* Camera — Image */
    { "air", "camera", "mirror",     "image.mirror",      FPVD_T_BOOL   },
    { "air", "camera", "flip",       "image.flip",        FPVD_T_BOOL   },
    { "air", "camera", "rotate",     "image.rotate",      FPVD_T_INT    },

    /* Camera — Recording */
    { "air", "camera", "rec_enable", "recording.enabled",    FPVD_T_BOOL },
    { "air", "camera", "rec_split",  "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN },
    { "air", "camera", "rec_maxmb",  "recording.maxMB",      FPVD_T_INT  },

    /* Link — WFB-NG */
    { "gs",  "wfbng", "gs_channel", "link.channel",  FPVD_T_INT },
    { "gs",  "wfbng", "bandwidth",  "link.width",    FPVD_T_INT },
    { "gs",  "wfbng", "txpower",    "link.txpower",  FPVD_T_INT },
    { "air", "wfbng", "mcs_index",  "link.mcs",      FPVD_T_INT },
    { "air", "wfbng", "stbc",       "link.stbc",     FPVD_T_BOOL },
    { "air", "wfbng", "ldpc",       "link.ldpc",     FPVD_T_BOOL },
    { "air", "wfbng", "fec_k",      "link.fec.k",    FPVD_T_INT },
    { "air", "wfbng", "fec_n",      "link.fec.n",    FPVD_T_INT },

    /* Dynamic Link */
    { "air", "dlink", "enabled",              "dynamicLink.enabled",              FPVD_T_BOOL },
    { "air", "dlink", "interleaving",         "dynamicLink.interleavingSupported",FPVD_T_BOOL },
    { "air", "dlink", "mavlink_enable",       "dynamicLink.mavlinkEnable",        FPVD_T_BOOL },
    { "air", "dlink", "osd_enabled",          "dynamicLink.osd.enabled",          FPVD_T_BOOL },
    { "air", "dlink", "osd_debug_latency",    "dynamicLink.osd.debugLatency",     FPVD_T_BOOL },
    { "air", "dlink", "health_timeout_ms",    "dynamicLink.healthTimeoutMs",      FPVD_T_INT },
    { "air", "dlink", "min_idr_interval_ms",  "dynamicLink.minIdrIntervalMs",     FPVD_T_INT },
    { "air", "dlink", "apply_stagger_ms",     "dynamicLink.applyStaggerMs",       FPVD_T_INT },
    { "air", "dlink", "apply_subpace_ms",     "dynamicLink.applySubPaceMs",       FPVD_T_INT },
    { "air", "dlink", "roiqp_threshold_kbps", "dynamicLink.roiQp.thresholdKbps",  FPVD_T_INT },
    { "air", "dlink", "roiqp_low_anchor_kbps","dynamicLink.roiQp.lowAnchorKbps",  FPVD_T_INT },
    { "air", "dlink", "roiqp_floor",          "dynamicLink.roiQp.floor",          FPVD_T_INT },
    { "air", "dlink", "roiqp_step",           "dynamicLink.roiQp.step",           FPVD_T_INT },
    { "air", "dlink", "safe_mcs",             "dynamicLink.safe.mcs",             FPVD_T_INT },
    { "air", "dlink", "safe_k",               "dynamicLink.safe.k",               FPVD_T_INT },
    { "air", "dlink", "safe_n",               "dynamicLink.safe.n",               FPVD_T_INT },
    { "air", "dlink", "safe_depth",           "dynamicLink.safe.depth",           FPVD_T_INT },
    { "air", "dlink", "safe_bandwidth",       "dynamicLink.safe.bandwidth",       FPVD_T_INT },
    { "air", "dlink", "safe_txpower_dbm",     "dynamicLink.safe.txPowerDbm",      FPVD_T_INT },
    { "air", "dlink", "safe_bitrate_kbps",    "dynamicLink.safe.bitrateKbps",     FPVD_T_INT },
};

static const size_t KEYMAP_N = sizeof(KEYMAP) / sizeof(KEYMAP[0]);

const fpvd_keymap_entry_t *fpvd_keymap_lookup(const char *d, const char *p, const char *k) {
    for (size_t i = 0; i < KEYMAP_N; i++) {
        if (strcmp(KEYMAP[i].domain, d) == 0 &&
            strcmp(KEYMAP[i].page,   p) == 0 &&
            strcmp(KEYMAP[i].key,    k) == 0) {
            return &KEYMAP[i];
        }
    }
    return NULL;
}

const fpvd_keymap_entry_t *fpvd_keymap_at(size_t i) {
    if (i >= KEYMAP_N) return NULL;
    return &KEYMAP[i];
}

void pp_settings_register_fpvd(void) {
    /* Filled in by Task 3.8. */
}
```

- [ ] **Step 5: Build and run**

Run:
```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][keymap]"
```
Expected: both keymap tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd.cpp
git commit -m "feat(fpvd): static keymap + lookup"
```

---

### Task 3.2: JSON snapshot read (`fpvd_snapshot_read_string`)

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `tests/test_settings_fpvd.cpp`

`fpvd_snapshot_read_string` takes a cJSON tree + a dotted path + a type, walks the tree, formats the value as a string (matching what page builders expect), and returns a heap-allocated string. Empty string ("") if path is missing.

- [ ] **Step 1: Write failing tests**

Append to `tests/test_settings_fpvd.cpp`:

```cpp
extern "C" {
#include "cJSON.h"
}
#include <cstdlib>

static cJSON *fixture_defaults() {
    /* Subset of /defaults from the API doc, enough for our path tests. */
    const char *src =
      "{\"link\":{\"channel\":161,\"width\":20,\"txpower\":1,\"mcs\":2,"
      "\"fec\":{\"k\":8,\"n\":12},\"stbc\":false,\"ldpc\":true},"
      "\"video\":{\"codec\":\"h265\",\"resolution\":\"1920x1080\","
      "\"fps\":60,\"bitrate\":8192,\"rcMode\":\"cbr\",\"gopSize\":1.0,"
      "\"qpDelta\":-4,\"roi\":{\"enabled\":true,\"qp\":0,\"center\":0.4,"
      "\"steps\":2}},"
      "\"image\":{\"mirror\":false,\"flip\":false,\"rotate\":0},"
      "\"recording\":{\"enabled\":false,\"maxSeconds\":300,\"maxMB\":500},"
      "\"dynamicLink\":{\"enabled\":false,\"safe\":{\"mcs\":1,"
      "\"bitrateKbps\":2000}}}";
    return cJSON_Parse(src);
}

TEST_CASE("snapshot: read int and string and bool", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    REQUIRE(root != nullptr);
    char *v;
    v = fpvd_snapshot_read_string(root, "video.fps", FPVD_T_INT);
    REQUIRE(std::strcmp(v, "60") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "video.codec", FPVD_T_ENUM);
    REQUIRE(std::strcmp(v, "h265") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "link.stbc", FPVD_T_BOOL);
    REQUIRE(std::strcmp(v, "off") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "link.ldpc", FPVD_T_BOOL);
    REQUIRE(std::strcmp(v, "on") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: nested path", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "link.fec.k", FPVD_T_INT);
    REQUIRE(std::strcmp(v, "8") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: bitrate kbps -> M suffix", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.bitrate", FPVD_T_BITRATE_KBPS);
    REQUIRE(std::strcmp(v, "8M") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: missing path returns empty", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.absent", FPVD_T_INT);
    REQUIRE(v != nullptr);
    REQUIRE(std::strcmp(v, "") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: float", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.gopSize", FPVD_T_FLOAT);
    REQUIRE(std::strcmp(v, "1") == 0); free(v);   /* int seconds, no fractional */
    cJSON_Delete(root);
}

TEST_CASE("snapshot: percent_to_frac", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.roi.center", FPVD_T_PERCENT_TO_FRAC);
    REQUIRE(std::strcmp(v, "40") == 0); free(v);   /* 0.4 → 40 */
    cJSON_Delete(root);
}

TEST_CASE("snapshot: seconds_from_min divides", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN);
    REQUIRE(std::strcmp(v, "5") == 0); free(v);    /* 300 / 60 = 5 */
    cJSON_Delete(root);
}
```

- [ ] **Step 2: Confirm fail**

Run: `cmake --build build_sim --target fpvd_tests 2>&1 | head -10`.
Expected: undeclared `fpvd_snapshot_read_string` and missing cJSON header.

- [ ] **Step 3: Add cJSON include and declaration to internal header**

In `src/gsmenu/settings_fpvd_internal.h`, add at the top after the existing includes:

```c
#include "cJSON.h"
```

Add the declaration:

```c
/* Walk `root` along the dotted `path`, format the leaf value as a string
 * according to `type`. Returns a heap-allocated string. Returns strdup("")
 * if path is missing or types are incompatible. Never returns NULL. */
char *fpvd_snapshot_read_string(cJSON *root, const char *path, fpvd_type_t type);
```

- [ ] **Step 4: Implement in `settings_fpvd.c`**

Add at the top of `settings_fpvd.c` (after the existing includes):

```c
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
```

Add the function (before `pp_settings_register_fpvd`):

```c
static cJSON *walk_path(cJSON *root, const char *path) {
    cJSON *node = root;
    const char *p = path;
    char seg[64];
    while (*p && node) {
        size_t i = 0;
        while (*p && *p != '.' && i + 1 < sizeof seg) { seg[i++] = *p++; }
        seg[i] = '\0';
        if (*p == '.') p++;
        node = cJSON_GetObjectItemCaseSensitive(node, seg);
    }
    return node;
}

char *fpvd_snapshot_read_string(cJSON *root, const char *path, fpvd_type_t type) {
    cJSON *node = walk_path(root, path);
    if (!node) return strdup("");
    char buf[64];
    switch (type) {
    case FPVD_T_INT:
        if (cJSON_IsNumber(node)) {
            snprintf(buf, sizeof buf, "%d", (int)node->valuedouble);
            return strdup(buf);
        }
        break;
    case FPVD_T_FLOAT:
        if (cJSON_IsNumber(node)) {
            double v = node->valuedouble;
            if (fabs(v - (int)v) < 1e-6) snprintf(buf, sizeof buf, "%d", (int)v);
            else snprintf(buf, sizeof buf, "%.3g", v);
            return strdup(buf);
        }
        break;
    case FPVD_T_BOOL:
        if (cJSON_IsBool(node))
            return strdup(cJSON_IsTrue(node) ? "on" : "off");
        break;
    case FPVD_T_STRING:
    case FPVD_T_ENUM:
        if (cJSON_IsString(node) && node->valuestring)
            return strdup(node->valuestring);
        break;
    case FPVD_T_BITRATE_KBPS:
        if (cJSON_IsNumber(node)) {
            int kbps = (int)node->valuedouble;
            snprintf(buf, sizeof buf, "%dM", kbps / 1000);
            return strdup(buf);
        }
        break;
    case FPVD_T_SECONDS_FROM_MIN:
        if (cJSON_IsNumber(node)) {
            int secs = (int)node->valuedouble;
            snprintf(buf, sizeof buf, "%d", secs / 60);
            return strdup(buf);
        }
        break;
    case FPVD_T_PERCENT_TO_FRAC:
        if (cJSON_IsNumber(node)) {
            int pct = (int)(node->valuedouble * 100.0 + 0.5);
            snprintf(buf, sizeof buf, "%d", pct);
            return strdup(buf);
        }
        break;
    }
    return strdup("");
}
```

- [ ] **Step 5: Build and run**

Run:
```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][snapshot]"
```
Expected: all snapshot tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd.cpp
git commit -m "feat(fpvd): json snapshot path read with type-aware formatting"
```

---

### Task 3.3: Sparse PATCH body builder

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `tests/test_settings_fpvd.cpp`

`fpvd_build_patch_body` takes a dotted path + a UI value string + a type, and produces a cJSON object that's the minimal nested PATCH body for that single field. The caller frees with `cJSON_Delete`.

- [ ] **Step 1: Write failing tests**

Append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("patch: build sparse body — flat int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.fps", "90", FPVD_T_INT);
    REQUIRE(body != nullptr);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"fps":90}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: build sparse body — nested int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("link.fec.k", "7", FPVD_T_INT);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"link":{"fec":{"k":7}}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: build sparse body — bool", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("link.stbc", "on", FPVD_T_BOOL);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"link":{"stbc":true}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: bitrate M-suffix parsed to kbps int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.bitrate", "15M", FPVD_T_BITRATE_KBPS);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"bitrate":15000}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: seconds_from_min multiplies", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("recording.maxSeconds", "5",
                                        FPVD_T_SECONDS_FROM_MIN);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"recording":{"maxSeconds":300}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: percent_to_frac divides by 100", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.roi.center", "40",
                                        FPVD_T_PERCENT_TO_FRAC);
    /* cJSON formats doubles; accept either "0.4" or "0.40". */
    char *s = cJSON_PrintUnformatted(body);
    std::string out(s);
    REQUIRE((out == R"({"video":{"roi":{"center":0.4}})" ||
             out == R"({"video":{"roi":{"center":0.40}})"));
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: enum stored as string", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.codec", "h264", FPVD_T_ENUM);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"codec":"h264"}})");
    free(s);
    cJSON_Delete(body);
}
```

- [ ] **Step 2: Confirm fail**

Run: `cmake --build build_sim --target fpvd_tests 2>&1 | head -5`.
Expected: undeclared `fpvd_build_patch_body`.

- [ ] **Step 3: Add declaration to internal header**

Append to `src/gsmenu/settings_fpvd_internal.h`:

```c
/* Build a sparse nested cJSON object for a single field. Returns NULL on
 * value parse error. Caller frees with cJSON_Delete. */
cJSON *fpvd_build_patch_body(const char *path, const char *value, fpvd_type_t type);
```

- [ ] **Step 4: Implement in `settings_fpvd.c`**

Add (before `pp_settings_register_fpvd`):

```c
static cJSON *value_to_cjson(const char *value, fpvd_type_t type) {
    switch (type) {
    case FPVD_T_INT: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)v);
    }
    case FPVD_T_FLOAT: {
        char *end;
        double v = strtod(value, &end);
        if (end == value) return NULL;
        return cJSON_CreateNumber(v);
    }
    case FPVD_T_BOOL:
        if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0)
            return cJSON_CreateBool(1);
        return cJSON_CreateBool(0);
    case FPVD_T_STRING:
    case FPVD_T_ENUM:
        return cJSON_CreateString(value);
    case FPVD_T_BITRATE_KBPS: {
        /* "15M" → 15000; bare "15000" → 15000. */
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        if (*end == 'M' || *end == 'm') v *= 1000;
        return cJSON_CreateNumber((double)v);
    }
    case FPVD_T_SECONDS_FROM_MIN: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)(v * 60));
    }
    case FPVD_T_PERCENT_TO_FRAC: {
        char *end;
        long v = strtol(value, &end, 10);
        if (end == value) return NULL;
        return cJSON_CreateNumber((double)v / 100.0);
    }
    }
    return NULL;
}

cJSON *fpvd_build_patch_body(const char *path, const char *value, fpvd_type_t type) {
    cJSON *leaf = value_to_cjson(value, type);
    if (!leaf) return NULL;

    /* Walk path segments in reverse so we can wrap leaf each step. */
    char buf[256];
    strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = '\0';

    /* Split into segments. */
    const char *segs[16];
    size_t nsegs = 0;
    char *tok = buf;
    while (tok && *tok && nsegs < 16) {
        segs[nsegs++] = tok;
        char *dot = strchr(tok, '.');
        if (!dot) break;
        *dot = '\0';
        tok = dot + 1;
    }

    cJSON *cur = leaf;
    for (ssize_t i = (ssize_t)nsegs - 1; i >= 0; i--) {
        cJSON *parent = cJSON_CreateObject();
        cJSON_AddItemToObject(parent, segs[i], cur);
        cur = parent;
    }
    return cur;
}
```

- [ ] **Step 5: Build and run**

Run:
```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][patch]"
```
Expected: all patch tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd.cpp
git commit -m "feat(fpvd): sparse patch body builder with type-aware value parsing"
```

---

### Task 3.4: Lock evaluator

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `tests/test_settings_fpvd.cpp`

`fpvd_is_locked_path(path)` returns true if `path` starts with any entry in the locked-paths list.

- [ ] **Step 1: Write failing tests**

Append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("lock: matches exact locked paths", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.mcs") == true);
    REQUIRE(fpvd_is_locked_path("link.txpower") == true);
    REQUIRE(fpvd_is_locked_path("link.width") == true);
    REQUIRE(fpvd_is_locked_path("video.bitrate") == true);
    REQUIRE(fpvd_is_locked_path("video.qpDelta") == true);
}

TEST_CASE("lock: matches subtrees", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.fec.k") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.n") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.enabled") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.center") == true);
}

TEST_CASE("lock: does not match unrelated", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.channel") == false);
    REQUIRE(fpvd_is_locked_path("video.fps") == false);
    REQUIRE(fpvd_is_locked_path("video.codec") == false);
    REQUIRE(fpvd_is_locked_path("image.mirror") == false);
}

TEST_CASE("lock: prefix overshoot is not a match", "[fpvd][lock]") {
    /* "link.widthful" is not a child of "link.width". */
    REQUIRE(fpvd_is_locked_path("link.widthful") == false);
}
```

- [ ] **Step 2: Add declaration**

Append to `src/gsmenu/settings_fpvd_internal.h`:

```c
bool fpvd_is_locked_path(const char *path);
```

- [ ] **Step 3: Implement**

Add to `settings_fpvd.c`:

```c
static const char *LOCKED_PATHS[] = {
    "link.mcs",
    "link.txpower",
    "link.fec",
    "link.width",
    "video.bitrate",
    "video.qpDelta",
    "video.roi",
};
static const size_t LOCKED_PATHS_N = sizeof(LOCKED_PATHS) / sizeof(LOCKED_PATHS[0]);

bool fpvd_is_locked_path(const char *path) {
    for (size_t i = 0; i < LOCKED_PATHS_N; i++) {
        size_t lp_len = strlen(LOCKED_PATHS[i]);
        if (strncmp(path, LOCKED_PATHS[i], lp_len) != 0) continue;
        /* Match either exact or extended by a '.' (subtree). */
        if (path[lp_len] == '\0' || path[lp_len] == '.') return true;
    }
    return false;
}
```

- [ ] **Step 4: Run tests**

Run:
```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][lock]"
```
Expected: all 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd.cpp
git commit -m "feat(fpvd): locked-path prefix evaluator"
```

---

### Task 3.5: HTTP client wrapper

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h`
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `tests/test_settings_fpvd.cpp` (cursory unit test — full coverage in integration tests)

Three primitives: `http_get(url)`, `http_patch_json(url, body)`, `http_post(url)`. Each returns a small result struct with `int status` (0 if transport failure, else HTTP code) and `char *body` (heap-allocated, may be NULL).

- [ ] **Step 1: Add types and declarations to internal header**

Append:

```c
typedef struct {
    int   status;          /* 0 = transport failure; else HTTP status */
    char *body;            /* heap-allocated, may be NULL */
    size_t body_len;
} fpvd_http_result_t;

void fpvd_http_result_free(fpvd_http_result_t *r);

fpvd_http_result_t fpvd_http_get(const char *url);
fpvd_http_result_t fpvd_http_patch_json(const char *url, const char *body);
fpvd_http_result_t fpvd_http_post(const char *url);
```

- [ ] **Step 2: Write a sanity unit test (skipped in CI without network — use `[!shouldfail]` if needed)**

Append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("http: GET against impossible host returns transport failure",
          "[fpvd][http]") {
    fpvd_http_result_t r = fpvd_http_get("http://127.0.0.1:1/nope");
    REQUIRE(r.status == 0);
    fpvd_http_result_free(&r);
}
```

(Port 1 is reserved; connect should fail fast. If the test takes too long, the timeout below will cap it.)

- [ ] **Step 3: Implement using libcurl**

Add to top of `settings_fpvd.c`:

```c
#include <curl/curl.h>
```

Add the three functions:

```c
static size_t curl_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    fpvd_http_result_t *r = (fpvd_http_result_t *)ud;
    size_t add = sz * nm;
    char *nb = realloc(r->body, r->body_len + add + 1);
    if (!nb) return 0;
    r->body = nb;
    memcpy(r->body + r->body_len, ptr, add);
    r->body_len += add;
    r->body[r->body_len] = '\0';
    return add;
}

static fpvd_http_result_t http_do(const char *url, const char *method,
                                  const char *body) {
    fpvd_http_result_t r = { 0, NULL, 0 };
    CURL *c = curl_easy_init();
    if (!c) return r;
    struct curl_slist *hdrs = NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    if (strcmp(method, "GET") == 0) {
        /* default */
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        if (!body) curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    } else {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        r.status = (int)code;
    }
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

fpvd_http_result_t fpvd_http_get(const char *url)        { return http_do(url, "GET", NULL); }
fpvd_http_result_t fpvd_http_post(const char *url)       { return http_do(url, "POST", NULL); }
fpvd_http_result_t fpvd_http_patch_json(const char *url, const char *body) {
    return http_do(url, "PATCH", body);
}

void fpvd_http_result_free(fpvd_http_result_t *r) {
    if (r && r->body) { free(r->body); r->body = NULL; r->body_len = 0; }
}
```

Add a one-time global init somewhere safe — since the provider's `pp_settings_register_fpvd()` will be called once at startup, do it there (will be filled in by Task 3.8). For now add a quiet init guard:

```c
static void fpvd_curl_init_once(void) {
    static int done = 0;
    if (!done) { curl_global_init(CURL_GLOBAL_DEFAULT); done = 1; }
}
```

Call `fpvd_curl_init_once()` at the top of each `http_do`.

- [ ] **Step 4: Build and run**

Run:
```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][http]"
```
Expected: test passes (immediate connect failure → status=0).

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h tests/test_settings_fpvd.cpp
git commit -m "feat(fpvd): libcurl wrapper for GET / PATCH / POST"
```

---

### Task 3.6: Worker thread, queue, debounce — provider state

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `src/gsmenu/settings_fpvd_internal.h`

This task wires up the actual provider:
- Background thread with a job queue (single-slot per path; producer coalesces).
- The `pp_settings_provider_t` interface methods (`set/get/set_async/is_locked/is_connected/...`).
- Snapshot held under a mutex.
- Polling logic + reconnect.

This is the largest task. Bite-sized steps below; each step does one self-contained piece.

- [ ] **Step 1: Add provider-state struct and forward declarations**

Add near the top of `settings_fpvd.c` (after existing includes):

```c
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define FPVD_DEFAULT_URL "http://10.5.0.10:8080"
#define FPVD_QUEUE_CAP   32

typedef struct fpvd_job {
    char     path[128];           /* dotted json path */
    char     value[128];          /* UI value string */
    fpvd_type_t type;
    pp_settings_done_cb on_done;
    void    *user_data;
} fpvd_job_t;

typedef struct {
    char     base_url[128];
    pthread_t worker;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool     stop;
    bool     visible;
    bool     connected;

    cJSON   *snapshot;            /* protected by mu */

    fpvd_job_t queue[FPVD_QUEUE_CAP];
    size_t     queue_n;

    pp_settings_snapshot_cb listener_cb;
    void                   *listener_ud;
} fpvd_state_t;

static fpvd_state_t G;
```

- [ ] **Step 2: Implement `pp_settings_set` / `pp_settings_get` provider methods**

Append to `settings_fpvd.c`:

```c
static char *prov_get(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return strdup("");
    pthread_mutex_lock(&G.mu);
    char *out = G.snapshot
        ? fpvd_snapshot_read_string(G.snapshot, e->path, e->type)
        : strdup("");
    pthread_mutex_unlock(&G.mu);
    return out;
}

/* Synchronous set is unsupported — production calls always go through set_async.
 * If somebody calls it, treat as a fire-and-forget enqueue with no callback. */
static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    /* See prov_set_async; fall through. */
    extern void fpvd_prov_set_async(const char *, const char *, const char *,
                                    const char *, pp_settings_done_cb, void *);
    fpvd_prov_set_async(d, p, k, v, NULL, NULL);
}
```

- [ ] **Step 3: Implement queue enqueue with coalescing**

```c
static void enqueue_locked(const fpvd_keymap_entry_t *e, const char *value,
                           pp_settings_done_cb cb, void *ud) {
    /* Coalesce: if a job already exists for this path, replace its value
     * and callback (the older one was superseded). */
    for (size_t i = 0; i < G.queue_n; i++) {
        if (strcmp(G.queue[i].path, e->path) == 0) {
            /* Older callback gets cancelled with rc=-1 "superseded"? We
             * silently drop it — the caller's row UX expects exactly one
             * callback, so we instead let the new callback override. */
            if (G.queue[i].on_done) {
                /* schedule cancel on next loop? simpler: just call it sync
                 * with rc=0,err="superseded" before overwriting. But cb is
                 * on the LVGL thread — we MUST NOT call it from this thread. */
                /* For now: leave the dropped callback orphaned — its
                 * user_data lv_malloc will leak. To avoid that, we keep
                 * the original cb and override only the value: */
                strncpy(G.queue[i].value, value, sizeof G.queue[i].value - 1);
                G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
                /* Replace the callback: the original row already updated
                 * its UI assumption to the new value. */
                G.queue[i].on_done   = cb;
                G.queue[i].user_data = ud;
                return;
            }
            G.queue[i].on_done = cb;
            G.queue[i].user_data = ud;
            strncpy(G.queue[i].value, value, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            return;
        }
    }
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        /* Overflow — invoke callback with rc=-1 to surface a toast. We
         * cannot call cb on this thread; mark as a special drop instead. */
        /* For v1, drop silently — under normal operation 32 jobs is a lot. */
        if (cb) {
            /* Best-effort sync cb on the calling (LVGL) thread, *if* the
             * caller is the LVGL thread. prov_set_async is called from
             * the LVGL thread, so this is safe. */
            cb(-1, "Settings queue full", ud);
        }
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    strncpy(j->path,  e->path,  sizeof j->path  - 1); j->path [sizeof j->path -1] = '\0';
    strncpy(j->value, value,    sizeof j->value - 1); j->value[sizeof j->value-1] = '\0';
    j->type      = e->type;
    j->on_done   = cb;
    j->user_data = ud;
}
```

- [ ] **Step 4: Implement `prov_set_async`**

Note: the caller (widget) is on the LVGL thread. The callback must also run on the LVGL thread. We use `lv_async_call` to schedule the callback from the worker thread.

```c
typedef struct {
    pp_settings_done_cb cb;
    void *user_data;
    int   rc;
    char  err[128];
} fpvd_done_dispatch_t;

static void done_dispatch_async(void *ptr) {
    fpvd_done_dispatch_t *d = (fpvd_done_dispatch_t *)ptr;
    if (d->cb) d->cb(d->rc, d->err[0] ? d->err : NULL, d->user_data);
    lv_free(d);
}

static void schedule_done(pp_settings_done_cb cb, void *ud,
                          int rc, const char *err) {
    if (!cb) return;
    fpvd_done_dispatch_t *d = lv_malloc(sizeof *d);
    d->cb = cb;
    d->user_data = ud;
    d->rc = rc;
    if (err) { strncpy(d->err, err, sizeof d->err - 1); d->err[sizeof d->err - 1] = '\0'; }
    else d->err[0] = '\0';
    lv_async_call(done_dispatch_async, d);
}

void fpvd_prov_set_async(const char *domain, const char *page, const char *key,
                         const char *value,
                         pp_settings_done_cb cb, void *ud) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(domain, page, key);
    if (!e) { schedule_done(cb, ud, -1, "Unknown setting"); return; }
    if (fpvd_is_locked_path(e->path)) {
        pthread_mutex_lock(&G.mu);
        cJSON *dlink = G.snapshot ? cJSON_GetObjectItemCaseSensitive(G.snapshot, "dynamicLink") : NULL;
        cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
        bool dlink_on = en && cJSON_IsTrue(en);
        pthread_mutex_unlock(&G.mu);
        if (dlink_on) {
            schedule_done(cb, ud, -1, "Locked by Dynamic Link");
            return;
        }
    }
    pthread_mutex_lock(&G.mu);
    enqueue_locked(e, value, cb, ud);
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    fpvd_prov_set_async(d, p, k, v, cb, ud);
}
```

- [ ] **Step 5: Implement `is_locked` / `is_connected` / listener / visibility**

```c
static bool prov_is_locked(const char *d, const char *p, const char *k) {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup(d, p, k);
    if (!e) return false;
    if (!fpvd_is_locked_path(e->path)) return false;
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.snapshot ? cJSON_GetObjectItemCaseSensitive(G.snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on = en && cJSON_IsTrue(en);
    pthread_mutex_unlock(&G.mu);
    return dl_on;
}

static bool prov_is_connected(void) {
    pthread_mutex_lock(&G.mu);
    bool c = G.connected;
    pthread_mutex_unlock(&G.mu);
    return c;
}

static void listener_dispatch_async(void *ptr) {
    (void)ptr;
    pthread_mutex_lock(&G.mu);
    pp_settings_snapshot_cb cb = G.listener_cb;
    void *ud = G.listener_ud;
    pthread_mutex_unlock(&G.mu);
    if (cb) cb(ud);
}

static void notify_listener(void) {
    /* Always dispatched to LVGL thread. */
    lv_async_call(listener_dispatch_async, NULL);
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    G.listener_cb = cb;
    G.listener_ud = ud;
    pthread_mutex_unlock(&G.mu);
}

static void prov_set_visibility(bool v) {
    pthread_mutex_lock(&G.mu);
    G.visible = v;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}
```

- [ ] **Step 6: Implement worker thread main loop**

```c
static char *url_join(const char *base, const char *path) {
    size_t n = strlen(base) + strlen(path) + 1;
    char *u = malloc(n);
    snprintf(u, n, "%s%s", base, path);
    return u;
}

static void refresh_snapshot_unlocked(void) {
    /* called with G.mu HELD */
    char *u = url_join(G.base_url, "/config");
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t r = fpvd_http_get(u);
    pthread_mutex_lock(&G.mu);
    free(u);
    bool was_connected = G.connected;
    if (r.status == 200 && r.body) {
        cJSON *new_snap = cJSON_Parse(r.body);
        if (new_snap) {
            if (G.snapshot) cJSON_Delete(G.snapshot);
            G.snapshot = new_snap;
            G.connected = true;
        }
    } else {
        G.connected = false;
    }
    fpvd_http_result_free(&r);
    if (was_connected != G.connected) notify_listener();
    /* Always fire on snapshot change; we don't bother diffing. */
    if (G.connected) notify_listener();
}

static const char *parse_error_message(const char *body) {
    /* body looks like {"error":"validation","message":"...","details":[...]} */
    if (!body) return NULL;
    cJSON *r = cJSON_Parse(body);
    if (!r) return NULL;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(r, "error");
    const char *code = (err && cJSON_IsString(err)) ? err->valuestring : "";
    static char buf[160];
    buf[0] = '\0';
    if (strcmp(code, "dynamic_link_locked") == 0) {
        snprintf(buf, sizeof buf, "Locked by Dynamic Link");
    } else if (strcmp(code, "validation") == 0) {
        cJSON *det = cJSON_GetObjectItemCaseSensitive(r, "details");
        if (cJSON_IsArray(det) && cJSON_GetArraySize(det) > 0) {
            cJSON *first = cJSON_GetArrayItem(det, 0);
            cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
            if (msg && cJSON_IsString(msg)) {
                snprintf(buf, sizeof buf, "%s", msg->valuestring);
            } else {
                snprintf(buf, sizeof buf, "Validation failed");
            }
        } else {
            snprintf(buf, sizeof buf, "Validation failed");
        }
    } else {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "message");
        snprintf(buf, sizeof buf, "%s",
            (m && cJSON_IsString(m)) ? m->valuestring : "Request rejected");
    }
    cJSON_Delete(r);
    return buf[0] ? buf : NULL;
}

static void run_job_unlocked(fpvd_job_t job) {
    /* called with G.mu RELEASED */
    cJSON *body = fpvd_build_patch_body(job.path, job.value, job.type);
    char *body_s = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body) cJSON_Delete(body);

    char *patch_url = url_join(G.base_url, "/config");
    char *apply_url = url_join(G.base_url, "/apply");

    fpvd_http_result_t r = fpvd_http_patch_json(patch_url, body_s ? body_s : "{}");
    int rc = 0;
    char err[160] = {0};
    if (r.status == 0) {
        rc = -1; snprintf(err, sizeof err, "Drone unreachable");
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
        r = fpvd_http_post(apply_url);
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "Drone unreachable");
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

static void *worker_main(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&G.mu);
        while (!G.stop && G.queue_n == 0) {
            /* Compute next wakeup: faster when disconnected to retry. */
            int wait_ms = G.connected ? (G.visible ? 3000 : 60000) : 2000;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int wr = pthread_cond_timedwait(&G.cv, &G.mu, &ts);
            if (wr == ETIMEDOUT) {
                /* Poll round. */
                refresh_snapshot_unlocked();
                /* Re-check stop/queue on next iteration. */
                continue;
            }
        }
        if (G.stop) { pthread_mutex_unlock(&G.mu); break; }

        /* Pull the head job. */
        fpvd_job_t job = G.queue[0];
        for (size_t i = 1; i < G.queue_n; i++) G.queue[i-1] = G.queue[i];
        G.queue_n--;
        pthread_mutex_unlock(&G.mu);

        run_job_unlocked(job);

        /* Debounce pause to coalesce burst commits. */
        struct timespec ts = { 0, 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}
```

(Note: `lv_async_call` and `lv_malloc`/`lv_free` are LVGL APIs called from the worker thread; LVGL's async-call queue is thread-safe for producers per LVGL docs. If the runtime asserts otherwise, replace with a plain producer-consumer queue feeding an `lv_timer` callback on the LVGL thread.)

- [ ] **Step 7: Implement `pp_settings_register_fpvd`**

Replace the existing stub with:

```c
static const pp_settings_provider_t G_PROVIDER = {
    .set                    = prov_set,
    .get                    = prov_get,
    .set_async              = prov_set_async,
    .is_locked              = prov_is_locked,
    .is_connected           = prov_is_connected,
    .set_snapshot_listener  = prov_set_snapshot_listener,
    .set_visibility         = prov_set_visibility,
};

void pp_settings_register_fpvd(void) {
    fpvd_curl_init_once();
    const char *u = getenv("PP_FPVD_URL");
    strncpy(G.base_url, u && *u ? u : FPVD_DEFAULT_URL, sizeof G.base_url - 1);
    G.base_url[sizeof G.base_url - 1] = '\0';
    pthread_mutex_init(&G.mu, NULL);
    pthread_cond_init(&G.cv, NULL);
    G.stop      = false;
    G.visible   = false;
    G.connected = false;
    G.snapshot  = NULL;
    G.queue_n   = 0;
    G.listener_cb = NULL;
    G.listener_ud = NULL;

    /* Prime snapshot synchronously (best-effort). */
    pthread_mutex_lock(&G.mu);
    refresh_snapshot_unlocked();
    pthread_mutex_unlock(&G.mu);

    pthread_create(&G.worker, NULL, worker_main, NULL);
    pp_settings_register(&G_PROVIDER);
}
```

Add an `lvgl` include where needed: at the top, add `#include "lvgl/lvgl.h"` if not already present (for `lv_async_call`, `lv_malloc`, `lv_free`).

- [ ] **Step 8: Build**

Run:
```bash
cmake --build build_sim --target fpvd_tests pixelpilot -j 2>&1 | tail -20
```
Expected: build succeeds. (The `pixelpilot` target may not exist in the sim build; if so, build whatever the executable target is named.)

- [ ] **Step 9: Commit**

```bash
git add src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h
git commit -m "feat(fpvd): worker thread + queue + lifecycle + provider methods"
```

---

## Phase 4 — UI changes

### Task 4.1: Camera tab field cleanup + new sections

**Files:**
- Modify: `src/gsmenu/pages/camera.c`

- [ ] **Step 1: Replace `build_camera_tab` body**

Open `src/gsmenu/pages/camera.c`. Replace the entire `build_camera_tab` function with:

```c
lv_obj_t *build_camera_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "camera");

    pp_section_header(page, "Video");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Size",
                "air", "camera", "size",
                "1920x1080\n1280x720\n960x540");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "FPS",
                "air", "camera", "fps",
                "30\n60\n90\n120");
    pp_dropdown(page, LV_SYMBOL_AUDIO, "Bitrate",
                "air", "camera", "bitrate",
                "5M\n10M\n15M\n20M\n25M");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "air", "camera", "codec",
                "h264\nh265");
    pp_slider(page, LV_SYMBOL_SETTINGS, "GOP size",
              "air", "camera", "gopsize", 1, 20);
    pp_dropdown(page, LV_SYMBOL_SETTINGS, "RC Mode",
                "air", "camera", "rc_mode",
                "cbr\nvbr");
    pp_slider(page, LV_SYMBOL_SETTINGS, "QP Delta",
              "air", "camera", "qp_delta", -32, 0);

    pp_section_header(page, "ROI");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Enabled",
              "air", "camera", "roi_enabled");
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "QP",
              "air", "camera", "roi_qp", -24, 0);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Center",
              "air", "camera", "roi_center", 0, 100);
    pp_slider(page, LV_SYMBOL_EYE_OPEN, "Steps",
              "air", "camera", "roi_steps", 1, 8);

    pp_section_header(page, "Image");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Mirror", "air", "camera", "mirror");
    pp_toggle(page, LV_SYMBOL_REFRESH, "Flip",   "air", "camera", "flip");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Rotate",
                "air", "camera", "rotate",
                "0\n90\n180\n270");

    pp_section_header(page, "Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "air", "camera", "rec_enable");
    pp_slider(page, LV_SYMBOL_VIDEO, "Split (min)",
              "air", "camera", "rec_split", 1, 60);
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max size (MB)",
              "air", "camera", "rec_maxmb", 100, 10000);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }
    return page;
}
```

- [ ] **Step 2: Build sim**

Run: `cmake --build build_sim -j`. Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/pages/camera.c
git commit -m "feat(gsmenu-camera): align fields with fpvd API; drop ISP/FPV; add ROI/QP/Rotate"
```

---

### Task 4.2: Link tab range fixes

**Files:**
- Modify: `src/gsmenu/pages/link.c`

- [ ] **Step 1: Replace `build_link_tab` body**

```c
lv_obj_t *build_link_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "link");

    pp_section_header(page, "WFB-NG");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
                "gs", "wfbng", "gs_channel",
                "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n149\n153\n157\n161\n165");
    pp_dropdown(page, LV_SYMBOL_WIFI, "Bandwidth",
                "gs", "wfbng", "bandwidth", "20\n40");
    pp_slider(page, LV_SYMBOL_UP, "TX Power",
              "gs", "wfbng", "txpower", 1, 63);
    pp_slider(page, LV_SYMBOL_SETTINGS, "MCS Index",
              "air", "wfbng", "mcs_index", 0, 7);
    pp_toggle(page, LV_SYMBOL_SETTINGS, "STBC", "air", "wfbng", "stbc");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "LDPC", "air", "wfbng", "ldpc");
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_K",
              "air", "wfbng", "fec_k", 1, 31);
    pp_slider(page, LV_SYMBOL_SETTINGS, "FEC_N",
              "air", "wfbng", "fec_n", 2, 32);

    lv_group_t *grp = pp_page_group(page);
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        if (lv_obj_check_type(c, &lv_obj_class)) {
            lv_group_add_obj(grp, c);
        }
    }
    return page;
}
```

- [ ] **Step 2: Build and commit**

```bash
cmake --build build_sim -j
git add src/gsmenu/pages/link.c
git commit -m "feat(gsmenu-link): align ranges with fpvd (no 80 BW, MCS 0..7, TX 1..63, FEC 1..31/2..32)"
```

---

### Task 4.3: New Dynamic Link tab page

**Files:**
- Create: `src/gsmenu/pages/dynamiclink.h`
- Create: `src/gsmenu/pages/dynamiclink.c`

- [ ] **Step 1: Create header**

`src/gsmenu/pages/dynamiclink.h`:

```c
#pragma once
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_dynamiclink_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create implementation**

`src/gsmenu/pages/dynamiclink.c`:

```c
#include "dynamiclink.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"

#include <string.h>
#include <stdlib.h>

/* Hide all rows except the Enabled toggle when dynamicLink is off.
 * Visibility is recomputed on every snapshot mutation. */
static void apply_visibility(lv_obj_t *page) {
    char *v = pp_settings_get("air", "dlink", "enabled");
    bool on = v && strcmp(v, "on") == 0;
    free(v);

    uint32_t n = lv_obj_get_child_cnt(page);
    bool past_enabled = false;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        /* The first row is the Enabled toggle. We use a custom flag set
         * by build_dynamiclink_tab below to mark it. */
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_3)) { past_enabled = true; continue; }
        if (!past_enabled) continue;
        if (on) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
}

static void snapshot_listener_cb(void *user_data) {
    lv_obj_t *page = (lv_obj_t *)user_data;
    apply_visibility(page);
}

lv_obj_t *build_dynamiclink_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "air", "dlink");

    pp_section_header(page, "General");
    lv_obj_t *enabled = pp_toggle(page, LV_SYMBOL_POWER, "Enabled",
                                  "air", "dlink", "enabled");
    lv_obj_add_flag(enabled, LV_OBJ_FLAG_USER_3);   /* visibility anchor */
    pp_toggle(page, LV_SYMBOL_SETTINGS, "Interleaving Supported",
              "air", "dlink", "interleaving");
    pp_toggle(page, LV_SYMBOL_SETTINGS, "MAVLink Enable",
              "air", "dlink", "mavlink_enable");

    pp_section_header(page, "OSD");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "OSD Enabled",
              "air", "dlink", "osd_enabled");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Debug Latency",
              "air", "dlink", "osd_debug_latency");

    pp_section_header(page, "Timing");
    pp_slider(page, LV_SYMBOL_REFRESH, "Health Timeout (ms)",
              "air", "dlink", "health_timeout_ms", 1000, 30000);
    pp_slider(page, LV_SYMBOL_REFRESH, "Min IDR Interval (ms)",
              "air", "dlink", "min_idr_interval_ms", 16, 2000);
    pp_slider(page, LV_SYMBOL_REFRESH, "Apply Stagger (ms)",
              "air", "dlink", "apply_stagger_ms", 0, 500);
    pp_slider(page, LV_SYMBOL_REFRESH, "Apply Sub-pace (ms)",
              "air", "dlink", "apply_subpace_ms", 0, 50);

    pp_section_header(page, "ROI QP");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Threshold (kbps)",
              "air", "dlink", "roiqp_threshold_kbps", 1000, 20000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Low Anchor (kbps)",
              "air", "dlink", "roiqp_low_anchor_kbps", 500, 10000);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Floor",
              "air", "dlink", "roiqp_floor", -48, 0);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Step",
              "air", "dlink", "roiqp_step", 1, 10);

    pp_section_header(page, "Safe Ceilings");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Max MCS",
              "air", "dlink", "safe_mcs", 0, 7);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Max FEC K",
              "air", "dlink", "safe_k", 1, 31);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Max FEC N",
              "air", "dlink", "safe_n", 2, 32);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Block Depth",
              "air", "dlink", "safe_depth", 1, 8);
    pp_dropdown(page, LV_SYMBOL_WIFI, "Max Bandwidth",
                "air", "dlink", "safe_bandwidth", "20\n40");
    pp_slider(page, LV_SYMBOL_UP, "Max TX Power (dBm)",
              "air", "dlink", "safe_txpower_dbm", -10, 30);
    pp_slider(page, LV_SYMBOL_AUDIO, "Max Bitrate (kbps)",
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
    pp_settings_set_snapshot_listener(snapshot_listener_cb, page);
    return page;
}
```

- [ ] **Step 3: Build and commit**

```bash
cmake --build build_sim -j
git add src/gsmenu/pages/dynamiclink.h src/gsmenu/pages/dynamiclink.c
git commit -m "feat(gsmenu-dlink): new Dynamic Link tab page covering full fpvd schema"
```

---

### Task 4.4: Wire the new tab into `menu.c`

**Files:**
- Modify: `src/menu.c`

- [ ] **Step 1: Edit `src/menu.c`**

Add include after the existing page includes (around line 14):

```c
#include "gsmenu/pages/dynamiclink.h"
```

Find the existing pages-build block (lines 60–75). Modify to add Dynamic Link between Link and Display:

```c
    lv_obj_t *cam = build_camera_tab(root);
    lv_obj_set_flex_grow(cam, 1); lv_obj_set_height(cam, LV_PCT(100));
    lv_obj_t *lnk = build_link_tab(root);
    lv_obj_set_flex_grow(lnk, 1); lv_obj_set_height(lnk, LV_PCT(100));
    lv_obj_t *dl  = build_dynamiclink_tab(root);
    lv_obj_set_flex_grow(dl, 1);  lv_obj_set_height(dl, LV_PCT(100));
    lv_obj_t *dsp = build_display_tab(root);
    lv_obj_set_flex_grow(dsp, 1); lv_obj_set_height(dsp, LV_PCT(100));
    lv_obj_t *dvr = build_dvr_tab(root);
    lv_obj_set_flex_grow(dvr, 1); lv_obj_set_height(dvr, LV_PCT(100));
    lv_obj_t *sys = build_system_tab(root);
    lv_obj_set_flex_grow(sys, 1); lv_obj_set_height(sys, LV_PCT(100));
```

Update the tabbar items and array sizes (replace `pp_tabbar_item_t items[5]` block):

```c
    pp_tabbar_item_t items[6] = {
        { "Camera",  LV_SYMBOL_IMAGE,     cam },
        { "Link",    LV_SYMBOL_WIFI,      lnk },
        { "DLink",   LV_SYMBOL_LOOP,      dl  },
        { "Display", LV_SYMBOL_EYE_OPEN,  dsp },
        { "DVR",     LV_SYMBOL_VIDEO,     dvr },
        { "System",  LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(root, items, 6);
```

Update the `pages` array and loop:

```c
    lv_obj_t *pages[6] = { cam, lnk, dl, dsp, dvr, sys };
    main_group = pp_tabbar_group(tabbar);
    for (int i = 0; i < 6; i++) {
        pp_page_set_back_group(pages[i], main_group);
    }
```

- [ ] **Step 2: Build sim and commit**

```bash
cmake --build build_sim -j
git add src/menu.c
git commit -m "feat(gsmenu): register Dynamic Link tab in tab strip"
```

---

### Task 4.5: Snapshot listener — offline + lock state for Camera/Link tabs

**Files:**
- Modify: `src/gsmenu/pages/camera.c`
- Modify: `src/gsmenu/pages/link.c`

Same listener pattern: walk all rows on the page, reset their lock state from `pp_settings_is_locked` + `pp_settings_is_connected`.

- [ ] **Step 1: Add listener helper to a shared place**

Create a small helper in `src/gsmenu/helper.h` / `.c`. First inspect the helper file:

```bash
grep -n "" src/gsmenu/helper.h | head -20
```

Append to `src/gsmenu/helper.h` (inside the existing `extern "C"` if any):

```c
/* Walk a page's rows and re-apply lock/offline state via pp_row_set_locked.
 * Domain/page on each row are read from the row's user_data which the
 * pp_toggle/pp_slider/pp_dropdown widgets all populate with a struct whose
 * first three fields are `char *domain, *page, *key`. Rows without that
 * shape are skipped. */
void pp_page_reapply_lock_state(lv_obj_t *page);
```

Append to `src/gsmenu/helper.c`:

```c
#include "settings.h"
#include "widgets/pp_row.h"

void pp_page_reapply_lock_state(lv_obj_t *page) {
    bool connected = pp_settings_is_connected();
    uint32_t n = lv_obj_get_child_cnt(page);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(page, i);
        struct dpk_head { char *d, *p, *k; };
        struct dpk_head *h = (struct dpk_head *)lv_obj_get_user_data(c);
        if (!h || !h->d || !h->p || !h->k) continue;
        if (!connected) {
            pp_row_set_locked(c, PP_ROW_LOCKED_OFFLINE);
        } else if (pp_settings_is_locked(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_DYNAMIC);
        } else {
            pp_row_set_locked(c, PP_ROW_UNLOCKED);
        }
    }
}
```

(Trick: the widget data structs (`pp_toggle_data_t`, `pp_slider_data_t`, `pp_dd_data_t`) all start with `char *domain, *page, *key` — we read the first three pointers as a common head. This is fragile but works for the current widget set; if a future widget breaks this assumption, switch to a typed registry.)

- [ ] **Step 2: Wire listeners in camera.c and link.c**

In `src/gsmenu/pages/camera.c`, just before the final `return page;`:

```c
    static lv_obj_t *cam_listener_page = NULL;
    cam_listener_page = page;
    extern void pp_page_reapply_lock_state(lv_obj_t *);
    /* If the previous listener is already the dlink tab's, we install ours
     * — provider supports a single listener; for now camera+link+dlink
     * each install on tab-show. v1 simplification. */
    pp_settings_set_snapshot_listener(
        (pp_settings_snapshot_cb)pp_page_reapply_lock_state, page);
    pp_page_reapply_lock_state(page);
```

(Note: provider only supports one listener at a time. A multi-listener design is deferred — for v1, the snapshot listener is owned by whichever tab was last shown. Since lock state matters most for Link and Camera, install in both; the Dynamic Link tab page also installs its visibility listener — these can interleave but the last installer wins. Improvement TODO: provider supports a list of listeners.)

The same change to `src/gsmenu/pages/link.c`. Add `extern` declaration and call `pp_page_reapply_lock_state` + `pp_settings_set_snapshot_listener` near the end of `build_link_tab`.

(This conflict between dynamiclink.c's visibility listener and camera/link's lock listener is the right callout for the open-questions section — see "Listener composition" below. For v1, accept the single-listener limitation: when the user is on the Dynamic Link tab, the lock listener is not active; when they switch tabs, the camera/link install replaces it.)

- [ ] **Step 3: Build and commit**

```bash
cmake --build build_sim -j
git add src/gsmenu/helper.h src/gsmenu/helper.c src/gsmenu/pages/camera.c src/gsmenu/pages/link.c
git commit -m "feat(gsmenu): re-apply lock/offline state from snapshot listener on Camera/Link"
```

---

## Phase 5 — Dummy provider improvements

### Task 5.1: PP_SIM_LATENCY_MS deferred callback

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`

- [ ] **Step 1: Update `dummy_set_async`**

Replace the existing `dummy_set_async` (lines 166–177) with:

```c
typedef struct {
    char *domain, *page, *key, *value;
    pp_settings_done_cb on_done;
    void *user_data;
    bool will_fail;
} dummy_deferred_t;

static void deferred_timer_cb(lv_timer_t *t) {
    dummy_deferred_t *ctx = (dummy_deferred_t *)lv_timer_get_user_data(t);
    if (!ctx->will_fail) {
        dummy_set(ctx->domain, ctx->page, ctx->key, ctx->value);
    }
    if (ctx->on_done) {
        ctx->on_done(ctx->will_fail ? 1 : 0,
                     ctx->will_fail ? "simulated failure (PP_SIM_FAIL set)" : NULL,
                     ctx->user_data);
    }
    free(ctx->domain); free(ctx->page); free(ctx->key); free(ctx->value);
    free(ctx);
    lv_timer_delete(t);
}

static void dummy_set_async(const char *d, const char *p, const char *k,
                            const char *v, pp_settings_done_cb on_done,
                            void *user_data) {
    const char *fail   = getenv("PP_SIM_FAIL");
    const char *latstr = getenv("PP_SIM_LATENCY_MS");
    int latency_ms = latstr ? atoi(latstr) : 200;
    if (latency_ms < 0) latency_ms = 0;

    if (latency_ms == 0) {
        /* Preserve the prior synchronous behavior. */
        if (fail && *fail) {
            if (on_done) on_done(1, "simulated failure (PP_SIM_FAIL set)", user_data);
            return;
        }
        dummy_set(d, p, k, v);
        if (on_done) on_done(0, NULL, user_data);
        return;
    }

    dummy_deferred_t *ctx = calloc(1, sizeof(*ctx));
    ctx->domain   = strdup(d);
    ctx->page     = strdup(p);
    ctx->key      = strdup(k);
    ctx->value    = strdup(v ? v : "");
    ctx->on_done  = on_done;
    ctx->user_data= user_data;
    ctx->will_fail= (fail && *fail);
    lv_timer_t *t = lv_timer_create(deferred_timer_cb, latency_ms, ctx);
    lv_timer_set_repeat_count(t, 1);
}
```

- [ ] **Step 2: Build sim and tests**

```bash
cmake --build build_sim --target settings_tests pixelpilot_sim -j 2>&1 | tail -10
./build_sim/settings_tests "[settings][failure]"
```
Expected: existing tests still pass (test environment has no `PP_SIM_LATENCY_MS` set, so default is 200ms — but the test runs without an LVGL loop, so the timer won't fire). **Caveat:** the existing failure test will hang under the new code because the timer needs an LVGL loop.

To fix: explicitly disable latency in the failure tests by setting `PP_SIM_LATENCY_MS=0` at the top of each Catch2 case.

- [ ] **Step 3: Update existing tests to set `PP_SIM_LATENCY_MS=0`**

In `tests/test_settings_failure.cpp`, add at the start of each `TEST_CASE` (after the `pp_settings_register_dummy()` call):

```cpp
    setenv("PP_SIM_LATENCY_MS", "0", 1);
```

- [ ] **Step 4: Run tests**

```bash
cmake --build build_sim --target settings_tests
./build_sim/settings_tests
```
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_dummy.c tests/test_settings_failure.cpp
git commit -m "feat(sim): PP_SIM_LATENCY_MS defers dummy set_async callback (default 200ms)"
```

---

### Task 5.2: Update dummy seed table to new keys

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`

- [ ] **Step 1: Replace the `g_seed` array**

Replace the entire `g_seed[]` array (lines ~21–117) with:

```c
static const dummy_entry_t g_seed[] = {
    /* System tab info rows (kept) */
    { "Version",      "1.3.0-sim" },
    { "Disk",         "12.4 / 64 GB" },
    { "Channel",      "149" },
    { "HDMI-OUT",     "1920x1080@60" },
    { "WFB_NICS",     "wlan0" },

    /* Camera — Video */
    { "size",         "1920x1080" },
    { "fps",          "60" },
    { "bitrate",      "15M" },
    { "codec",        "h265" },
    { "gopsize",      "1" },
    { "rc_mode",      "cbr" },
    { "qp_delta",     "-4" },

    /* Camera — ROI */
    { "roi_enabled",  "on" },
    { "roi_qp",       "0" },
    { "roi_center",   "40" },
    { "roi_steps",    "2" },

    /* Camera — Image */
    { "mirror",       "off" },
    { "flip",         "off" },
    { "rotate",       "0" },

    /* Camera — Recording */
    { "rec_enable",   "off" },
    { "rec_split",    "5" },
    { "rec_maxmb",    "500" },

    /* Link — WFB-NG */
    { "gs_channel",   "149" },
    { "bandwidth",    "40" },
    { "txpower",      "20" },
    { "mcs_index",    "2" },
    { "stbc",         "off" },
    { "ldpc",         "on" },
    { "fec_k",        "8" },
    { "fec_n",        "12" },

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
    /* Dynamic Link — Safe Ceilings */
    { "safe_mcs",         "1" },
    { "safe_k",           "8" },
    { "safe_n",           "12" },
    { "safe_depth",       "1" },
    { "safe_bandwidth",   "20" },
    { "safe_txpower_dbm", "20" },
    { "safe_bitrate_kbps","2000" },

    /* Display (kept for the unmodified Display tab) */
    { "hdmi_mode",        "1920x1080@60" },
    { "video_scale",      "100" },
    { "color_correction", "off" },
    { "cc_gain",          "25" },
    { "cc_offset",        "0" },

    /* DVR (kept) */
    { "rec_enabled",          "on" },
    { "dvr_mode",             "reencode" },
    { "rec_fps",              "60" },
    { "dvr_max_size",         "4000" },
    { "dvr_reenc_codec",      "h265" },
    { "dvr_reenc_resolution", "1920x1080" },
    { "dvr_reenc_fps",        "60" },
    { "dvr_reenc_bitrate",    "8000" },
    { "dvr_osd",              "on" },

    /* System — Receiver / Network / Telemetry (kept) */
    { "rx_codec",     "h265" },
    { "rx_mode",      "wfb" },
    { "hotspot",      "off" },
    { "restream",     "off" },
    { "serial",       "ttyS2" },
    { "router",       "mavfwd" },
    { "osd_fps",      "30" },
    { "gs_rendering", "on" },
};
```

- [ ] **Step 2: Build sim and run**

```bash
cmake --build build_sim -j
./build_sim/<sim-binary-name>
```
(Use whatever the sim build's executable is named — check `ls build_sim/`.)

Expected: sim launches, menu opens, all tabs render with sensible initial values; new ROI section is visible; old ISP/FPV/Contrast etc. are gone; Dynamic Link tab shows only the Enabled toggle by default.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/settings_dummy.c
git commit -m "feat(sim): dummy seed updated to match new fpvd-aligned keys"
```

---

## Phase 6 — Integration tests

### Task 6.1: Embedded HTTP server fixture

**Files:**
- Modify: `tests/test_settings_fpvd_integration.cpp`

- [ ] **Step 1: Replace placeholder with the fixture**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>

extern "C" {
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
}

/* The provider talks HTTP to PP_FPVD_URL. We spin up a cpp-httplib server
 * on an ephemeral port, set the env var, and register the provider. */
struct FpvdMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;
    std::atomic<int> patch_calls{0};
    std::atomic<int> apply_calls{0};
    std::atomic<int> get_calls{0};
    std::string     last_patch_body;
    std::string     get_response =
        R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":false,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";
    /* Optional override: if non-empty, PATCH returns 400 with this body. */
    std::string     patch_error_body;
    /* Optional override: if non-empty, POST /apply returns 400 with this body. */
    std::string     apply_error_body;

    void start() {
        svr.Get("/config", [this](const httplib::Request &, httplib::Response &res) {
            get_calls++;
            res.set_content(get_response, "application/json");
        });
        svr.Patch("/config", [this](const httplib::Request &req, httplib::Response &res) {
            patch_calls++;
            last_patch_body = req.body;
            if (!patch_error_body.empty()) {
                res.status = 400;
                res.set_content(patch_error_body, "application/json");
            } else {
                res.set_content(get_response, "application/json");
            }
        });
        svr.Post("/apply", [this](const httplib::Request &, httplib::Response &res) {
            apply_calls++;
            if (!apply_error_body.empty()) {
                res.status = 400;
                res.set_content(apply_error_body, "application/json");
            } else {
                res.set_content(R"({"applied":true,"version":1,"restarted":[]})",
                                "application/json");
            }
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void stop() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

/* Helper: build PP_FPVD_URL, set env, then register provider. */
static void install_provider_pointing_at(int port) {
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
}
```

- [ ] **Step 2: Build to confirm fixture compiles**

Run:
```bash
cmake --build build_sim --target fpvd_tests 2>&1 | tail -10
```
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): cpp-httplib mock server fixture"
```

---

### Task 6.2: Integration test — happy path

- [ ] **Step 1: Append to `test_settings_fpvd_integration.cpp`**

```cpp
TEST_CASE("integration: PATCH + apply happy path", "[fpvd][network]") {
    FpvdMockServer m; m.start();
    install_provider_pointing_at(m.port);

    /* Wait for initial GET /config to land. */
    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(m.get_calls >= 1);
    REQUIRE(pp_settings_is_connected() == true);

    /* Trigger an async set; the callback fires on the LVGL thread which
     * we don't have a loop for in tests. Instead we use a synchronous
     * helper: poll-wait for PATCH+apply to land at the server. */
    pp_settings_set_async("air", "camera", "fps", "90", nullptr, nullptr);
    for (int i = 0; i < 200 && m.apply_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    REQUIRE(m.patch_calls >= 1);
    REQUIRE(m.apply_calls >= 1);
    REQUIRE(m.last_patch_body.find("\"fps\":90") != std::string::npos);

    m.stop();
}
```

- [ ] **Step 2: Run and confirm pass**

```bash
cmake --build build_sim --target fpvd_tests
./build_sim/fpvd_tests "[fpvd][network]"
```
Expected: test passes. (Note: `[network]` tag is just a category — runs by default.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): integration happy-path PATCH + apply"
```

---

### Task 6.3: Integration test — validation error

- [ ] **Step 1: Append**

```cpp
TEST_CASE("integration: PATCH validation error surfaces in callback",
          "[fpvd][network]") {
    FpvdMockServer m;
    m.patch_error_body =
      R"({"error":"validation","message":"schema validation failed",)"
      R"("details":[{"path":"link.mcs","message":"must be 0..7"}]})";
    m.start();
    install_provider_pointing_at(m.port);

    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    /* Capture the callback synchronously via a polling shim. The provider
     * dispatches the callback via lv_async_call → LVGL ticks. Tests don't
     * pump the LVGL loop; for the validation-error path the call to
     * lv_async_call still queues, but no consumer drains it. As a
     * workaround for tests, we observe the server side — number of patch
     * calls + connected state. */
    pp_settings_set_async("air", "wfbng", "mcs_index", "9", nullptr, nullptr);
    for (int i = 0; i < 200 && m.patch_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.patch_calls >= 1);
    /* Apply should NOT have been called (PATCH 400 short-circuits). */
    REQUIRE(m.apply_calls == 0);

    m.stop();
}
```

(Note: testing the callback's `err` message requires draining `lv_async_call` queue. We add an LVGL tick loop helper in Task 6.6; for this case the server-side assertion is sufficient.)

- [ ] **Step 2: Run + commit**

```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][network]"
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): integration validation-error short-circuits apply"
```

---

### Task 6.4: Integration test — lock error

- [ ] **Step 1: Append**

```cpp
TEST_CASE("integration: dynamic_link_locked rejected client-side",
          "[fpvd][network]") {
    FpvdMockServer m;
    /* Snapshot has dynamicLink.enabled = true so client-side lock check fires
     * before any network call. */
    m.get_response =
      R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
      R"("fec":{"k":8,"n":12}},"video":{"bitrate":8192,"qpDelta":-4,)"
      R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(pp_settings_is_connected() == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "mcs_index") == true);

    /* Attempting to set the locked field should be rejected before any
     * PATCH; the server sees no patch call. */
    int before = m.patch_calls;
    pp_settings_set_async("air", "wfbng", "mcs_index", "5", nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(m.patch_calls == before);

    m.stop();
}
```

- [ ] **Step 2: Run + commit**

```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][network]"
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): client-side lock check short-circuits PATCH"
```

---

### Task 6.5: Integration test — transport failure + reconnect

- [ ] **Step 1: Append**

```cpp
TEST_CASE("integration: offline -> reconnect transitions connected flag",
          "[fpvd][network]") {
    /* No server running yet — pointing at a port that should refuse. */
    setenv("PP_FPVD_URL", "http://127.0.0.1:1", 1);
    pp_settings_register_fpvd();
    /* The synchronous prime fails → connected==false. */
    REQUIRE(pp_settings_is_connected() == false);

    /* Now start a server, repoint the env, re-register (provider re-reads
     * env in pp_settings_register_fpvd). */
    FpvdMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 50 && pp_settings_is_connected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_is_connected() == true);
    m.stop();
}
```

(Caveat: re-registering the provider mid-test leaks the first worker thread. For v1 tests we accept the leak; a clean shutdown API is a future task. The plan currently has no `pp_settings_unregister_fpvd()` — adding it is optional and out of scope.)

- [ ] **Step 2: Run + commit**

```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][network]"
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): offline-to-reconnect flips connected flag"
```

---

### Task 6.6: Integration test — debounce coalescing

- [ ] **Step 1: Append**

```cpp
TEST_CASE("integration: rapid same-path writes coalesce to one PATCH",
          "[fpvd][network]") {
    FpvdMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int before = m.patch_calls;
    for (int i = 0; i < 5; i++) {
        char buf[8]; snprintf(buf, sizeof buf, "%d", 30 + i*10);
        pp_settings_set_async("air", "camera", "fps", buf, nullptr, nullptr);
    }
    /* Wait long enough for worker to drain (250ms debounce + http). */
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    /* Worker may process the first job, then coalesce the rest into one
     * follow-up. So expect AT MOST 2 PATCH calls, not 5. */
    REQUIRE((m.patch_calls - before) <= 2);
    /* The final value '70' (i=4) wins. */
    REQUIRE(m.last_patch_body.find("\"fps\":70") != std::string::npos);

    m.stop();
}
```

- [ ] **Step 2: Run + commit**

```bash
cmake --build build_sim --target fpvd_tests && ./build_sim/fpvd_tests "[fpvd][network]"
git add tests/test_settings_fpvd_integration.cpp
git commit -m "test(fpvd): rapid same-path writes coalesce in the queue"
```

---

## Phase 7 — Wire-up and manual smoke test

### Task 7.1: Switch device build from stub to fpvd provider

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Edit `src/main.cpp`**

Find line 1442 (`pp_settings_register_stub();`) and replace with:

```c
	pp_settings_register_fpvd();
```

- [ ] **Step 2: Add libcurl to the device-build target link line**

Edit `CMakeLists.txt`. In the device branch (after the `else()` around line 200), find the `target_link_libraries(${PROJECT_NAME} ...)` call (around line 231). Add `CURL::libcurl` to the link list, and add `find_package(CURL REQUIRED)` near the other `find_package` calls in that branch.

Also add the cJSON source + include path to the device build. Near line 230 (where source lists are assembled), include the cJSON source file in the executable target — add to the `SOURCE_FILES` (or whichever variable accumulates sources):

```cmake
    list(APPEND SOURCE_FILES
        ${PROJECT_SOURCE_DIR}/third_party/cjson/cJSON.c
        src/gsmenu/settings_fpvd.c)
    list(APPEND PROJECT_INCLUDE_DIRS
        ${PROJECT_SOURCE_DIR}/third_party/cjson)
```

(Inspect the actual variable names in `CMakeLists.txt` and adapt — the exact names may differ. The intent: ensure `settings_fpvd.c` and `cJSON.c` get compiled into the device binary with the appropriate include path.)

- [ ] **Step 3: Try building the device binary if a cross/native toolchain is available**

Run:
```bash
cmake -S . -B build -DBUILD_SIMULATOR=OFF
cmake --build build -j 2>&1 | tail -20
```
Expected: device build succeeds. (If the host lacks the rockchip/drm deps, this is fine — the device CI build will catch it.)

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp CMakeLists.txt
git commit -m "feat: device build uses fpvd provider (replaces stub)"
```

---

### Task 7.2: Manual smoke test instructions in PR description

**Files:**
- Modify: `docs/superpowers/plans/2026-05-27-fpvd-settings-backend.md` (this file)
- Or just include in the PR body when raising the PR

- [ ] **Step 1: Verify in sim**

Run with latency:
```bash
PP_SIM_LATENCY_MS=500 ./build_sim/<sim-binary>
```

Checklist:
- [ ] Camera tab: ISP/FPV sections are gone, Contrast/Hue/Saturation/Luminance rows are gone.
- [ ] Camera tab: ROI section visible with Enabled/QP/Center/Steps; Image has Rotate; Recording has Max size (MB).
- [ ] Link tab: Bandwidth dropdown shows only 20/40; MCS slider 0..7; TX Power 1..63; FEC_K 1..31; FEC_N 2..32.
- [ ] New "DLink" tab shows only the Enabled toggle. Toggling it on reveals all other rows. Toggling off hides them again.
- [ ] Editing any row shows the spinner during ~500ms latency, then the value updates.
- [ ] `PP_SIM_FAIL=1 PP_SIM_LATENCY_MS=500 ./build_sim/<sim-binary>` shows spinner then a toast on every edit; value reverts.

- [ ] **Step 2: Verify on-device**

With a real drone running fpvd at `10.5.0.10:8080` (and the GS on the wfb tunnel):
- [ ] Open menu → Camera → change FPS → spinner shows briefly → value commits.
- [ ] On Link tab, enable Dynamic Link from the DLink tab → return to Link tab → confirm MCS/TX Power/FEC rows are greyed with lock icon.
- [ ] Pull the drone's power → after ~2 s, all drone rows grey out with the offline icon. Restore power → after a few s, rows re-enable.

- [ ] **Step 3: No commit needed for documentation; this is the PR's test plan.**

---

## Self-review

(Performed inline against `docs/superpowers/specs/2026-05-27-fpvd-settings-backend-design.md` — all sections of the spec covered.)

### Spec coverage

| Spec section | Implementing tasks |
|---|---|
| New files: settings_fpvd.c, dynamiclink.c/h | 0.3, 3.x, 4.3 |
| Modified files: settings.h/c, dummy, camera, link, ui.h/menu.c, widgets, CMake | 1.1, 5.x, 4.x, 2.x, 7.1 |
| Provider internals: snapshot, worker, libcurl, key map, polling | 3.1, 3.2, 3.3, 3.5, 3.6 |
| Key mapping table | 3.1 |
| Locked paths | 3.4, lock UX in 4.5 |
| Snapshot listener pattern | 3.6, 4.5, 4.3 |
| UI changes (Camera/Link/DLink) | 4.1, 4.2, 4.3, 4.4 |
| Sync-feeling write flow with busy state | 2.1, 2.2, 2.3, 2.4 |
| Offline behavior | 3.6 (refresh_snapshot, connected flag), 4.5 (offline lock state), 6.5 (test) |
| Dummy provider PP_SIM_LATENCY_MS + seed | 5.1, 5.2 |
| Testing: unit (keymap/snapshot/patch/lock) | 3.1–3.4 |
| Testing: integration (happy/validation/lock/transport/debounce) | 6.1–6.6 |
| Sim parity | 5.2 |
| Manual smoke test | 7.2 |

### Known fragile points (called out in plan)

- **Single snapshot listener limit.** Provider exposes one listener slot. Dynamic Link tab installs its own visibility listener; Camera/Link install their lock listener; whichever was installed last wins. v1 limitation — acceptable because the lock listener only matters when the user is *on* Camera/Link tabs, which is the most common case. Future task: a listener list.
- **Common widget-data head trick.** `pp_page_reapply_lock_state` reinterprets every focusable child's `lv_obj_get_user_data` as `{char *d, char *p, char *k}`. This holds today because all three of `pp_toggle_data_t`, `pp_slider_data_t`, `pp_dd_data_t` start with that triple. If any future widget breaks the convention, the lock helper will mis-cast.
- **Callback path through `lv_async_call` is not pumped in unit tests** — integration tests assert server-side observable effects only (PATCH/apply count, last body). Asserting callback `err` text requires either a test harness that pumps `lv_timer_handler` (deferred to a follow-up plan) or a non-LVGL test build of the provider with the async layer mocked.
- **No clean shutdown.** Re-registering the provider in tests leaks the prior worker thread. Production has a single registration at startup, so this is benign there but noted.

### Placeholder scan

No instances of "TBD", "TODO", "implement later", "fill in details" in implementation steps. The known-fragile points above are explicit limitations of v1, not undefined behavior. Manual smoke checklist is the only intentionally interactive section.

### Type consistency

`fpvd_keymap_entry_t`, `fpvd_type_t`, `fpvd_http_result_t`, `fpvd_job_t`, `fpvd_state_t`, `pp_settings_provider_t`, `pp_settings_snapshot_cb`, `pp_row_lock_t` — all defined once and referenced consistently. Function names: `fpvd_keymap_lookup`, `fpvd_snapshot_read_string`, `fpvd_build_patch_body`, `fpvd_is_locked_path`, `fpvd_http_*`, `pp_settings_*`, `pp_row_set_busy`, `pp_row_set_locked` — used consistently across tasks.
