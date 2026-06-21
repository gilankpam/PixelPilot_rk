# Drone Connection Event Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make fpvd's drone connection state (`/gs/status.connection`) the single source of truth for the GS menu's drone reachability, delivered via a dedicated ~1 s poll and a process-wide publish/subscribe service.

**Architecture:** A new standalone C module `conn_state` runs its own thread polling `GET {base_url}/gs/status` every ~1 s, parses `.connection.state`, and notifies subscribers on change. `settings_fpvd.c` becomes its first subscriber: it deletes its `/air`-probe-derived `G.drone_reachable` and sources both menu row-locking and apply-routing from `conn_state_get()`. No fpvd changes.

**Tech Stack:** C (C-ABI header for C/C++ interop), libcurl, cJSON, pthreads. Catch2 v2 host unit tests under the `USE_SIMULATOR` CMake branch.

## Global Constraints

- **Single source of truth:** drone reachability comes from `conn_state` only. After this change no code derives drone reachability from the `/air` round-trip.
- **No fallback:** `conn_state` states `ARMED`, `DISCONNECTED`, and `UNKNOWN` all map to "not reachable". Only `CONNECTED` is reachable. (Per design: monitor is default-on; the ~1 s startup `UNKNOWN` window self-clears.)
- **GS-local only:** never compare timestamps across the GS/drone clock boundary. `conn_state.updated_ms` uses `CLOCK_MONOTONIC` (GS-local); `sinceMs` is passed through verbatim from fpvd, not interpreted.
- **Host build/test:** always wrap in `nix-shell --run '...'` (PixelPilot_rk `shell.nix`, gcc + Catch2 v2). Test targets build under `-DUSE_SIMULATOR=ON` (NOT `BUILD_TESTS`). Catch2 v2: `#include <catch2/catch.hpp>`, link `Catch2::Catch2WithMain`. Keep the build dir on `CMAKE_BUILD_TYPE=Release` (Debug appends ASAN C_FLAGS but not link opts → C-source test exes fail to link).
- **`main.cpp` / `osd.cpp` do NOT compile in the host env** (rockchip/drm/gst). The `conn_state` module and `settings_fpvd.c` changes ARE host-testable via `conn_state_tests` and `fpvd_tests`; the `main.cpp` wiring is verified only by the aarch64 GS cross-build / on-device.
- **fpvd `.connection` shape:** `{ "enabled":bool, "state":"disconnected"|"armed"|"connected", "reason":str, "sinceMs":int|null }`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/conn_state.h` (new) | C-ABI: enum, snapshot struct, `parse`, `start`/`stop`, `get`, `subscribe`/`unsubscribe`, `ingest`. |
| `src/conn_state.c` (new) | Static state + mutex, subscriber array, pure parser, poller thread. |
| `tests/test_conn_state.cpp` (new) | Catch2 unit tests for `parse` + pub/sub. |
| `src/gsmenu/settings_fpvd.c` (modify) | Delete `G.drone_reachable`; source reachability from `conn_state`; start+subscribe in register. |
| `tests/test_settings_fpvd_integration.cpp` (modify) | Drive `conn_state` via `ingest`; rewrite the 3 tests that encoded `/air`-sourced reachability. |
| `src/main.cpp` (modify) | `conn_state_stop()` at shutdown. |
| `CMakeLists.txt` (modify) | Add `src/conn_state.c` to device build + `fpvd_tests`; add `conn_state_tests` target. |

---

## Task 1: `conn_state` parse + snapshot storage

**Files:**
- Create: `src/conn_state.h`
- Create: `src/conn_state.c`
- Test: `tests/test_conn_state.cpp`
- Modify: `CMakeLists.txt` (new `conn_state_tests` target, after the `runtime_cfg_tests` block ~line 287)

**Interfaces:**
- Produces: `conn_state_e`, `conn_state_t`, `bool conn_state_parse(const char *json, conn_state_t *out)`, `conn_state_t conn_state_get(void)`.

- [ ] **Step 1: Write `src/conn_state.h`** (full public API — later tasks implement the rest)

```c
#ifndef PP_CONN_STATE_H
#define PP_CONN_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONN_UNKNOWN = 0,     /* no data yet / fpvd or GS unreachable / .connection absent */
    CONN_DISCONNECTED,
    CONN_ARMED,
    CONN_CONNECTED,
} conn_state_e;

typedef struct {
    conn_state_e state;
    char         reason[64];   /* fpvd .reason; "" if none */
    long         since_ms;     /* .sinceMs, -1 if absent/null */
    uint64_t     updated_ms;   /* CLOCK_MONOTONIC ms of last apply */
} conn_state_t;

