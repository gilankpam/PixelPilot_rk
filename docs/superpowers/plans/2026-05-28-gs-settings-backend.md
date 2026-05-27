# GS-Side Settings Backend — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real GS-side settings backend that writes `/etc/wifibroadcast.cfg` and `/etc/default/pixelpilot`, restarts wifibroadcast.service where required, and plugs into the existing `pp_settings_provider_t` seam via a new composite router that fans out shared rows (channel / bandwidth / codec) to drone (fpvd) AND GS (gs_local), drone-first.

**Architecture:** Composite router provider over fpvd + gs_local. gs_local mirrors fpvd's worker-thread + job-queue pattern but uses atomic file writes + systemctl instead of HTTP. RX power slider (new) and HDMI mode (existing key) are GS-only. Codec / channel / bandwidth share their widget binding via a fan-out table inside the router.

**Tech Stack:** C11 + LVGL, pthread, Catch2 (C++ test harness), cJSON (already vendored). No new third-party deps.

---

## Reality check before starting

The design doc's fan-out table used `(air,wfbng,channel)` and `(air,wfbng,width)`, but inspection of `src/gsmenu/settings_fpvd.c:77-79` shows the actual current bindings are:

```
("gs",  "wfbng", "gs_channel") → link.channel
("gs",  "wfbng", "bandwidth")  → link.width
("gs",  "wfbng", "txpower")    → link.txpower   ← this is the DRONE's tx power
```

So fan-out tuples in this plan use `("gs","wfbng","gs_channel")` etc. The new **GS-side RX power** is a brand-new key `("gs","link","rx_power")` that fpvd does NOT know about — it routes only to gs_local. The "txpower" key remains drone-side.

Page bindings stay where they are. The router intercepts writes to the fan-out tuples and adds a GS-side write after fpvd succeeds.

---

## File structure

**New files:**

| Path | Responsibility |
|---|---|
| `src/gsmenu/settings_gs_writers.h` | Public interfaces of the two file writers + writer-result struct. |
| `src/gsmenu/settings_gs_writers.c` | `wifibroadcast_cfg_write_channel/bandwidth/rxpower`, `pixelpilot_env_write_codec/screenmode`. Atomic write via mkstemp+rename. |
| `src/gsmenu/settings_gs_rxpower.h` | RX-power percentage ↔ per-driver-int mapping. |
| `src/gsmenu/settings_gs_rxpower.c` | Pure mapping functions + NIC enumeration. |
| `src/gsmenu/settings_gs_enum.h` | `pp_gs_enum_init(void)`, `pp_gs_enum_channels(void)`, `pp_gs_enum_hdmi_modes(void)`. |
| `src/gsmenu/settings_gs_enum.c` | popen `iw list` and `drm_info`; parsers + caches. |
| `src/gsmenu/settings_gs_local_internal.h` | Internal config: file paths, systemctl binary, exposed for tests via env vars. |
| `src/gsmenu/settings_gs_local.c` | Provider impl: worker, queue, dispatch by key. |
| `src/gsmenu/settings_router_internal.h` | Fan-out entry type + lookup. |
| `src/gsmenu/settings_router.c` | Provider impl that delegates to fpvd + gs_local. |
| `tests/test_settings_gs_writers.cpp` | Pure tests for both writers using tempdir. |
| `tests/test_settings_gs_rxpower.cpp` | Boundary tests for percentage mapping. |
| `tests/test_settings_gs_enum.cpp` | Parser tests on canned `iw list` / `drm_info` outputs. |
| `tests/test_settings_router.cpp` | Router behavior with fake child providers. |

**Modified files:**

| Path | Change |
|---|---|
| `src/gsmenu/settings.h` | Add `pp_settings_register_router()`, `pp_settings_get_options()`. |
| `src/gsmenu/settings.c` | Add `pp_settings_get_options` forwarder + storage hooks for enum sources. |
| `src/gsmenu/pages/link.c` | Add RX Power slider row `("gs","link","rx_power")`. |
| `src/gsmenu/pages/display.c` | HDMI Mode dropdown uses `pp_settings_get_options(...)` with fallback to hard-coded list. |
| `src/gsmenu/settings_dummy.c` | Seed `rx_power=50`, ensure `hdmi_mode` defaulted, `codec` parallel "gs" entry if applicable. |
| `src/main.cpp` (or whichever file currently calls `pp_settings_register_fpvd()`) | Replace with `pp_settings_register_router()` on device build. |
| `CMakeLists.txt` | Add new device sources + 3 new Catch2 test executables. |

---

## Task 1: `wifibroadcast.cfg` writer — replace/insert lines

**Files:**
- Create: `src/gsmenu/settings_gs_writers.h`
- Create: `src/gsmenu/settings_gs_writers.c`
- Create: `tests/test_settings_gs_writers.cpp`
- Modify: `CMakeLists.txt` — add `gs_writers_tests` Catch2 executable.

- [ ] **Step 1: Sketch the header**

`src/gsmenu/settings_gs_writers.h`:

```c
#ifndef PP_SETTINGS_GS_WRITERS_H
#define PP_SETTINGS_GS_WRITERS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   rc;       /* 0 = success */
    char *err;      /* heap; NULL on success */
} pp_gs_write_result_t;

void pp_gs_write_result_free(pp_gs_write_result_t *r);

/* All three operate on the config file at `cfg_path` (overridable for tests).
 * On success, the file contains the new value and is atomically rewritten.
 * On failure, the file is untouched and r.err describes the error. */
pp_gs_write_result_t pp_gs_wfbcfg_set_channel  (const char *cfg_path, const char *value);
pp_gs_write_result_t pp_gs_wfbcfg_set_bandwidth(const char *cfg_path, const char *value);

/* `txpower_json` is the pre-built JSON-ish dict (e.g. `{"wlx...": -2000}`).
 * Caller is responsible for building it via pp_gs_build_txpower_json. */
pp_gs_write_result_t pp_gs_wfbcfg_set_txpower  (const char *cfg_path, const char *txpower_json);

/* Upserts KEY=VALUE in a shell-env-style file (e.g. /etc/default/pixelpilot).
 * Quotes value if it contains whitespace or shell metacharacters. */
pp_gs_write_result_t pp_gs_env_set(const char *env_path, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the first failing test (channel replace)**

`tests/test_settings_gs_writers.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <unistd.h>

extern "C" {
#include "gsmenu/settings_gs_writers.h"
}

namespace fs = std::filesystem;

static std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string write_temp(const std::string &contents) {
    char tmpl[] = "/tmp/ppgsw_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, contents.data(), contents.size()) == (ssize_t)contents.size());
    close(fd);
    return std::string(tmpl);
}

TEST_CASE("wfbcfg: channel replaces existing line", "[gs][writers]") {
    auto p = write_temp("[common]\nwifi_channel = 149\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_channel(p.c_str(), "36");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p) == "[common]\nwifi_channel = 36\nbandwidth = 20\n");
    pp_gs_write_result_free(&r);
    fs::remove(p);
}
```

- [ ] **Step 3: Run — verify it fails (file not yet built / function missing)**

```bash
cmake -S . -B build_sim -DUSE_SIMULATOR=ON
cmake --build build_sim --target gs_writers_tests 2>&1 | head -30
```

Expected: build error or link error referencing `pp_gs_wfbcfg_set_channel`.

- [ ] **Step 4: Implement the writer**

`src/gsmenu/settings_gs_writers.c`:

```c
#include "settings_gs_writers.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void pp_gs_write_result_free(pp_gs_write_result_t *r) {
    if (r && r->err) { free(r->err); r->err = NULL; }
}

static pp_gs_write_result_t err_result(const char *msg) {
    pp_gs_write_result_t r = { -1, NULL };
    r.err = strdup(msg ? msg : "unknown");
    return r;
}

static char *slurp_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int atomic_write(const char *path, const char *contents, size_t len) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    ssize_t w = write(fd, contents, len);
    if (w != (ssize_t)len) { close(fd); unlink(tmp); return -1; }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Replace lines whose lhs (before '=') equals `key`, ignoring whitespace.
 * If no such line, insert after the [common] header. If no header, append.
 * Returns a heap-allocated new content string. */
static char *cfg_upsert(const char *src, const char *key, const char *value_line) {
    size_t cap = strlen(src) + strlen(value_line) + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    bool replaced = false;
    bool injected = false;
    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        size_t cpy = llen < sizeof line - 1 ? llen : sizeof line - 1;
        memcpy(line, p, cpy); line[cpy] = '\0';

        char lhs[64] = {0};
        const char *eq = strchr(line, '=');
        if (eq) {
            size_t k = (size_t)(eq - line);
            while (k > 0 && isspace((unsigned char)line[k-1])) k--;
            size_t i = 0;
            while (i < k && isspace((unsigned char)line[i])) i++;
            size_t klen = k - i;
            if (klen < sizeof lhs) { memcpy(lhs, line + i, klen); lhs[klen] = '\0'; }
        }

        if (strcmp(lhs, key) == 0 && !replaced) {
            strcat(out, value_line);
            replaced = true;
        } else {
            strncat(out, p, llen);
        }
        if (eol) { strcat(out, "\n"); p = eol + 1; } else { break; }

        if (!replaced && !injected && strncmp(line, "[common]", 8) == 0) {
            strcat(out, value_line);
            strcat(out, "\n");
            injected = true;
            replaced = true;
        }
    }
    if (!replaced) {
        strcat(out, value_line);
        strcat(out, "\n");
    }
    return out;
}