typedef void (*conn_state_cb)(const conn_state_t *st, void *ud);

/* Pure: parse a /gs/status body. On a recognized .connection.state, fills *out
 * and returns true; otherwise sets *out to {CONN_UNKNOWN,"",-1,0} and returns
 * false. Never reads the clock (updated_ms left 0). */
bool conn_state_parse(const char *json, conn_state_t *out);

/* Poller lifecycle. start() is idempotent; stop() also resets state +
 * subscribers (safe to call when never started — used by tests). */
void conn_state_start(const char *base_url, int interval_ms);
void conn_state_stop(void);

conn_state_t conn_state_get(void);

/* Feed a state from any source (poller, simulator, tests). Notifies
 * subscribers iff the enum state changed. */
void conn_state_ingest(conn_state_e state, const char *reason, long since_ms);

/* Returns a token >= 0, or -1 if full/invalid. Delivers the current snapshot
 * to cb once, synchronously, before returning. */
int  conn_state_subscribe(conn_state_cb cb, void *ud);
void conn_state_unsubscribe(int token);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the failing test** `tests/test_conn_state.cpp`

```cpp
#include <catch2/catch.hpp>
#include <cstring>
#include "conn_state.h"

TEST_CASE("parse: connected with reason and sinceMs", "[conn_state]") {
    conn_state_t s;
    const char *j = R"({"connection":{"enabled":true,"state":"connected","reason":"","sinceMs":1234}})";
    REQUIRE(conn_state_parse(j, &s) == true);
    REQUIRE(s.state == CONN_CONNECTED);
    REQUIRE(s.since_ms == 1234);
}

TEST_CASE("parse: armed and disconnected map through", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"connection":{"state":"armed"}})", &s));
    REQUIRE(s.state == CONN_ARMED);
    REQUIRE(conn_state_parse(R"({"connection":{"state":"disconnected","reason":"timeout"}})", &s));
    REQUIRE(s.state == CONN_DISCONNECTED);
    REQUIRE(std::strcmp(s.reason, "timeout") == 0);
}

TEST_CASE("parse: missing connection block -> UNKNOWN, false", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"runner":{"running":true}})", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
    REQUIRE(s.since_ms == -1);
    REQUIRE(s.reason[0] == '\0');
}

TEST_CASE("parse: garbage / unknown state -> UNKNOWN, false", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse("not json", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
    REQUIRE(conn_state_parse(R"({"connection":{"state":"bogus"}})", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
}

TEST_CASE("parse: sinceMs null -> -1", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"connection":{"state":"connected","sinceMs":null}})", &s));
    REQUIRE(s.since_ms == -1);
}

TEST_CASE("get returns the last ingested snapshot", "[conn_state]") {
    conn_state_stop();   /* reset */
    conn_state_ingest(CONN_CONNECTED, "", 7);
    REQUIRE(conn_state_get().state == CONN_CONNECTED);
    conn_state_stop();
}
```

- [ ] **Step 3: Write `src/conn_state.c`** (parse + storage + get + ingest minimal; pub/sub & poller filled in Tasks 2–3)

```c
#include "conn_state.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "cJSON.h"

#define CONN_MAX_SUBS 8

typedef struct { conn_state_cb cb; void *ud; bool used; } conn_sub_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    conn_state_t    cur;
    conn_sub_t      subs[CONN_MAX_SUBS];
    pthread_t       thread;
    bool            started;
    bool            stop;
    char            base_url[128];
    int             interval_ms;
} C = {
    .mu  = PTHREAD_MUTEX_INITIALIZER,
    .cv  = PTHREAD_COND_INITIALIZER,
    .cur = { CONN_UNKNOWN, "", -1, 0 },
};

static uint64_t conn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

bool conn_state_parse(const char *json, conn_state_t *out) {
    out->state = CONN_UNKNOWN;
    out->reason[0] = '\0';
    out->since_ms = -1;
    out->updated_ms = 0;
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *conn = cJSON_GetObjectItemCaseSensitive(root, "connection");
    cJSON *st   = conn ? cJSON_GetObjectItemCaseSensitive(conn, "state") : NULL;
    bool ok = false;
    if (st && cJSON_IsString(st) && st->valuestring) {
        const char *s = st->valuestring;
        if      (strcmp(s, "connected")    == 0) { out->state = CONN_CONNECTED;    ok = true; }
        else if (strcmp(s, "armed")        == 0) { out->state = CONN_ARMED;        ok = true; }
        else if (strcmp(s, "disconnected") == 0) { out->state = CONN_DISCONNECTED; ok = true; }
        if (ok) {
            cJSON *rs = cJSON_GetObjectItemCaseSensitive(conn, "reason");
            if (rs && cJSON_IsString(rs) && rs->valuestring) {
                strncpy(out->reason, rs->valuestring, sizeof out->reason - 1);
                out->reason[sizeof out->reason - 1] = '\0';
            }
            cJSON *sm = cJSON_GetObjectItemCaseSensitive(conn, "sinceMs");
            if (sm && cJSON_IsNumber(sm)) out->since_ms = (long)sm->valuedouble;
        }
    }
    cJSON_Delete(root);
    return ok;
}

conn_state_t conn_state_get(void) {
    pthread_mutex_lock(&C.mu);
    conn_state_t c = C.cur;
    pthread_mutex_unlock(&C.mu);
    return c;
}

/* Filled in Task 2. */
void conn_state_ingest(conn_state_e state, const char *reason, long since_ms) {
    pthread_mutex_lock(&C.mu);
    C.cur.state = state;
    C.cur.reason[0] = '\0';
    if (reason) { strncpy(C.cur.reason, reason, sizeof C.cur.reason - 1);
                  C.cur.reason[sizeof C.cur.reason - 1] = '\0'; }
    C.cur.since_ms = since_ms;
    C.cur.updated_ms = conn_now_ms();
    pthread_mutex_unlock(&C.mu);
}

int  conn_state_subscribe(conn_state_cb cb, void *ud) { (void)cb; (void)ud; return -1; }
void conn_state_unsubscribe(int token) { (void)token; }

void conn_state_start(const char *base_url, int interval_ms) { (void)base_url; (void)interval_ms; }
void conn_state_stop(void) {
    pthread_mutex_lock(&C.mu);
    C.cur.state = CONN_UNKNOWN; C.cur.reason[0] = '\0'; C.cur.since_ms = -1; C.cur.updated_ms = 0;
    pthread_mutex_unlock(&C.mu);
}
```

- [ ] **Step 4: Add the `conn_state_tests` target** in `CMakeLists.txt` after the `runtime_cfg_tests` block (~line 287, inside `if(Catch2_FOUND)`)

```cmake
    add_executable(conn_state_tests
      src/conn_state.c
      ${PP_CJSON_SOURCES}
      tests/test_conn_state.cpp)
    target_include_directories(conn_state_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC})
    target_link_libraries(conn_state_tests
      Catch2::Catch2WithMain CURL::libcurl pthread)
```

- [ ] **Step 5: Build + run; verify pass**

Run: `nix-shell --run 'cmake build-test && cmake --build build-test --target conn_state_tests -j 2>&1 | tail -20'`
Then: `nix-shell --run './build-test/conn_state_tests'`
Expected: all `[conn_state]` assertions PASS.

- [ ] **Step 6: Commit**

```bash
git add src/conn_state.h src/conn_state.c tests/test_conn_state.cpp CMakeLists.txt
git commit -m "feat(conn_state): pure parser + snapshot storage for /gs/status.connection"
```

---

## Task 2: `conn_state` publish/subscribe

**Files:**
- Modify: `src/conn_state.c` (implement `ingest`/`subscribe`/`unsubscribe` + internal apply)
- Test: `tests/test_conn_state.cpp` (add pub/sub cases)

**Interfaces:**
- Consumes: Task 1 types + `conn_state_get`.
- Produces: working `conn_state_subscribe`, `conn_state_unsubscribe`, `conn_state_ingest` (notify-on-change, immediate delivery on subscribe).

- [ ] **Step 1: Add failing pub/sub tests** to `tests/test_conn_state.cpp`

```cpp
namespace {
struct Counter { int calls = 0; conn_state_e last = CONN_UNKNOWN; };
void count_cb(const conn_state_t *st, void *ud) {
    auto *c = static_cast<Counter *>(ud);
    c->calls++; c->last = st->state;
}
}

TEST_CASE("subscribe delivers current state immediately", "[conn_state]") {
    conn_state_stop();
    conn_state_ingest(CONN_CONNECTED, "", -1);
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);
    REQUIRE(tok >= 0);
    REQUIRE(c.calls == 1);
    REQUIRE(c.last == CONN_CONNECTED);
    conn_state_unsubscribe(tok);
    conn_state_stop();
}

TEST_CASE("ingest notifies only on state change", "[conn_state]") {
    conn_state_stop();
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);   /* +1 immediate (UNKNOWN) */
    conn_state_ingest(CONN_CONNECTED, "", -1);       /* change -> +1 */
    conn_state_ingest(CONN_CONNECTED, "still", 5);   /* same state -> no fire */
    conn_state_ingest(CONN_DISCONNECTED, "drop", -1);/* change -> +1 */
    REQUIRE(c.calls == 3);
    REQUIRE(c.last == CONN_DISCONNECTED);
    conn_state_unsubscribe(tok);
    conn_state_stop();
}

TEST_CASE("unsubscribe stops delivery", "[conn_state]") {
    conn_state_stop();
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);    /* +1 immediate */
    conn_state_unsubscribe(tok);
    conn_state_ingest(CONN_CONNECTED, "", -1);        /* no delivery */
    REQUIRE(c.calls == 1);
    conn_state_stop();
}
```