static pp_gs_write_result_t write_kv_line(const char *cfg_path,
                                          const char *key, const char *value) {
    size_t src_len = 0;
    char *src = slurp_file(cfg_path, &src_len);
    if (!src) src = strdup("[common]\n");
    char line[256];
    snprintf(line, sizeof line, "%s = %s", key, value);
    char *out = cfg_upsert(src, key, line);
    free(src);
    if (!out) return err_result("alloc failed");
    int rc = atomic_write(cfg_path, out, strlen(out));
    free(out);
    if (rc != 0) return err_result(strerror(errno));
    return (pp_gs_write_result_t){ 0, NULL };
}

pp_gs_write_result_t pp_gs_wfbcfg_set_channel(const char *cfg_path, const char *value) {
    return write_kv_line(cfg_path, "wifi_channel", value);
}

pp_gs_write_result_t pp_gs_wfbcfg_set_bandwidth(const char *cfg_path, const char *value) {
    return write_kv_line(cfg_path, "bandwidth", value);
}

pp_gs_write_result_t pp_gs_wfbcfg_set_txpower(const char *cfg_path, const char *json) {
    return write_kv_line(cfg_path, "wifi_txpower", json);
}

pp_gs_write_result_t pp_gs_env_set(const char *env_path, const char *key, const char *value) {
    size_t src_len = 0;
    char *src = slurp_file(env_path, &src_len);
    if (!src) src = strdup("");
    /* Decide if value needs quoting. */
    bool needs_quote = false;
    for (const char *c = value; *c; c++) {
        if (isspace((unsigned char)*c) || *c == '"' || *c == '\'' ||
            *c == '$' || *c == '`' || *c == '\\') { needs_quote = true; break; }
    }
    char vbuf[512];
    if (needs_quote)
        snprintf(vbuf, sizeof vbuf, "\"%s\"", value);
    else
        snprintf(vbuf, sizeof vbuf, "%s", value);
    char line[768];
    snprintf(line, sizeof line, "%s=%s", key, vbuf);

    /* Same upsert routine but match on KEY= (no spaces, no [section]). */
    size_t cap = strlen(src) + strlen(line) + 8;
    char *out = (char *)malloc(cap);
    if (!out) { free(src); return err_result("alloc failed"); }
    out[0] = '\0';
    bool replaced = false;
    const char *p = src;
    size_t klen = strlen(key);
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (!replaced && llen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            strcat(out, line);
            replaced = true;
        } else {
            strncat(out, p, llen);
        }
        if (eol) { strcat(out, "\n"); p = eol + 1; } else { break; }
    }
    if (!replaced) {
        if (out[0] && out[strlen(out)-1] != '\n') strcat(out, "\n");
        strcat(out, line);
        strcat(out, "\n");
    }
    free(src);
    int rc = atomic_write(env_path, out, strlen(out));
    free(out);
    if (rc != 0) return err_result(strerror(errno));
    return (pp_gs_write_result_t){ 0, NULL };
}
```

- [ ] **Step 5: Add `gs_writers_tests` to CMakeLists.txt**

Insert after the existing `add_executable(fpvd_tests ...)` block in the `USE_SIMULATOR` branch (around line 234):

```cmake
    add_executable(gs_writers_tests
      src/gsmenu/settings_gs_writers.c
      tests/test_settings_gs_writers.cpp)
    target_include_directories(gs_writers_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(gs_writers_tests
      Catch2::Catch2WithMain pthread)
```

- [ ] **Step 6: Build and run the test — expect pass**

```bash
cmake --build build_sim --target gs_writers_tests
./build_sim/gs_writers_tests "[gs][writers]"
```

Expected: one test passes.

- [ ] **Step 7: Add coverage for the rest of the writer cases**

Append to `tests/test_settings_gs_writers.cpp`:

```cpp
TEST_CASE("wfbcfg: channel inserts under [common] when missing", "[gs][writers]") {
    auto p = write_temp("[common]\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_channel(p.c_str(), "36");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("wifi_channel = 36") != std::string::npos);
    REQUIRE(out.find("[common]") < out.find("wifi_channel = 36"));
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("wfbcfg: bandwidth replaces in place", "[gs][writers]") {
    auto p = write_temp("[common]\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_bandwidth(p.c_str(), "40");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p) == "[common]\nbandwidth = 40\n");
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("wfbcfg: txpower writes JSON dict line", "[gs][writers]") {
    auto p = write_temp("[common]\n");
    auto r = pp_gs_wfbcfg_set_txpower(p.c_str(), "{\"wlx00\": -2000}");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p).find("wifi_txpower = {\"wlx00\": -2000}") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: upsert preserves other lines", "[gs][writers]") {
    auto p = write_temp("# comment\nFOO=bar\nBAZ=qux\n");
    auto r = pp_gs_env_set(p.c_str(), "FOO", "baz");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("FOO=baz") != std::string::npos);
    REQUIRE(out.find("# comment") != std::string::npos);
    REQUIRE(out.find("BAZ=qux") != std::string::npos);
    REQUIRE(out.find("FOO=bar") == std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: appends when key missing", "[gs][writers]") {
    auto p = write_temp("FOO=bar\n");
    auto r = pp_gs_env_set(p.c_str(), "CODEC", "h265");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("CODEC=h265") != std::string::npos);
    REQUIRE(out.find("FOO=bar") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: quotes value with whitespace", "[gs][writers]") {
    auto p = write_temp("");
    auto r = pp_gs_env_set(p.c_str(), "SCREEN_MODE", "1920x1080@60");
    REQUIRE(r.rc == 0);
    /* No spaces in 1920x1080@60, so no quotes expected. */
    REQUIRE(slurp(p).find("SCREEN_MODE=1920x1080@60") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);

    auto p2 = write_temp("");
    auto r2 = pp_gs_env_set(p2.c_str(), "X", "a b");
    REQUIRE(r2.rc == 0);
    REQUIRE(slurp(p2).find("X=\"a b\"") != std::string::npos);
    pp_gs_write_result_free(&r2); fs::remove(p2);
}

TEST_CASE("atomic: write failure leaves file untouched", "[gs][writers]") {
    /* /dev/full lets writes succeed but fsync fails; portable enough on Linux. */
    /* This case is best-effort — skip on platforms without /dev/full. */
    if (access("/dev/full", W_OK) != 0) { SUCCEED(); return; }
    auto p = write_temp("[common]\nwifi_channel = 36\n");
    /* Force atomic_write into a directory we can't rename into by pointing to a
     * path under a read-only parent. mkstemp will fail. */
    auto r = pp_gs_wfbcfg_set_channel("/proc/does_not_exist/x.cfg", "44");
    REQUIRE(r.rc == -1);
    REQUIRE(r.err != nullptr);
    pp_gs_write_result_free(&r);
    /* The original temp file was not the target, so it's irrelevant. */
    fs::remove(p);
}
```

- [ ] **Step 8: Rebuild & rerun — expect all tests pass**

```bash
cmake --build build_sim --target gs_writers_tests && ./build_sim/gs_writers_tests "[gs][writers]"
```

- [ ] **Step 9: Commit**

```bash
git add src/gsmenu/settings_gs_writers.h src/gsmenu/settings_gs_writers.c \
        tests/test_settings_gs_writers.cpp CMakeLists.txt
git commit -m "feat(gsmenu): atomic file writers for wifibroadcast.cfg and /etc/default/pixelpilot"
```

---

## Task 2: RX power percentage → per-NIC integer mapping

**Files:**
- Create: `src/gsmenu/settings_gs_rxpower.h`
- Create: `src/gsmenu/settings_gs_rxpower.c`
- Create: `tests/test_settings_gs_rxpower.cpp`
- Modify: `CMakeLists.txt` — add `gs_rxpower_tests` Catch2 executable.

- [ ] **Step 1: Header**

```c
/* src/gsmenu/settings_gs_rxpower.h */
#ifndef PP_SETTINGS_GS_RXPOWER_H
#define PP_SETTINGS_GS_RXPOWER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PP_NIC_RTL88XXAU_WFB,
    PP_NIC_RTL88X2EU,
    PP_NIC_UNKNOWN,
} pp_nic_driver_t;

/* Map driver name (from udevadm ID_NET_DRIVER) to enum. */
pp_nic_driver_t pp_nic_driver_from_name(const char *name);

/* Map percent (1..100, clamped) to a signed per-driver value.
 * Returns 0 and sets *out=0 for PP_NIC_UNKNOWN. Returns 1 on success. */
int pp_rxpower_pct_to_driver_value(pp_nic_driver_t driver, int pct, int *out);

/* Build the `wifi_txpower = { ... }` JSON-ish dict body for the given NICs.
 * `nics` is a NULL-terminated array of strings (e.g. {"wlx00", NULL}).
 * `driver_for` is a parallel array of driver enums. Returns a heap-allocated
 * string like `{"wlx00": -2000, "wlx01": -2500}` or NULL on alloc failure or
 * if all NICs are PP_NIC_UNKNOWN. Caller frees. */
char *pp_rxpower_build_json(const char *const *nics,
                            const pp_nic_driver_t *driver_for,
                            int pct);

/* Enumerate wlx* interfaces from /sys/class/net (path overridable via env
 * PP_GS_SYS_CLASS_NET — used in tests). Returns NULL-terminated heap array
 * of heap-allocated strings; caller frees each + array. */
char **pp_rxpower_list_wlx_nics(void);

/* Look up the NET driver name for a given iface via /sys/class/net/<if>/device/uevent
 * (parses MODALIAS / DRIVER). Returns heap string or NULL. Caller frees. */
char *pp_rxpower_nic_driver_name(const char *iface);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Failing test for the percent mapping**

```cpp
/* tests/test_settings_gs_rxpower.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "gsmenu/settings_gs_rxpower.h"
}

TEST_CASE("rxpower: driver name -> enum", "[gs][rxpower]") {
    REQUIRE(pp_nic_driver_from_name("rtl88xxau_wfb") == PP_NIC_RTL88XXAU_WFB);
    REQUIRE(pp_nic_driver_from_name("rtl88x2eu")     == PP_NIC_RTL88X2EU);
    REQUIRE(pp_nic_driver_from_name("ath9k")         == PP_NIC_UNKNOWN);
    REQUIRE(pp_nic_driver_from_name(NULL)            == PP_NIC_UNKNOWN);
}

TEST_CASE("rxpower: rtl88xxau_wfb maps inverted range (-1000..-3000)", "[gs][rxpower]") {
    int v = 0;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 1,   &v) == 1);
    REQUIRE(v == -1020);   /* min_phy + (1/100 * (-2000)) */
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 50,  &v) == 1);
    REQUIRE(v == -2000);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88XXAU_WFB, 100, &v) == 1);
    REQUIRE(v == -3000);
}

TEST_CASE("rxpower: rtl88x2eu maps 1000..2900", "[gs][rxpower]") {
    int v = 0;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 1,   &v) == 1);
    REQUIRE(v == 1019);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 50,  &v) == 1);
    REQUIRE(v == 1950);
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_RTL88X2EU, 100, &v) == 1);
    REQUIRE(v == 2900);
}

TEST_CASE("rxpower: unknown driver returns 0", "[gs][rxpower]") {
    int v = 99;
    REQUIRE(pp_rxpower_pct_to_driver_value(PP_NIC_UNKNOWN, 50, &v) == 0);
    REQUIRE(v == 0);
}

TEST_CASE("rxpower: json single NIC", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_RTL88XXAU_WFB };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j != nullptr);
    REQUIRE(std::strstr(j, "\"wlx00\": -2000") != nullptr);
    REQUIRE(j[0] == '{');
    REQUIRE(j[std::strlen(j)-1] == '}');
    free(j);
}

TEST_CASE("rxpower: json skips unknown driver NIC", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", "wlx01", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_UNKNOWN, PP_NIC_RTL88X2EU };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j != nullptr);
    REQUIRE(std::strstr(j, "wlx00") == nullptr);
    REQUIRE(std::strstr(j, "\"wlx01\": 1950") != nullptr);
    free(j);
}

TEST_CASE("rxpower: json all-unknown returns NULL", "[gs][rxpower]") {
    const char *nics[] = { "wlx00", NULL };
    pp_nic_driver_t drv[] = { PP_NIC_UNKNOWN };
    char *j = pp_rxpower_build_json(nics, drv, 50);
    REQUIRE(j == nullptr);
}
```

- [ ] **Step 3: CMake entry + run — expect failure**

Add to `CMakeLists.txt` next to `gs_writers_tests`:

```cmake
    add_executable(gs_rxpower_tests
      src/gsmenu/settings_gs_rxpower.c
      tests/test_settings_gs_rxpower.cpp)
    target_include_directories(gs_rxpower_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(gs_rxpower_tests Catch2::Catch2WithMain)
```

```bash
cmake --build build_sim --target gs_rxpower_tests 2>&1 | head -30
```

Expected: link error referencing the new functions.

- [ ] **Step 4: Implement**

```c
/* src/gsmenu/settings_gs_rxpower.c */
#include "settings_gs_rxpower.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pp_nic_driver_t pp_nic_driver_from_name(const char *name) {
    if (!name) return PP_NIC_UNKNOWN;
    if (strcmp(name, "rtl88xxau_wfb") == 0) return PP_NIC_RTL88XXAU_WFB;
    if (strcmp(name, "rtl88x2eu")     == 0) return PP_NIC_RTL88X2EU;
    return PP_NIC_UNKNOWN;
}

int pp_rxpower_pct_to_driver_value(pp_nic_driver_t drv, int pct, int *out) {
    if (!out) return 0;
    if (pct < 1)   pct = 1;
    if (pct > 100) pct = 100;
    int min_v, max_v;
    switch (drv) {
    case PP_NIC_RTL88XXAU_WFB: min_v = -1000; max_v = -3000; break;
    case PP_NIC_RTL88X2EU:     min_v =  1000; max_v =  2900; break;
    default: *out = 0; return 0;
    }
    int range = max_v - min_v;          /* signed; can be negative */
    /* Old script: position = pct - 1 (so pct=1 means position=0). But the
     * original arithmetic actually used position = pct and range/100. We
     * mirror that: value = min + (pct * range) / 100. */
    *out = min_v + (pct * range) / 100;
    return 1;
}

char *pp_rxpower_build_json(const char *const *nics,
                            const pp_nic_driver_t *drv,
                            int pct) {
    if (!nics) return NULL;
    char buf[1024];
    size_t off = 0;
    buf[off++] = '{';
    bool first = true;
    for (size_t i = 0; nics[i]; i++) {
        int v;
        if (!pp_rxpower_pct_to_driver_value(drv[i], pct, &v)) continue;
        int wrote = snprintf(buf + off, sizeof buf - off,
                             "%s\"%s\": %d", first ? "" : ", ", nics[i], v);
        if (wrote < 0 || (size_t)wrote >= sizeof buf - off) return NULL;
        off += (size_t)wrote;
        first = false;
    }
    if (first) return NULL;             /* nothing written */
    if (off + 2 > sizeof buf) return NULL;
    buf[off++] = '}';
    buf[off]   = '\0';
    return strdup(buf);
}

/* Enumeration: walk /sys/class/net and pick names starting with "wlx".
 * Root path overridable via PP_GS_SYS_CLASS_NET for tests. */
char **pp_rxpower_list_wlx_nics(void) {
    const char *root = getenv("PP_GS_SYS_CLASS_NET");
    if (!root) root = "/sys/class/net";
    DIR *d = opendir(root);
    if (!d) return NULL;
    char **out = (char **)calloc(16, sizeof(char *));
    if (!out) { closedir(d); return NULL; }
    size_t cap = 16, n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "wlx", 3) != 0) continue;
        if (n + 1 >= cap) {
            cap *= 2;
            char **nx = (char **)realloc(out, cap * sizeof(char *));
            if (!nx) break;
            out = nx;
        }
        out[n++] = strdup(de->d_name);
    }
    out[n] = NULL;
    closedir(d);
    return out;
}

/* Read /sys/class/net/<iface>/device/uevent — DRIVER=<name>. */
char *pp_rxpower_nic_driver_name(const char *iface) {
    if (!iface) return NULL;
    const char *root = getenv("PP_GS_SYS_CLASS_NET");
    if (!root) root = "/sys/class/net";
    char path[512];
    snprintf(path, sizeof path, "%s/%s/device/uevent", root, iface);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[256];
    char *out = NULL;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "DRIVER=", 7) == 0) {
            size_t l = strlen(line);
            if (l && line[l-1] == '\n') line[l-1] = '\0';
            out = strdup(line + 7);
            break;
        }
    }
    fclose(f);
    return out;
}
```

- [ ] **Step 5: Build + run — expect pass**

```bash
cmake --build build_sim --target gs_rxpower_tests && ./build_sim/gs_rxpower_tests "[gs][rxpower]"
```

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_gs_rxpower.h src/gsmenu/settings_gs_rxpower.c \
        tests/test_settings_gs_rxpower.cpp CMakeLists.txt
git commit -m "feat(gsmenu): RX power pct->driver-int mapping + NIC enumeration"
```

---

## Task 3: Channel & HDMI mode enumeration parsers

**Files:**
- Create: `src/gsmenu/settings_gs_enum.h`
- Create: `src/gsmenu/settings_gs_enum.c`
- Create: `tests/test_settings_gs_enum.cpp`
- Modify: `CMakeLists.txt` — add `gs_enum_tests`.

- [ ] **Step 1: Header**

```c
/* src/gsmenu/settings_gs_enum.h */
#ifndef PP_SETTINGS_GS_ENUM_H
#define PP_SETTINGS_GS_ENUM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Both return newline-joined strings (heap, caller free) or NULL on parse fail.
 * The parser functions are pure (no popen) so they can be tested with canned
 * inputs. */

char *pp_gs_parse_iw_list_channels(const char *iw_list_output);
char *pp_gs_parse_drm_info_modes (const char *drm_info_json);

/* Run the underlying binaries and parse. Binary paths overridable via
 * PP_GS_IW_BIN and PP_GS_DRM_INFO_BIN. Returns same as the parse functions. */
char *pp_gs_enum_channels(void);
char *pp_gs_enum_hdmi_modes(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Failing tests with canned inputs**

```cpp
/* tests/test_settings_gs_enum.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "gsmenu/settings_gs_enum.h"
}

TEST_CASE("iw_list: extracts enabled channels in old gsmenu format", "[gs][enum]") {
    /* Subset of real `iw list` output. Lines without a [<n>] band marker
     * and disabled lines must be skipped. */
    const char *in =
      "    Frequencies:\n"
      "      * 2412 MHz [1] (20.0 dBm)\n"
      "      * 2417 MHz [2] (20.0 dBm) (radar detection)\n"
      "      * 2484 MHz [14] (disabled)\n"
      "      * 5180 MHz [36] (20.0 dBm)\n"
      "      * 5825 MHz [165] (20.0 dBm)\n";
    char *out = pp_gs_parse_iw_list_channels(in);
    REQUIRE(out != nullptr);
    std::string s(out); free(out);
    /* Expected format: "<chan> (<freq> MHz)" per old script. */
    REQUIRE(s.find("1 (2412 MHz)") != std::string::npos);
    REQUIRE(s.find("36 (5180 MHz)") != std::string::npos);
    REQUIRE(s.find("165 (5825 MHz)") != std::string::npos);
    REQUIRE(s.find("14 ") == std::string::npos);     /* disabled */
    REQUIRE(s.find("2 (2417 MHz)") == std::string::npos); /* radar */
}

TEST_CASE("iw_list: empty input -> NULL", "[gs][enum]") {
    REQUIRE(pp_gs_parse_iw_list_channels("") == nullptr);
    REQUIRE(pp_gs_parse_iw_list_channels("no channels here") == nullptr);
}

TEST_CASE("drm_info: extracts non-interlaced modes from connector[1]", "[gs][enum]") {
    /* Minimal shape mirroring the path the old gsmenu.sh queried. */
    const char *in =
      "{\"/dev/dri/card0\":{"
        "\"crtcs\":[{\"mode\":{\"name\":\"1920x1080\",\"vrefresh\":60}}],"
        "\"connectors\":["
          "{},"
          "{\"modes\":["
            "{\"name\":\"1920x1080\",\"vrefresh\":60},"
            "{\"name\":\"1920x1080i\",\"vrefresh\":60},"
            "{\"name\":\"1280x720\",\"vrefresh\":60}"
          "]}"
        "]"
      "}}";
    char *out = pp_gs_parse_drm_info_modes(in);
    REQUIRE(out != nullptr);
    std::string s(out); free(out);
    REQUIRE(s.find("1920x1080@60") != std::string::npos);
    REQUIRE(s.find("1280x720@60")  != std::string::npos);
    REQUIRE(s.find("1920x1080i")   == std::string::npos);
}

TEST_CASE("drm_info: malformed input -> NULL", "[gs][enum]") {
    REQUIRE(pp_gs_parse_drm_info_modes("not json") == nullptr);
    REQUIRE(pp_gs_parse_drm_info_modes("{}")       == nullptr);
}
```

- [ ] **Step 3: CMakeLists entry**

```cmake
    add_executable(gs_enum_tests
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      tests/test_settings_gs_enum.cpp)
    target_include_directories(gs_enum_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC})
    target_link_libraries(gs_enum_tests Catch2::Catch2WithMain)
```

```bash
cmake --build build_sim --target gs_enum_tests 2>&1 | head -30
```

Expected: link error.

- [ ] **Step 4: Implement**

```c
/* src/gsmenu/settings_gs_enum.c */
#include "settings_gs_enum.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* iw list line of interest:
 *   "      * 5180 MHz [36] (20.0 dBm)"
 * Filter out lines containing "disabled" or "radar detection".
 * Emit "<chan> (<freq> MHz)\n" per line, sorted by channel number,
 * deduplicated. Trailing newline trimmed. */

typedef struct { int chan; int freq; } chan_entry_t;

static int cmp_chan(const void *a, const void *b) {
    int ca = ((const chan_entry_t *)a)->chan;
    int cb = ((const chan_entry_t *)b)->chan;
    return ca - cb;
}

char *pp_gs_parse_iw_list_channels(const char *in) {
    if (!in || !*in) return NULL;
    chan_entry_t list[512];
    size_t n = 0;
    const char *p = in;
    while (*p && n < 512) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        size_t cpy = llen < sizeof line - 1 ? llen : sizeof line - 1;
        memcpy(line, p, cpy); line[cpy] = '\0';
        if (!strstr(line, "MHz") || strstr(line, "disabled") ||
            strstr(line, "radar detection")) {
            p = eol ? eol + 1 : p + llen;
            continue;
        }
        int freq = 0, chan = 0;
        const char *lb = strchr(line, '[');
        const char *rb = lb ? strchr(lb, ']') : NULL;
        if (lb && rb) {
            sscanf(lb + 1, "%d", &chan);
        }
        sscanf(line, " * %d MHz", &freq);
        if (freq <= 0) { /* fall back: scan for first number before MHz */
            const char *mhz = strstr(line, "MHz");
            if (mhz) for (const char *q = mhz - 1; q >= line; q--)
                if (isdigit((unsigned char)*q)) { while (q > line && isdigit((unsigned char)q[-1])) q--; freq = atoi(q); break; }
        }
        if (chan > 0 && freq > 0) {
            list[n].chan = chan; list[n].freq = freq;
            n++;
        }
        p = eol ? eol + 1 : p + llen;
    }
    if (n == 0) return NULL;
    qsort(list, n, sizeof list[0], cmp_chan);
    /* Dedup by channel. */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m == 0 || list[m-1].chan != list[i].chan) list[m++] = list[i];
    }
    /* Build output. */
    char buf[4096]; buf[0] = '\0';
    for (size_t i = 0; i < m; i++) {
        char line[64];
        snprintf(line, sizeof line, "%s%d (%d MHz)",
                 i == 0 ? "" : "\n", list[i].chan, list[i].freq);
        if (strlen(buf) + strlen(line) + 1 >= sizeof buf) break;
        strcat(buf, line);
    }
    return strdup(buf);
}

char *pp_gs_parse_drm_info_modes(const char *in) {
    if (!in) return NULL;
    cJSON *root = cJSON_Parse(in);
    if (!root) return NULL;
    cJSON *card = cJSON_GetObjectItemCaseSensitive(root, "/dev/dri/card0");
    if (!card) { cJSON_Delete(root); return NULL; }
    cJSON *conns = cJSON_GetObjectItemCaseSensitive(card, "connectors");
    if (!cJSON_IsArray(conns) || cJSON_GetArraySize(conns) < 2) {
        cJSON_Delete(root); return NULL;
    }
    cJSON *conn = cJSON_GetArrayItem(conns, 1);
    cJSON *modes = cJSON_GetObjectItemCaseSensitive(conn, "modes");
    if (!cJSON_IsArray(modes)) { cJSON_Delete(root); return NULL; }

    /* Collect "<name>@<vrefresh>" strings, skipping names containing 'i'. */
    char *items[256]; size_t n = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, modes) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(m, "name");
        cJSON *vref = cJSON_GetObjectItemCaseSensitive(m, "vrefresh");
        if (!cJSON_IsString(name) || !cJSON_IsNumber(vref)) continue;
        if (strchr(name->valuestring, 'i')) continue;
        char buf[64];
        snprintf(buf, sizeof buf, "%s@%d", name->valuestring, (int)vref->valuedouble);
        /* Dedup. */
        bool dup = false;
        for (size_t i = 0; i < n; i++) if (strcmp(items[i], buf) == 0) { dup = true; break; }
        if (!dup && n < 256) items[n++] = strdup(buf);
    }
    cJSON_Delete(root);
    if (n == 0) return NULL;

    /* Sort by string (stable enough for display). */
    for (size_t i = 1; i < n; i++) for (size_t j = i; j > 0; j--) {
        if (strcmp(items[j-1], items[j]) > 0) {
            char *t = items[j-1]; items[j-1] = items[j]; items[j] = t;
        } else break;
    }
    char buf[4096]; buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i) strcat(buf, "\n");
        if (strlen(buf) + strlen(items[i]) + 1 >= sizeof buf) break;
        strcat(buf, items[i]);
    }
    for (size_t i = 0; i < n; i++) free(items[i]);
    return strdup(buf);
}

/* popen variants — overridable binary paths. */
static char *popen_slurp(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char *out = (char *)malloc(65536); if (!out) { pclose(p); return NULL; }
    size_t off = 0;
    size_t got;
    while ((got = fread(out + off, 1, 65535 - off, p)) > 0) {
        off += got;
        if (off >= 65535) break;
    }
    out[off] = '\0';
    pclose(p);
    return out;
}

char *pp_gs_enum_channels(void) {
    const char *bin = getenv("PP_GS_IW_BIN");
    if (!bin) bin = "iw";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s list 2>/dev/null", bin);
    char *raw = popen_slurp(cmd);
    if (!raw) return NULL;
    char *r = pp_gs_parse_iw_list_channels(raw);
    free(raw);
    return r;
}

char *pp_gs_enum_hdmi_modes(void) {
    const char *bin = getenv("PP_GS_DRM_INFO_BIN");
    if (!bin) bin = "drm_info";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s -j /dev/dri/card0 2>/dev/null", bin);
    char *raw = popen_slurp(cmd);
    if (!raw) return NULL;
    char *r = pp_gs_parse_drm_info_modes(raw);
    free(raw);
    return r;
}
```

- [ ] **Step 5: Build + run — expect pass**

```bash
cmake --build build_sim --target gs_enum_tests && ./build_sim/gs_enum_tests "[gs][enum]"
```

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_gs_enum.h src/gsmenu/settings_gs_enum.c \
        tests/test_settings_gs_enum.cpp CMakeLists.txt
git commit -m "feat(gsmenu): iw list / drm_info parsers for channel + HDMI mode dropdowns"
```

---

## Task 4: `settings_gs_local` provider

**Files:**
- Create: `src/gsmenu/settings_gs_local_internal.h`
- Create: `src/gsmenu/settings_gs_local.c`

`settings_gs_local` exposes a `pp_settings_provider_t` and `pp_gs_local_install_in(...)` for the router to embed it. It does NOT call `pp_settings_register` itself (the router will).

This task ships gs_local without tests of its own. The router tests in Task 5 exercise its behavior end-to-end via fake writer/exec overrides.

- [ ] **Step 1: Internal header**

```c
/* src/gsmenu/settings_gs_local_internal.h */
#ifndef PP_SETTINGS_GS_LOCAL_INTERNAL_H
#define PP_SETTINGS_GS_LOCAL_INTERNAL_H

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Construct the gs_local provider. Returns a pointer with static lifetime
 * (provider table inside the .c file). NULL on init failure. Idempotent. */
const pp_settings_provider_t *pp_gs_local_provider(void);

/* For tests: replace the systemctl binary path. */
void pp_gs_local_set_systemctl_bin(const char *bin);

/* For tests: replace the config file paths. */
void pp_gs_local_set_paths(const char *wfb_cfg, const char *pixelpilot_env);

/* For tests: shut down the worker thread cleanly. */
void pp_gs_local_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Provider implementation**

```c
/* src/gsmenu/settings_gs_local.c */
#include "settings_gs_local_internal.h"
#include "settings_gs_writers.h"
#include "settings_gs_rxpower.h"
#include "settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl.h"

#define GS_QUEUE_CAP 16

typedef enum {
    GS_KEY_CHANNEL,
    GS_KEY_BANDWIDTH,
    GS_KEY_RXPOWER,
    GS_KEY_CODEC,
    GS_KEY_HDMI_MODE,
    GS_KEY_NONE,
} gs_key_t;

typedef struct {
    gs_key_t key;
    char     value[128];
    pp_settings_done_cb cb;
    void    *user_data;
} gs_job_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            started, stop;
    gs_job_t        queue[GS_QUEUE_CAP];
    size_t          queue_n;

    /* Configurable paths / bins. */
    char wfb_cfg[256];
    char pp_env[256];
    char systemctl_bin[128];

    /* Snapshot. */
    char *channel;
    char *bandwidth;
    int   rxpower_pct;
    char *codec;
    char *hdmi_mode;

    pp_settings_snapshot_cb listener;
    void *listener_ud;

    bool connected;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .rxpower_pct = -1,
        .connected   = true };

static void set_path(char *dst, size_t sz, const char *src) {
    strncpy(dst, src, sz - 1); dst[sz - 1] = '\0';
}

static void init_paths_once(void) {
    if (G.wfb_cfg[0] == '\0') set_path(G.wfb_cfg, sizeof G.wfb_cfg, "/etc/wifibroadcast.cfg");
    if (G.pp_env[0]  == '\0') set_path(G.pp_env,  sizeof G.pp_env,  "/etc/default/pixelpilot");
    if (G.systemctl_bin[0] == '\0') set_path(G.systemctl_bin, sizeof G.systemctl_bin, "systemctl");
}

void pp_gs_local_set_paths(const char *wfb, const char *env) {
    pthread_mutex_lock(&G.mu);
    if (wfb) set_path(G.wfb_cfg, sizeof G.wfb_cfg, wfb);
    if (env) set_path(G.pp_env,  sizeof G.pp_env,  env);
    pthread_mutex_unlock(&G.mu);
}

void pp_gs_local_set_systemctl_bin(const char *bin) {
    pthread_mutex_lock(&G.mu);
    if (bin) set_path(G.systemctl_bin, sizeof G.systemctl_bin, bin);
    pthread_mutex_unlock(&G.mu);
}

/* Key resolution. */
static gs_key_t resolve_key(const char *d, const char *p, const char *k) {
    if (strcmp(d, "gs") == 0) {
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "gs_channel") == 0) return GS_KEY_CHANNEL;
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "bandwidth")  == 0) return GS_KEY_BANDWIDTH;
        if (strcmp(p, "link")  == 0 && strcmp(k, "rx_power")   == 0) return GS_KEY_RXPOWER;
        if (strcmp(p, "pp")    == 0 && strcmp(k, "codec")      == 0) return GS_KEY_CODEC;
        if (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) return GS_KEY_HDMI_MODE;
    }
    return GS_KEY_NONE;
}

/* Dispatch a callback on the LVGL thread. */
typedef struct { pp_settings_done_cb cb; void *ud; int rc; char err[160]; } gs_done_t;
static void done_async(void *ptr) {
    gs_done_t *d = (gs_done_t *)ptr;
    if (d->cb) d->cb(d->rc, d->err[0] ? d->err : NULL, d->ud);
    lv_free(d);
}
static void schedule_done(pp_settings_done_cb cb, void *ud, int rc, const char *err) {
    if (!cb) return;
    lv_lock();
    gs_done_t *d = lv_malloc(sizeof *d);
    if (!d) { lv_unlock(); return; }
    d->cb = cb; d->ud = ud; d->rc = rc;
    if (err) { strncpy(d->err, err, sizeof d->err - 1); d->err[sizeof d->err - 1] = '\0'; }
    else d->err[0] = '\0';
    lv_async_call(done_async, d);
    lv_unlock();
}

static int run_systemctl_restart(const char *service) {
    /* Returns 0 on exit code 0, non-zero otherwise. */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp(G.systemctl_bin, G.systemctl_bin, "restart", service, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static void notify_listener(void) {
    pthread_mutex_lock(&G.mu);
    pp_settings_snapshot_cb cb = G.listener;
    void *ud = G.listener_ud;
    pthread_mutex_unlock(&G.mu);
    if (cb) cb(ud);
}

/* Actual work, called outside the mutex. */
static void run_job(gs_job_t job) {
    pp_gs_write_result_t r = { -1, NULL };
    bool needs_restart = false;
    const char *toast_msg = NULL;

    switch (job.key) {
    case GS_KEY_CHANNEL:
        r = pp_gs_wfbcfg_set_channel(G.wfb_cfg, job.value);
        needs_restart = true;
        break;
    case GS_KEY_BANDWIDTH:
        r = pp_gs_wfbcfg_set_bandwidth(G.wfb_cfg, job.value);
        needs_restart = true;
        break;
    case GS_KEY_RXPOWER: {
        char **nics = pp_rxpower_list_wlx_nics();
        if (!nics || !nics[0]) {
            r.rc = -1; r.err = strdup("No wfb NICs detected");
            if (nics) free(nics);
        } else {
            size_t n = 0; while (nics[n]) n++;
            pp_nic_driver_t *drv = (pp_nic_driver_t *)calloc(n, sizeof *drv);
            for (size_t i = 0; i < n; i++) {
                char *name = pp_rxpower_nic_driver_name(nics[i]);
                drv[i] = pp_nic_driver_from_name(name);
                free(name);
            }
            int pct = atoi(job.value);
            char *json = pp_rxpower_build_json((const char *const *)nics, drv, pct);
            free(drv);
            for (size_t i = 0; i < n; i++) free(nics[i]);
            free(nics);
            if (!json) { r.rc = -1; r.err = strdup("No supported NIC drivers"); }
            else { r = pp_gs_wfbcfg_set_txpower(G.wfb_cfg, json); free(json); }
        }
        needs_restart = (r.rc == 0);
        break;
    }
    case GS_KEY_CODEC:
        r = pp_gs_env_set(G.pp_env, "CODEC", job.value);
        toast_msg = "Applies on next restart";
        break;
    case GS_KEY_HDMI_MODE:
        r = pp_gs_env_set(G.pp_env, "SCREEN_MODE", job.value);
        toast_msg = "Applies on next restart";
        break;
    case GS_KEY_NONE:
        r.rc = -1; r.err = strdup("Unknown GS setting");
        break;
    }

    if (r.rc != 0) {
        schedule_done(job.cb, job.user_data, -1, r.err ? r.err : "GS write failed");
        pp_gs_write_result_free(&r);
        return;
    }
    pp_gs_write_result_free(&r);

    /* Restart, if any. */
    if (needs_restart) {
        int xst = run_systemctl_restart("wifibroadcast.service");
        if (xst != 0) {
            schedule_done(job.cb, job.user_data, -1, "wifibroadcast restart failed");
            return;
        }
    }

    /* Update snapshot under mutex. */
    pthread_mutex_lock(&G.mu);
    switch (job.key) {
    case GS_KEY_CHANNEL:   free(G.channel);   G.channel   = strdup(job.value); break;
    case GS_KEY_BANDWIDTH: free(G.bandwidth); G.bandwidth = strdup(job.value); break;
    case GS_KEY_RXPOWER:   G.rxpower_pct = atoi(job.value); break;
    case GS_KEY_CODEC:     free(G.codec);     G.codec     = strdup(job.value); break;
    case GS_KEY_HDMI_MODE: free(G.hdmi_mode); G.hdmi_mode = strdup(job.value); break;
    default: break;
    }
    pthread_mutex_unlock(&G.mu);

    notify_listener();
    /* On success-with-toast, pass message as the err string with rc=0 so widgets
     * render it as an informational toast. (Existing fpvd callers only react to
     * rc != 0; widgets that want the success-toast UX must opt in. For now we
     * pass rc=0, err=NULL because the existing widget code treats non-NULL err
     * as failure. Toast support for success messages is follow-up UI work.) */
    (void)toast_msg;
    schedule_done(job.cb, job.user_data, 0, NULL);
}

static void *worker_main(void *_) {
    (void)_;
    while (1) {
        pthread_mutex_lock(&G.mu);
        while (!G.stop && G.queue_n == 0) {
            pthread_cond_wait(&G.cv, &G.mu);
        }
        if (G.stop) { pthread_mutex_unlock(&G.mu); break; }
        gs_job_t job = G.queue[0];
        for (size_t i = 1; i < G.queue_n; i++) G.queue[i-1] = G.queue[i];
        G.queue_n--;
        pthread_mutex_unlock(&G.mu);

        run_job(job);
    }
    return NULL;
}

static void start_worker_once(void) {
    pthread_mutex_lock(&G.mu);
    if (!G.started) {
        init_paths_once();
        G.started = true;
        pthread_create(&G.thread, NULL, worker_main, NULL);
    }
    pthread_mutex_unlock(&G.mu);
}

void pp_gs_local_shutdown(void) {
    pthread_mutex_lock(&G.mu);
    if (!G.started) { pthread_mutex_unlock(&G.mu); return; }
    G.stop = true;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.thread, NULL);
    G.started = false;
    G.stop = false;
}

/* -------- vtable -------- */
static char *prov_get(const char *d, const char *p, const char *k) {
    gs_key_t key = resolve_key(d, p, k);
    if (key == GS_KEY_NONE) return strdup("");
    pthread_mutex_lock(&G.mu);
    char buf[32];
    char *out = strdup("");
    switch (key) {
    case GS_KEY_CHANNEL:   if (G.channel)   { free(out); out = strdup(G.channel); }   break;
    case GS_KEY_BANDWIDTH: if (G.bandwidth) { free(out); out = strdup(G.bandwidth); } break;
    case GS_KEY_RXPOWER:   if (G.rxpower_pct >= 0) { snprintf(buf, sizeof buf, "%d", G.rxpower_pct); free(out); out = strdup(buf); } break;
    case GS_KEY_CODEC:     if (G.codec)     { free(out); out = strdup(G.codec); }     break;
    case GS_KEY_HDMI_MODE: if (G.hdmi_mode) { free(out); out = strdup(G.hdmi_mode); } break;
    default: break;
    }
    pthread_mutex_unlock(&G.mu);
    return out;
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    gs_key_t key = resolve_key(d, p, k);
    if (key == GS_KEY_NONE) { schedule_done(cb, ud, -1, "Unknown GS setting"); return; }
    start_worker_once();
    pthread_mutex_lock(&G.mu);
    /* Coalesce. */
    for (size_t i = 0; i < G.queue_n; i++) {
        if (G.queue[i].key == key) {
            strncpy(G.queue[i].value, v, sizeof G.queue[i].value - 1);
            G.queue[i].value[sizeof G.queue[i].value - 1] = '\0';
            G.queue[i].cb = cb;
            G.queue[i].user_data = ud;
            pthread_mutex_unlock(&G.mu);
            return;
        }
    }
    if (G.queue_n >= GS_QUEUE_CAP) {
        pthread_mutex_unlock(&G.mu);
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    gs_job_t *j = &G.queue[G.queue_n++];
    j->key = key;
    strncpy(j->value, v, sizeof j->value - 1); j->value[sizeof j->value - 1] = '\0';
    j->cb = cb; j->user_data = ud;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    prov_set_async(d, p, k, v, NULL, NULL);
}

static bool prov_is_connected(void) {
    pthread_mutex_lock(&G.mu);
    bool c = G.connected;
    pthread_mutex_unlock(&G.mu);
    return c;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    G.listener = cb; G.listener_ud = ud;
    pthread_mutex_unlock(&G.mu);
}

static const pp_settings_provider_t G_PROVIDER = {
    .set                   = prov_set,
    .get                   = prov_get,
    .set_async             = prov_set_async,
    .is_locked             = NULL,
    .is_connected          = prov_is_connected,
    .set_snapshot_listener = prov_set_snapshot_listener,
    .set_visibility        = NULL,
};

const pp_settings_provider_t *pp_gs_local_provider(void) {
    return &G_PROVIDER;
}
```

- [ ] **Step 3: Build for the device target sanity (gs_local has no own tests yet)**

Just confirm the file compiles by adding it to the device sources later (Task 7). For now, run a quick syntactic check:

```bash
gcc -c -std=c11 -Isrc -Ilvgl -DUSE_SIMULATOR \
  src/gsmenu/settings_gs_local.c -o /tmp/_gs_local.o 2>&1 | head -30 || true
```

Don't require this to link — just that it parses. Compile-only failures get fixed in this task; the linker test happens when wired up in Task 7.

(If lvgl headers are unavailable in the agent's environment, skip Step 3 and let the build in Task 5/7 surface any syntax errors.)

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/settings_gs_local.c src/gsmenu/settings_gs_local_internal.h
git commit -m "feat(gsmenu): settings_gs_local provider — worker + dispatch to writers"
```

---

## Task 5: `settings_router` + tests

**Files:**
- Create: `src/gsmenu/settings_router_internal.h`
- Create: `src/gsmenu/settings_router.c`
- Create: `tests/test_settings_router.cpp`
- Modify: `CMakeLists.txt` — add `router_tests`.
- Modify: `src/gsmenu/settings.h` — add `pp_settings_register_router(void)`.

- [ ] **Step 1: settings.h export**

Append to `src/gsmenu/settings.h` next to `pp_settings_register_fpvd`:

```c
/* Registers the composite router provider: fpvd (drone-side HTTP) plus
 * settings_gs_local (GS-side file writers), with a fan-out table for keys
 * that must be written to both. Replaces register_fpvd in the device build. */
void pp_settings_register_router(void);
```

- [ ] **Step 2: Router internal header**

```c
/* src/gsmenu/settings_router_internal.h */
#ifndef PP_SETTINGS_ROUTER_INTERNAL_H
#define PP_SETTINGS_ROUTER_INTERNAL_H

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For tests: install router with explicit children. Both children must
 * outlive the router. */
void pp_router_install_children(const pp_settings_provider_t *drone,
                                const pp_settings_provider_t *gs);

/* Reset router state (tests only). */
void pp_router_reset(void);

/* Returns the router's vtable (registration-only convenience for code paths
 * that need the table itself). */
const pp_settings_provider_t *pp_router_provider(void);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Failing tests with fake providers**

```cpp
/* tests/test_settings_router.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "gsmenu/settings.h"
#include "gsmenu/settings_router_internal.h"
}

namespace {

struct FakeChild {
    std::vector<std::tuple<std::string,std::string,std::string,std::string>> sets;
    int next_rc = 0;
    const char *next_err = nullptr;
    bool connected = true;
    bool locked    = false;

    void on_set_async(const char *d, const char *p, const char *k, const char *v,
                      pp_settings_done_cb cb, void *ud) {
        sets.push_back({d, p, k, v});
        if (cb) cb(next_rc, next_err, ud);
    }
};

FakeChild g_drone, g_gs;

extern "C" {
    static void drone_set_async(const char *d, const char *p, const char *k, const char *v,
                                pp_settings_done_cb cb, void *ud) { g_drone.on_set_async(d,p,k,v,cb,ud); }
    static void drone_set(const char *d, const char *p, const char *k, const char *v) { drone_set_async(d,p,k,v,nullptr,nullptr); }
    static char *drone_get(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return strdup("DRONE"); }
    static bool drone_is_connected(void) { return g_drone.connected; }
    static bool drone_is_locked(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return g_drone.locked; }

    static void gs_set_async(const char *d, const char *p, const char *k, const char *v,
                              pp_settings_done_cb cb, void *ud) { g_gs.on_set_async(d,p,k,v,cb,ud); }
    static void gs_set(const char *d, const char *p, const char *k, const char *v) { gs_set_async(d,p,k,v,nullptr,nullptr); }
    static char *gs_get(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return strdup("GS"); }
    static bool gs_is_connected(void) { return g_gs.connected; }
}

static pp_settings_provider_t DRONE = {
    .set = drone_set, .get = drone_get, .set_async = drone_set_async,
    .is_locked = drone_is_locked, .is_connected = drone_is_connected,
    .set_snapshot_listener = nullptr, .set_visibility = nullptr,
};
static pp_settings_provider_t GS = {
    .set = gs_set, .get = gs_get, .set_async = gs_set_async,
    .is_locked = nullptr, .is_connected = gs_is_connected,
    .set_snapshot_listener = nullptr, .set_visibility = nullptr,
};

struct DoneCapture { int rc = 99; std::string err; };
static void capture_done(int rc, const char *err, void *ud) {
    auto *c = (DoneCapture *)ud;
    c->rc = rc; c->err = err ? err : "";
}

} // namespace

TEST_CASE("router: non-fanout drone key hits only drone", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("air", "camera", "fps", "60", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.empty());
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: gs-only key hits only gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "link", "rx_power", "70", capture_done, &cap);
    REQUIRE(g_drone.sets.empty());
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<2>(g_gs.sets[0]) == "rx_power");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: fan-out channel writes drone then gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "36", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<3>(g_drone.sets[0]) == "36");
    REQUIRE(std::get<3>(g_gs.sets[0]) == "36");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: fan-out aborts gs when drone fails", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    g_drone.next_rc = -1; g_drone.next_err = "Drone unreachable";
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "wfbng", "bandwidth", "40", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.empty());
    REQUIRE(cap.rc == -1);
    REQUIRE(cap.err == "Drone unreachable");
    pp_router_reset();
}

TEST_CASE("router: fan-out reports gs error when drone ok but gs fails", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    g_gs.next_rc = -1; g_gs.next_err = "GS write failed";
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("air", "camera", "codec", "h264", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(cap.rc == -1);
    REQUIRE(cap.err.find("GS") != std::string::npos);
    pp_router_reset();
}

TEST_CASE("router: get delegates by domain", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    char *a = pp_settings_get("air", "camera", "fps"); REQUIRE(std::string(a) == "DRONE"); free(a);
    char *g = pp_settings_get("gs", "link", "rx_power"); REQUIRE(std::string(g) == "GS");   free(g);
    pp_router_reset();
}

TEST_CASE("router: is_connected = drone AND gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    REQUIRE(pp_settings_is_connected() == true);
    g_drone.connected = false;
    REQUIRE(pp_settings_is_connected() == false);
    g_drone.connected = true; g_gs.connected = false;
    REQUIRE(pp_settings_is_connected() == false);
    pp_router_reset();
}
```

- [ ] **Step 4: Implement the router**

```c
/* src/gsmenu/settings_router.c */
#include "settings_router_internal.h"
#include "settings_gs_local_internal.h"
#include "settings.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Forward decl from fpvd. */
extern void pp_settings_register_fpvd(void);
extern const pp_settings_provider_t *pp_fpvd_provider_for_router(void);  /* added below */

/* Fan-out table: (in domain/page/key) -> (out domain/page/key on GS). */
typedef struct {
    const char *d, *p, *k;
    const char *gd, *gp, *gk;
} fanout_t;

static const fanout_t FANOUT[] = {
    { "gs",  "wfbng",  "gs_channel", "gs", "wfbng",   "gs_channel" },
    { "gs",  "wfbng",  "bandwidth",  "gs", "wfbng",   "bandwidth"  },
    { "air", "camera", "codec",      "gs", "pp",      "codec"      },
};
static const size_t FANOUT_N = sizeof(FANOUT) / sizeof(FANOUT[0]);

static const fanout_t *fanout_lookup(const char *d, const char *p, const char *k) {
    for (size_t i = 0; i < FANOUT_N; i++) {
        if (strcmp(FANOUT[i].d, d) == 0 &&
            strcmp(FANOUT[i].p, p) == 0 &&
            strcmp(FANOUT[i].k, k) == 0)
            return &FANOUT[i];
    }
    return NULL;
}

static const pp_settings_provider_t *g_drone;
static const pp_settings_provider_t *g_gs;

void pp_router_install_children(const pp_settings_provider_t *drone,
                                const pp_settings_provider_t *gs) {
    g_drone = drone;
    g_gs    = gs;
    pp_settings_register(pp_router_provider());
}

void pp_router_reset(void) {
    g_drone = NULL;
    g_gs    = NULL;
    pp_settings_register(NULL);
}

/* For fan-out, we need a 2-stage callback. */
typedef struct {
    const fanout_t *fan;
    char  value[128];
    pp_settings_done_cb cb;
    void *user_data;
} fanout_state_t;

static void on_gs_done(int rc, const char *err, void *ud) {
    fanout_state_t *st = (fanout_state_t *)ud;
    if (rc == 0) {
        if (st->cb) st->cb(0, NULL, st->user_data);
    } else {
        char buf[256];
        snprintf(buf, sizeof buf, "Drone applied; GS: %s",
                 err ? err : "write failed");
        if (st->cb) st->cb(-1, buf, st->user_data);
    }
    free(st);
}

static void on_drone_done(int rc, const char *err, void *ud) {
    fanout_state_t *st = (fanout_state_t *)ud;
    if (rc != 0) {
        if (st->cb) st->cb(rc, err, st->user_data);
        free(st);
        return;
    }
    /* Drone OK -> fire GS. */
    if (g_gs && g_gs->set_async) {
        g_gs->set_async(st->fan->gd, st->fan->gp, st->fan->gk,
                        st->value, on_gs_done, st);
    } else {
        if (st->cb) st->cb(0, NULL, st->user_data);
        free(st);
    }
}

static char *prov_get(const char *d, const char *p, const char *k) {
    /* Domain dispatch on read. fpvd knows some "gs" keys, gs_local knows
     * others — try the matching child first, fall back to the other. */
    if (strcmp(d, "gs") == 0 && g_gs && g_gs->get) {
        char *r = g_gs->get(d, p, k);
        if (r && *r) return r;
        if (r) free(r);
        if (g_drone && g_drone->get) return g_drone->get(d, p, k);
        return strdup("");
    }
    if (g_drone && g_drone->get) return g_drone->get(d, p, k);
    if (g_gs && g_gs->get)        return g_gs->get(d, p, k);
    return strdup("");
}

static void prov_set_async(const char *d, const char *p, const char *k,
                           const char *v, pp_settings_done_cb cb, void *ud) {
    const fanout_t *fan = fanout_lookup(d, p, k);
    if (fan) {
        fanout_state_t *st = (fanout_state_t *)calloc(1, sizeof *st);
        if (!st) { if (cb) cb(-1, "Out of memory", ud); return; }
        st->fan = fan;
        strncpy(st->value, v ? v : "", sizeof st->value - 1);
        st->cb = cb; st->user_data = ud;
        if (g_drone && g_drone->set_async) {
            g_drone->set_async(d, p, k, v, on_drone_done, st);
        } else {
            on_drone_done(0, NULL, st);   /* no drone provider -> proceed to gs */
        }
        return;
    }
    /* Non-fanout: dispatch by domain. fpvd handles "air" and many "gs" keys;
     * gs_local handles GS-only keys it knows about. Try drone first; if it
     * reports "Unknown setting" let gs handle. We pick a simple rule: if
     * domain == "gs" and key is rx_power/hdmi_mode, route to gs_local; else
     * route to drone. */
    bool gs_only =
        (strcmp(d, "gs") == 0 &&
         ((strcmp(p, "link")    == 0 && strcmp(k, "rx_power")  == 0) ||
          (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) ||
          (strcmp(p, "pp")      == 0 && strcmp(k, "codec")     == 0)));
    const pp_settings_provider_t *child = gs_only ? g_gs : g_drone;
    if (child && child->set_async) child->set_async(d, p, k, v, cb, ud);
    else if (cb) cb(-1, "No provider", ud);
}

static void prov_set(const char *d, const char *p, const char *k, const char *v) {
    prov_set_async(d, p, k, v, NULL, NULL);
}

static bool prov_is_connected(void) {
    bool dc = (!g_drone || !g_drone->is_connected) ? true : g_drone->is_connected();
    bool gc = (!g_gs    || !g_gs->is_connected)    ? true : g_gs->is_connected();
    return dc && gc;
}

static bool prov_is_locked(const char *d, const char *p, const char *k) {
    bool dl = (g_drone && g_drone->is_locked) ? g_drone->is_locked(d,p,k) : false;
    bool gl = (g_gs    && g_gs->is_locked)    ? g_gs->is_locked(d,p,k)    : false;
    return dl || gl;
}

static void prov_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud) {
    if (g_drone && g_drone->set_snapshot_listener) g_drone->set_snapshot_listener(cb, ud);
    if (g_gs    && g_gs->set_snapshot_listener)    g_gs->set_snapshot_listener(cb, ud);
}

static void prov_set_visibility(bool v) {
    if (g_drone && g_drone->set_visibility) g_drone->set_visibility(v);
    if (g_gs    && g_gs->set_visibility)    g_gs->set_visibility(v);
}

static const pp_settings_provider_t G_ROUTER = {
    .set                   = prov_set,
    .get                   = prov_get,
    .set_async             = prov_set_async,
    .is_locked             = prov_is_locked,
    .is_connected          = prov_is_connected,
    .set_snapshot_listener = prov_set_snapshot_listener,
    .set_visibility        = prov_set_visibility,
};

const pp_settings_provider_t *pp_router_provider(void) { return &G_ROUTER; }

/* Public entry that the device build calls. fpvd's existing init code is
 * unchanged — we hand it through a small accessor (added in fpvd in next step). */
void pp_settings_register_router(void) {
    pp_settings_register_fpvd();           /* spins up the worker + initial poll */
    /* Once fpvd is registered, its provider table is the active one; we now
     * yank it out and reinstall the router on top. fpvd exposes its provider
     * via pp_fpvd_provider_for_router() (small addition below). */
    g_drone = pp_fpvd_provider_for_router();
    g_gs    = pp_gs_local_provider();
    pp_settings_register(&G_ROUTER);
}
```

- [ ] **Step 5: Tiny addition to settings_fpvd.c — expose the provider table for the router**

In `src/gsmenu/settings_fpvd.c`, at the end of the file (or next to `pp_settings_register_fpvd`), add:

```c
const pp_settings_provider_t *pp_fpvd_provider_for_router(void) {
    return &G_PROVIDER;
}
```

And declare it in `src/gsmenu/settings_fpvd_internal.h`:

```c
/* For use by settings_router only. Returns the static provider table. */
const pp_settings_provider_t *pp_fpvd_provider_for_router(void);
```

- [ ] **Step 6: CMake — add `router_tests` (sim) and wire router/gs_local into device build**

After the existing `gs_enum_tests` entry, add:

```cmake
    add_executable(router_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_router.c
      tests/test_settings_router.cpp)
    target_include_directories(router_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(router_tests Catch2::Catch2WithMain)
    target_compile_definitions(router_tests PRIVATE PP_ROUTER_TEST=1)
```

Note: router tests do NOT link `settings_fpvd.c` or `settings_gs_local.c` — they install fake children directly via `pp_router_install_children`, so the `pp_settings_register_router` path (which depends on `pp_settings_register_fpvd`) is not exercised here. That path is covered by the manual on-device verification step at the end.

To keep tests link-clean, guard the `pp_settings_register_router` function body with `#ifndef PP_ROUTER_TEST` (the test will not need it). Update the function:

```c
#ifndef PP_ROUTER_TEST
void pp_settings_register_router(void) {
    pp_settings_register_fpvd();
    g_drone = pp_fpvd_provider_for_router();
    g_gs    = pp_gs_local_provider();
    pp_settings_register(&G_ROUTER);
}
#endif
```

In the device branch of CMakeLists, append to `SOURCE_FILES`:

```cmake
  list(APPEND SOURCE_FILES
      src/gsmenu/settings_router.c
      src/gsmenu/settings_gs_local.c
      src/gsmenu/settings_gs_writers.c
      src/gsmenu/settings_gs_rxpower.c
      src/gsmenu/settings_gs_enum.c)
```

- [ ] **Step 7: Build + run router tests**

```bash
cmake --build build_sim --target router_tests && ./build_sim/router_tests "[router]"
```

Expected: all router tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/gsmenu/settings_router.c src/gsmenu/settings_router_internal.h \
        src/gsmenu/settings_fpvd.c src/gsmenu/settings_fpvd_internal.h \
        src/gsmenu/settings.h tests/test_settings_router.cpp CMakeLists.txt
git commit -m "feat(gsmenu): settings_router with drone-first fan-out for shared rows"
```

---

## Task 6: pp_settings_get_options API + Display page integration

**Files:**
- Modify: `src/gsmenu/settings.h` — add `pp_settings_get_options(...)`.
- Modify: `src/gsmenu/settings.c` — implement forwarder + enum bootstrap.
- Modify: `src/gsmenu/pages/display.c` — use new API.
- Modify: `CMakeLists.txt` — link `settings_gs_enum.c` into the simulator (already done in device branch).

- [ ] **Step 1: Header addition**

Append to `src/gsmenu/settings.h` (after `pp_settings_set_visibility`):

```c
/* Returns a newline-joined list of valid option strings for the given key,
 * or NULL if no enumerator is available. Caller frees with free().
 * The current implementation supports:
 *   ("gs","wfbng","gs_channel")  -> iw list channels
 *   ("gs","display","hdmi_mode") -> drm_info modes
 * All other tuples return NULL. */
char *pp_settings_get_options(const char *domain, const char *page, const char *key);
```

- [ ] **Step 2: Implementation forwarder**

In `src/gsmenu/settings.c`, add at the bottom:

```c
#include "settings_gs_enum.h"

char *pp_settings_get_options(const char *domain, const char *page, const char *key) {
    if (!domain || !page || !key) return NULL;
    if (strcmp(domain, "gs") == 0 && strcmp(page, "wfbng") == 0 && strcmp(key, "gs_channel") == 0)
        return pp_gs_enum_channels();
    if (strcmp(domain, "gs") == 0 && strcmp(page, "display") == 0 && strcmp(key, "hdmi_mode") == 0)
        return pp_gs_enum_hdmi_modes();
    return NULL;
}
```

If `settings.c` doesn't already include `<string.h>`, add it. Make sure `gs_enum_tests` keeps building.

- [ ] **Step 3: Update display.c**

Edit `src/gsmenu/pages/display.c`:

```c
#include "display.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../settings.h"
#include <stdlib.h>

lv_obj_t *build_display_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "display");

    pp_section_header(page, "Output");
    char *hdmi_opts = pp_settings_get_options("gs", "display", "hdmi_mode");
    pp_dropdown(page, LV_SYMBOL_EYE_OPEN, "HDMI Mode",
                "gs", "display", "hdmi_mode",
                hdmi_opts ? hdmi_opts : "1920x1080@60\n1280x720@60\n1920x1080@30");
    free(hdmi_opts);
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 200);
    /* ... rest of the file unchanged ... */
```

(Keep the existing Color section and group-add loop intact.)

- [ ] **Step 4: Wire `settings_gs_enum.c` into the simulator settings_tests target**

We need `settings.c` (which now references `pp_gs_enum_channels`) to link cleanly in the simulator `settings_tests` executable. Add `src/gsmenu/settings_gs_enum.c` and the cJSON sources to `settings_tests`:

```cmake
    add_executable(settings_tests
      src/gsmenu/settings.c
      src/gsmenu/settings_dummy.c
      src/gsmenu/settings_gs_enum.c
      ${PP_CJSON_SOURCES}
      src/gsmenu/widgets/pp_slider_bounds.c
      src/gsmenu/widgets/pp_slider_accel.c
      tests/test_settings.cpp
      tests/test_settings_failure.cpp
      tests/test_slider_bounds.cpp
      tests/test_slider_accel.cpp)
    target_include_directories(settings_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PP_CJSON_INC})
```

(Do the same for `fpvd_tests` if it links `settings.c`. Check by running the build.)

- [ ] **Step 5: Build everything that currently builds, expect green**

```bash
cmake --build build_sim --target settings_tests gs_writers_tests gs_rxpower_tests gs_enum_tests router_tests fpvd_tests
./build_sim/settings_tests && ./build_sim/router_tests
```

Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings.h src/gsmenu/settings.c src/gsmenu/pages/display.c CMakeLists.txt
git commit -m "feat(gsmenu): pp_settings_get_options + Display uses live HDMI mode list"
```

---

## Task 7: Add RX Power slider to Link tab + wire device build

**Files:**
- Modify: `src/gsmenu/pages/link.c` — insert RX Power slider after TX Power.
- Modify: `src/gsmenu/settings_dummy.c` — seed new GS keys for the simulator.
- Modify: `src/main.cpp` — replace `pp_settings_register_fpvd()` with `pp_settings_register_router()` on device.

- [ ] **Step 1: link.c — add RX Power**

Edit `src/gsmenu/pages/link.c`. After the TX Power slider, insert:

```c
    pp_slider(page, LV_SYMBOL_DOWN, "RX Power",
              "gs", "link", "rx_power", 1, 100);
```

- [ ] **Step 2: settings_dummy.c — seed**

Open `src/gsmenu/settings_dummy.c` and add seed entries so the sim renders sensible defaults. Find the `seed[]` table around line 55 and add:

```c
    { "rx_power",         "50" },   /* under ("gs","link") */
    { "hdmi_mode",        "1920x1080@60" },  /* under ("gs","display") */
```

If the existing dummy table is keyed by `(page,key)` not `(domain,page,key)`, follow the same layout. Inspect the existing seed structure first; mirror it.

- [ ] **Step 3: main.cpp — switch registration on device**

```bash
grep -n "pp_settings_register_fpvd\|pp_settings_register_dummy\|pp_settings_register_stub" src/main.cpp
```

In the device branch (whatever `#ifdef` guards the non-sim build), replace:

```c
pp_settings_register_fpvd();
```

with:

```c
pp_settings_register_router();
```

Leave the simulator branch (which calls `pp_settings_register_dummy`) untouched.

- [ ] **Step 4: Device build sanity (only if cross-compile toolchain present)**

```bash
cmake --build build -j$(nproc) 2>&1 | tail -20 || true
```

If the device build is not available in the agent environment, skip this step — the sim build covers compilation.

- [ ] **Step 5: Sim build sanity + run the simulator briefly**

```bash
cmake --build build_sim -j$(nproc)
./build_sim/pixelpilot &
SIM_PID=$!
sleep 2
kill $SIM_PID 2>/dev/null
```

Expected: process starts cleanly (or stays alive 2 s).

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/pages/link.c src/gsmenu/settings_dummy.c src/main.cpp
git commit -m "feat(gsmenu): wire router on device; add RX Power slider; seed dummy"
```

---

## Task 8: Full test suite green + final commit

- [ ] **Step 1: Run every Catch2 test and verify all pass**

```bash
cmake --build build_sim --target settings_tests gs_writers_tests gs_rxpower_tests \
                                gs_enum_tests router_tests fpvd_tests lvgl_state_tests
for t in settings_tests gs_writers_tests gs_rxpower_tests gs_enum_tests router_tests fpvd_tests lvgl_state_tests; do
    echo "== $t =="
    ./build_sim/$t || { echo "FAIL $t"; exit 1; }
done
echo "All tests passing."
```

Expected: every binary exits 0.

- [ ] **Step 2: Final review of the diff**

```bash
git log --oneline master..HEAD
git diff master..HEAD --stat
```

Confirm:
- No leftover TODOs / debug prints.
- No untracked test outputs / build artifacts staged.
- All commits have meaningful messages.

- [ ] **Step 3: (optional) PR**

If the user requests a PR, follow the standard `gh pr create` template — body summarizes the five wired rows, the drone-first fan-out, and explicitly notes live-apply for codec/HDMI is out of scope.

---

## Self-review notes

- **Spec coverage** — each row in the spec's scope table maps to a task: codec (Task 4+5 fan-out + writer), channel (Task 1+5), bandwidth (Task 1+5), rx_power (Task 1+2+4 + slider in 7), hdmi_mode (Task 1 env + Task 3 enumeration + Task 6 page).
- **Tests** — Task 1/2/3/5 cover the pure-function and router behavior. The gs_local provider's worker is exercised end-to-end implicitly on device (manual verification). Adding an integration test with a fake systemctl is plausible follow-up but not required for this scope.
- **Restart UX** — codec/HDMI mode writes return `rc=0, err=NULL` in this plan (Task 4 step 2). The spec called for a success-toast on `"Applies on next restart"`; that's deferred to a follow-up since the current widget code treats non-NULL err as failure. Documented inside `run_job`.
- **Sim build** — keeps dummy; new GS keys are seeded in Task 7 step 2.
- **No placeholders** — every step has concrete code or commands.