- [ ] **Step 2: Run; verify the new cases fail** (current stubs return -1 / never notify)

Run: `nix-shell --run 'cmake --build build-test --target conn_state_tests -j && ./build-test/conn_state_tests "[conn_state]"'`
Expected: FAIL on the three new cases.

- [ ] **Step 3: Implement pub/sub** — replace the three stubbed functions and the placeholder `ingest` in `src/conn_state.c` with:

```c
/* Apply a new snapshot; notify subscribers iff the enum state changed.
 * Subscribers are snapshotted under the lock then invoked unlocked. */
static void conn_state_apply(const conn_state_t *ns) {
    conn_sub_t snap[CONN_MAX_SUBS];
    int n = 0;
    bool changed;
    conn_state_t cur_copy;
    pthread_mutex_lock(&C.mu);
    changed = (ns->state != C.cur.state);
    C.cur = *ns;
    cur_copy = C.cur;
    if (changed)
        for (int i = 0; i < CONN_MAX_SUBS; i++)
            if (C.subs[i].used) snap[n++] = C.subs[i];
    pthread_mutex_unlock(&C.mu);
    for (int i = 0; i < n; i++) snap[i].cb(&cur_copy, snap[i].ud);
}

void conn_state_ingest(conn_state_e state, const char *reason, long since_ms) {
    conn_state_t ns;
    ns.state = state;
    ns.reason[0] = '\0';
    if (reason) { strncpy(ns.reason, reason, sizeof ns.reason - 1);
                  ns.reason[sizeof ns.reason - 1] = '\0'; }
    ns.since_ms = since_ms;
    ns.updated_ms = conn_now_ms();
    conn_state_apply(&ns);
}

int conn_state_subscribe(conn_state_cb cb, void *ud) {
    if (!cb) return -1;
    int token = -1;
    conn_state_t cur_copy;
    pthread_mutex_lock(&C.mu);
    for (int i = 0; i < CONN_MAX_SUBS; i++) {
        if (!C.subs[i].used) {
            C.subs[i].cb = cb; C.subs[i].ud = ud; C.subs[i].used = true;
            token = i; break;
        }
    }
    cur_copy = C.cur;
    pthread_mutex_unlock(&C.mu);
    if (token >= 0) cb(&cur_copy, ud);   /* immediate current-state delivery */
    return token;
}

void conn_state_unsubscribe(int token) {
    if (token < 0 || token >= CONN_MAX_SUBS) return;
    pthread_mutex_lock(&C.mu);
    C.subs[token].used = false;
    C.subs[token].cb = NULL;
    C.subs[token].ud = NULL;
    pthread_mutex_unlock(&C.mu);
}
```

Also update `conn_state_stop()` to clear subscribers (replace the Task-1 body):

```c
void conn_state_stop(void) {
    pthread_mutex_lock(&C.mu);
    /* Task 3 adds: stop+join the poller thread here when started. */
    for (int i = 0; i < CONN_MAX_SUBS; i++) {
        C.subs[i].used = false; C.subs[i].cb = NULL; C.subs[i].ud = NULL;
    }
    C.cur.state = CONN_UNKNOWN; C.cur.reason[0] = '\0'; C.cur.since_ms = -1; C.cur.updated_ms = 0;
    pthread_mutex_unlock(&C.mu);
}
```

- [ ] **Step 4: Run; verify pass**

Run: `nix-shell --run 'cmake --build build-test --target conn_state_tests -j && ./build-test/conn_state_tests'`
Expected: all `[conn_state]` PASS.

- [ ] **Step 5: Commit**

```bash
git add src/conn_state.c tests/test_conn_state.cpp
git commit -m "feat(conn_state): notify-on-change pub/sub with immediate delivery"
```

---

## Task 3: `conn_state` poller thread

**Files:**
- Modify: `src/conn_state.c` (implement `conn_state_start` + curl GET loop; finish `conn_state_stop` join)

**Interfaces:**
- Consumes: Task 2 `conn_state_apply` (internal), `conn_state_parse`.
- Produces: working `conn_state_start(base_url, interval_ms)` / `conn_state_stop()`.

No host unit test (requires a live HTTP server; the parse + pub/sub paths it drives are already covered). Verified by compile + on-device (Task 6).

- [ ] **Step 1: Add curl + the poller** to `src/conn_state.c`. Add includes near the top:

```c
#include <curl/curl.h>
```

Add before `conn_state_start`:

```c
typedef struct { char *body; size_t len; } conn_buf_t;

static size_t conn_write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    conn_buf_t *b = (conn_buf_t *)ud;
    size_t add = sz * nm;
    char *nb = realloc(b->body, b->len + add + 1);
    if (!nb) return 0;
    b->body = nb;
    memcpy(b->body + b->len, ptr, add);
    b->len += add;
    b->body[b->len] = '\0';
    return add;
}

static void conn_curl_init_once(void) {
    static int done = 0;
    if (!done) { curl_global_init(CURL_GLOBAL_DEFAULT); done = 1; }
}

static void conn_poll_once(void) {
    char url[160];
    snprintf(url, sizeof url, "%s/gs/status", C.base_url);
    conn_buf_t buf = { NULL, 0 };
    long code = 0;
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 800L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 1500L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, conn_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        if (curl_easy_perform(c) == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
    }
    conn_state_t ns;
    if (!(code == 200 && buf.body && conn_state_parse(buf.body, &ns))) {
        ns.state = CONN_UNKNOWN; ns.reason[0] = '\0'; ns.since_ms = -1;
    }
    ns.updated_ms = conn_now_ms();
    free(buf.body);
    conn_state_apply(&ns);
}

static void *conn_poll_main(void *arg) {
    (void)arg;
    for (;;) {
        conn_poll_once();
        pthread_mutex_lock(&C.mu);
        if (C.stop) { pthread_mutex_unlock(&C.mu); break; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);   /* default cond clock */
        ts.tv_sec  += C.interval_ms / 1000;
        ts.tv_nsec += (C.interval_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&C.cv, &C.mu, &ts);
        bool stop = C.stop;
        pthread_mutex_unlock(&C.mu);
        if (stop) break;
    }
    return NULL;
}
```

- [ ] **Step 2: Implement `conn_state_start`** — replace the stub:

```c
void conn_state_start(const char *base_url, int interval_ms) {
    pthread_mutex_lock(&C.mu);
    if (C.started) { pthread_mutex_unlock(&C.mu); return; }
    strncpy(C.base_url, base_url && *base_url ? base_url : "http://127.0.0.1:8080",
            sizeof C.base_url - 1);
    C.base_url[sizeof C.base_url - 1] = '\0';
    C.interval_ms = interval_ms > 0 ? interval_ms : 1000;
    C.stop = false;
    C.started = true;
    pthread_mutex_unlock(&C.mu);
    conn_curl_init_once();
    pthread_create(&C.thread, NULL, conn_poll_main, NULL);
}
```

- [ ] **Step 3: Finish `conn_state_stop`** — make it join the thread when running (replace the Task-2 body):

```c
void conn_state_stop(void) {
    pthread_mutex_lock(&C.mu);
    bool was_started = C.started;
    if (was_started) { C.stop = true; pthread_cond_signal(&C.cv); }
    pthread_mutex_unlock(&C.mu);
    if (was_started) {
        pthread_join(C.thread, NULL);
        pthread_mutex_lock(&C.mu);
        C.started = false;
        pthread_mutex_unlock(&C.mu);
    }
    pthread_mutex_lock(&C.mu);
    for (int i = 0; i < CONN_MAX_SUBS; i++) {
        C.subs[i].used = false; C.subs[i].cb = NULL; C.subs[i].ud = NULL;
    }
    C.cur.state = CONN_UNKNOWN; C.cur.reason[0] = '\0'; C.cur.since_ms = -1; C.cur.updated_ms = 0;
    pthread_mutex_unlock(&C.mu);
}
```

- [ ] **Step 4: Build conn_state_tests; verify still green** (poller code compiles; existing tests unaffected — they never call `start`)

Run: `nix-shell --run 'cmake --build build-test --target conn_state_tests -j && ./build-test/conn_state_tests'`
Expected: all `[conn_state]` PASS.

- [ ] **Step 5: Commit**

```bash
git add src/conn_state.c
git commit -m "feat(conn_state): 1s /gs/status poller thread with clean shutdown"
```

---

## Task 4: Source GS-menu reachability from `conn_state`

This task lands the `settings_fpvd.c` rewire and its `fpvd_tests` updates together so the suite ends green. **Reverses a prior deliberate choice** (tests `372`/`392` asserted reachability comes from `/air`, not `/gs/status`) — justified because fpvd's `ConnectionMonitor` is a new authoritative signal that postdates those tests.

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c`
- Modify: `tests/test_settings_fpvd_integration.cpp`
- Modify: `CMakeLists.txt` (`fpvd_tests` target: add `src/conn_state.c`)

**Interfaces:**
- Consumes: `conn_state_get`, `conn_state_ingest`, `conn_state_start`, `conn_state_subscribe`, `CONN_CONNECTED`/`CONN_DISCONNECTED`.

- [ ] **Step 1: Add `src/conn_state.c` to the `fpvd_tests` target** in `CMakeLists.txt` (the `add_executable(fpvd_tests ...)` list ~line 252), after `src/gsmenu/settings_runtime_cfg.c`:

```cmake
      src/gsmenu/settings_runtime_cfg.c
      src/conn_state.c
```

(The target already links `CURL::libcurl pthread` and has `${PROJECT_SOURCE_DIR}/src` on the include path — no other target changes needed.)

- [ ] **Step 2: Edit `src/gsmenu/settings_fpvd.c` — include + delete the field + add the change callback.**

Add the include after the existing `#include "settings_runtime_cfg.h"` (top of file):
```c
#include "../conn_state.h"
```

In `struct fpvd_state_t` (the `typedef struct { ... } fpvd_state_t;`), **delete** the line:
```c
    bool     drone_reachable;     /* derived from the /air round-trip (2xx=reachable) */
```

After `listener_dispatch_async` (just below `notify_listener`), add:
```c
#ifndef PP_FPVD_TEST
/* conn_state pushes drone-link transitions; re-lock the menu rows on change. */
static void on_conn_state_change(const conn_state_t *st, void *ud) {
    (void)st; (void)ud;
    notify_listener();
}
#endif
```

- [ ] **Step 3: Edit `refresh_snapshot_unlocked`** — stop deriving reachability from `/air` (keep fetching `air_snapshot`).

Replace:
```c
    bool was_gs = G.gs_connected, was_drone = G.drone_reachable;
```
with:
```c
    bool was_gs = G.gs_connected;
```

Replace the `/air` reachability block:
```c
    if (ar.status >= 200 && ar.status < 300) {
        if (ar.body) {
            cJSON *a = cJSON_Parse(ar.body);
            if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
        }
        G.drone_reachable = true;
    } else {
        G.drone_reachable = false;
    }
```
with:
```c
    /* /air/config is fetched only to refresh air_snapshot (the drone's config
     * values shown in the menu). Drone reachability is no longer derived here —
     * it comes from conn_state (fpvd's /gs/status.connection). */
    if (ar.status >= 200 && ar.status < 300 && ar.body) {
        cJSON *a = cJSON_Parse(ar.body);
        if (a) { if (G.air_snapshot) cJSON_Delete(G.air_snapshot); G.air_snapshot = a; }
    }
```

Replace the return:
```c
    return was_gs != G.gs_connected || was_drone != G.drone_reachable || G.gs_connected;
```
with:
```c
    return was_gs != G.gs_connected || G.gs_connected;
```

- [ ] **Step 4: Edit `run_job_unlocked`** — source apply-routing reachability from conn_state.

Replace:
```c
        pthread_mutex_lock(&G.mu);
        bool reachable = G.drone_reachable;
        char mac[24] = {0};
```
with:
```c
        bool reachable = (conn_state_get().state == CONN_CONNECTED);
        char mac[24] = {0};
        pthread_mutex_lock(&G.mu);
```
(The `cJSON *bf = ...` / `localMac` read and its `pthread_mutex_unlock(&G.mu);` stay as-is, now immediately after the lock.)

- [ ] **Step 5: Edit the 502 error path** — remove the now-deleted field write. Replace:
```c
            } else {
                m = parse_error_message(r.body);
                /* A 502 only signals "drone unreachable" when it came from the
                 * /air/* proxy; /gs/* 502s are GS-side errors. */
                if (r.status == 502 && !steps[i].gs_side) {
                    pthread_mutex_lock(&G.mu);
                    bool was = G.drone_reachable; G.drone_reachable = false;
                    pthread_mutex_unlock(&G.mu);
                    if (was) notify_listener();
                    if (!m) m = "Drone unreachable";
                }
            }
```
with:
```c
            } else {
                m = parse_error_message(r.body);
                /* A 502 from the /air/* proxy still surfaces a message; drone
                 * reachability is owned by conn_state, not by apply outcomes. */
                if (r.status == 502 && !steps[i].gs_side && !m) m = "Drone unreachable";
            }
```

- [ ] **Step 6: Edit `prov_is_reachable`** — keep the keymap gating, source the bool from conn_state. Replace:
```c
    if (e->endpoint != FPVD_EP_AIR && e->kind != FPVD_ROW_BEAMFORM) return true;
    pthread_mutex_lock(&G.mu);
    bool r = G.drone_reachable;
    pthread_mutex_unlock(&G.mu);
    return r;
```
with:
```c
    if (e->endpoint != FPVD_EP_AIR && e->kind != FPVD_ROW_BEAMFORM) return true;
    return conn_state_get().state == CONN_CONNECTED;
```

- [ ] **Step 7: Edit `pp_settings_register_fpvd`** — drop the field init, start + subscribe conn_state.

Delete the line:
```c
    G.drone_reachable = false;
```

Replace the worker-start block:
```c
    if (!G.worker_started) {
        pthread_create(&G.worker, NULL, worker_main, NULL);
        G.worker_started = true;
    }
```
with:
```c
    if (!G.worker_started) {
        pthread_create(&G.worker, NULL, worker_main, NULL);
#ifndef PP_FPVD_TEST
        conn_state_start(G.base_url, 1000);
        conn_state_subscribe(on_conn_state_change, NULL);
#endif
        G.worker_started = true;
    }
```

- [ ] **Step 8: Update `tests/test_settings_fpvd_integration.cpp`.**

Add the include after the existing includes:
```cpp
#include "conn_state.h"
```

Make `install_provider_pointing_at` establish a connected baseline (tests run with the poller disabled, so state is test-driven):
```cpp
static void install_provider_pointing_at(int port) {
    ensure_lv_init();
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
    conn_state_ingest(CONN_CONNECTED, "", -1);   /* default: drone link up */
}
```

In each of the four drone-down tests, add a disconnect ingest immediately after `install_provider_pointing_at(...)`:
- `"shared change degrades to GS-only when drone down"` (~line 236)
- `"beamforming rejected while drone down"` (~line 324)
- `"dynamicLink toggle rejected while drone down"` (~line 358)
- `"drone reachability gates air rows, not GS rows"` (~line 485)

```cpp
    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);
```

- [ ] **Step 9: Rewrite the two `/air`-sourced-reachability tests.** Replace the whole `"drone reachability comes only from /air, not /gs/status"` test (~372–390) **and** the `"/air 2xx marks the drone reachable (no status field)"` test (~392–398) with a single test proving the source is now conn_state:

```cpp
TEST_CASE("integration: reachability follows conn_state link, not /air", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();  /* /air would report DOWN */
    install_provider_pointing_at(srv.port);               /* ingests CONNECTED */

    /* conn_state says connected even though the /air probe is failing. */
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);

    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);  /* link drops */
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);
    /* GS-local rows stay reachable regardless of the drone. */
    REQUIRE(pp_settings_is_reachable("gs", "link", "rx_power") == true);
    srv.stop();
}
```

- [ ] **Step 10: Rewrite the visibility-refresh test** (~557 `"hidden->visible triggers an immediate refresh"`) to observe the refresh through a GS config value (reachability no longer flows from the refresh):

```cpp
TEST_CASE("integration: hidden->visible triggers an immediate refresh", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);
    char *c0 = pp_settings_get("gs", "wfbng", "gs_channel");
    REQUIRE(std::string(c0) == "132"); free(c0);

    /* GS config changes server-side; only a refresh will surface it. */
    srv.gs_config_response =
        R"({"link":{"channel":120,"width":40,"txPowerDbm":25,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";
    pp_settings_set_visibility(false);
    pp_settings_set_visibility(true);     /* menu opens -> immediate refresh */

    bool flipped = false;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!flipped && std::chrono::steady_clock::now() < end) {
        char *c = pp_settings_get("gs", "wfbng", "gs_channel");
        flipped = (std::string(c) == "120"); free(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(flipped == true);
    pp_settings_set_visibility(false);
    srv.stop();
}
```

- [ ] **Step 11: Build + run the full fpvd suite; verify green**

Run: `nix-shell --run 'cmake build-test && cmake --build build-test --target fpvd_tests conn_state_tests -j 2>&1 | tail -20'`
Then: `nix-shell --run './build-test/fpvd_tests'` and `nix-shell --run './build-test/conn_state_tests'`
Expected: all `[fpvd]` and `[conn_state]` cases PASS. (`[fpvd][network]` integration cases each take a few seconds.)

- [ ] **Step 12: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp CMakeLists.txt
git commit -m "feat(gsmenu): source drone reachability from conn_state, single-source"
```

---

## Task 5: Wire into the device build + shutdown

**Files:**
- Modify: `CMakeLists.txt` (device/`else()` source list ~line 428: add `src/conn_state.c`)
- Modify: `src/main.cpp` (`conn_state_stop()` at shutdown)

**Interfaces:**
- Consumes: `conn_state_stop` (start happens inside `pp_settings_register_fpvd`, already called at `main.cpp:1260`).

Not host-compilable (main.cpp is rockchip/drm/gst). Verified by the aarch64 GS cross-build + on-device (Task 6).

- [ ] **Step 1: Add `src/conn_state.c` to the device source list** in `CMakeLists.txt` (the `else()` branch, after `src/gsmenu/settings_fpvd.c` ~line 428):

```cmake
      src/gsmenu/settings_fpvd.c
      src/conn_state.c
```

- [ ] **Step 2: Include the header in `src/main.cpp`** (with the other project includes near the top):

```cpp
#include "conn_state.h"
```

- [ ] **Step 3: Stop the poller at shutdown** — in `src/main.cpp`, add before `gamma_lut_cleanup(&lut_ctrl);` (~line 1643, after the pthread_join block):

```cpp
	conn_state_stop();
```

- [ ] **Step 4: Cross-compile the device build** (per the GS deploy workflow — host cannot build main.cpp).

Run the project's aarch64 cross-build (nix stdin-only cross-build per the GS deploy notes).
Expected: links cleanly; `conn_state_*` symbols resolve.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.cpp
git commit -m "feat: start conn_state in the GS build; stop it on shutdown"
```

---

## Task 6: On-device verification

**Files:** none (manual verification on the GS, `root@10.18.0.1`).

- [ ] **Step 1: Deploy** the cross-built `pixelpilot` to the GS (per the GS deploy workflow) and launch it with the menu/OSD.

- [ ] **Step 2: Drone connected** — confirm the menu's AIR/beamforming rows are editable (not greyed) within ~1 s of the drone link coming up; GS-only rows always editable.

- [ ] **Step 3: Drone disconnected** — power-cycle / drop the drone link. Confirm AIR + beamforming + drone-TX rows grey out (⚠ OFFLINE) within ~1–2 s, while GS-only rows (channel/width/rx_power) stay editable. Confirm this happens with the menu already open (background reactivity), not only on menu-open.

- [ ] **Step 4: fpvd/GS down** — stop the fpvd daemon (`/etc/init.d/S99fpvd stop`); confirm drone-backed rows grey out (state → UNKNOWN → not reachable). Restart and confirm recovery.

- [ ] **Step 5: Apply while disconnected** — with the drone down, change a shared row (e.g. channel); confirm it applies GS-only and surfaces the expected message, matching the integration test contract.

---

## Self-Review

**Spec coverage:**
- A (source from `.connection`) → Tasks 1–4. ✓
- B-ii (dedicated ~1 s poll) → Task 3. ✓
- Global subscribe-able event → Tasks 1–2 (`subscribe`/`ingest`). ✓
- Delete `G.drone_reachable`, single-source both row-lock + apply-routing → Task 4 Steps 2/4/6. ✓
- `/air` demoted to air_snapshot fetch only → Task 4 Step 3. ✓
- No fallback (`UNKNOWN`→not reachable) → `prov_is_reachable` returns `== CONN_CONNECTED` (Step 6). ✓
- Tests: host parse + pub/sub (Tasks 1–2), integration rewire (Task 4), on-device (Task 6). ✓
- **Deviation from spec:** the spec floated wiring the simulator's `PP_SIM_DRONE_OFFLINE` to `conn_state`. Dropped — the simulator uses the `settings_dummy` provider, which never consults `conn_state`; wiring it would be incoherent. Coverage is via host unit tests + on-device instead.

**Placeholder scan:** none — all steps carry exact code/commands. (Cross-build and deploy commands defer to the established GS workflow rather than inventing flags.)

**Type consistency:** `conn_state_t`/`conn_state_e`/`conn_state_cb` and the function signatures are identical across `conn_state.h`, `conn_state.c`, `settings_fpvd.c`, and both test files. `prov_is_reachable` and `run_job_unlocked` both use `conn_state_get().state == CONN_CONNECTED`.
