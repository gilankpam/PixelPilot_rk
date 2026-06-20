# gsmenu ⇆ fpvd-GS PixelPilot config — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the gsmenu's PixelPilot rows to fpvd-GS's `pixelpilot` config API through a new `EP_CONFIG` endpoint group, consolidate them into one **PixelPilot** tab (Display + DVR sections + Screen Mode/RTP Jitter), and stage changes for a single Apply button (one pixelpilot restart per batch).

**Architecture:** `settings_fpvd.c` already routes by an `fpvd_endpoint_t` + three pure path helpers + per-endpoint snapshots. Add a third group (`EP_CONFIG` → `/config`, `/apply`, `GET /config?pending=true`), make its rows `PATCH`-only (staged), add an apply-only job + `pp_settings_apply()`, a general `is_available` rule that greys unmapped rows, and a consolidated `pages/pixelpilot.c`.

**Tech Stack:** C11 (gsmenu), LVGL, cJSON, libcurl, Catch2 tests (`fpvd_tests` target), cpp-httplib mock server for integration tests.

**Spec:** `docs/superpowers/specs/2026-06-03-gsmenu-pixelpilot-config-design.md`

**Build/test commands** (run inside the dev shell — `nix-shell` if the toolchain isn't on PATH):
- Unit/integration tests: `cmake -S . -B build-test -DBUILD_TESTS=ON >/dev/null && cmake --build build-test --target fpvd_tests -j 2>&1 | tail -5 && ./build-test/fpvd_tests` (filter with a tag, e.g. `./build-test/fpvd_tests "[fpvd]"`).
- Sim/device compile check (for page/menu/widget tasks): `./sim.sh` (builds the simulator; Ctrl-C after it launches) — or build the sim cmake target directly.

---

### Task 1: `EP_CONFIG` enum + 3-way path helpers

**Files:**
- Modify: `src/gsmenu/settings_fpvd_internal.h` (enum + helper signatures)
- Modify: `src/gsmenu/settings_fpvd.c` (helper bodies; `run_job_unlocked` uses them)
- Test: `tests/test_settings_fpvd.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("path helpers route EP_CONFIG to /config and /apply", "[fpvd][endpoint]") {
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_CONFIG), "/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_CONFIG), "/apply")  == 0);
    REQUIRE(std::strcmp(fpvd_read_path (FPVD_EP_CONFIG), "/config") == 0);
    // existing groups unchanged
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_LINK), "/link") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_AIR),  "/air/apply") == 0);
}
```

- [ ] **Step 2: Run test to verify it fails** — `cmake --build build-test --target fpvd_tests -j 2>&1 | tail -5` → compile error: `FPVD_EP_CONFIG` undeclared / `fpvd_write_path` takes a pointer, not an enum.

- [ ] **Step 3: Implement** — in `src/gsmenu/settings_fpvd_internal.h`, add the enum value and change the three helper signatures to take the endpoint enum:

```c
typedef enum {
    FPVD_EP_AIR,    /* drone proxy:   /air/config + /air/apply,  GET /air/config */
    FPVD_EP_LINK,   /* GS link coord: /link       + /link/apply, GET /link       */
    FPVD_EP_CONFIG, /* GS config:     /config     + /apply,      GET /config (pending) */
} fpvd_endpoint_t;
```

```c
/* Endpoint → URL path. Pure; never NULL. */
const char *fpvd_write_path(fpvd_endpoint_t ep);
const char *fpvd_apply_path(fpvd_endpoint_t ep);
const char *fpvd_read_path (fpvd_endpoint_t ep);
```

In `src/gsmenu/settings_fpvd.c`, replace the three helper bodies:

```c
const char *fpvd_write_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link";
    case FPVD_EP_CONFIG: return "/config";
    default:             return "/air/config";
    }
}
const char *fpvd_apply_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link/apply";
    case FPVD_EP_CONFIG: return "/apply";
    default:             return "/air/apply";
    }
}
const char *fpvd_read_path(fpvd_endpoint_t ep) {
    switch (ep) {
    case FPVD_EP_LINK:   return "/link";
    case FPVD_EP_CONFIG: return "/config";
    default:             return "/air/config";
    }
}
```

In `run_job_unlocked`, replace the hardcoded path lines:

```c
    const char *wpath = (job.endpoint == FPVD_EP_LINK) ? "/link"       : "/air/config";
    const char *apath = (job.endpoint == FPVD_EP_LINK) ? "/link/apply" : "/air/apply";
```

with:

```c
    const char *wpath = fpvd_write_path(job.endpoint);
    const char *apath = fpvd_apply_path(job.endpoint);
```

- [ ] **Step 4: Run test to verify it passes** — `cmake --build build-test --target fpvd_tests -j 2>&1 | tail -3 && ./build-test/fpvd_tests "[endpoint]"` → all pass.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd_internal.h src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp
git commit -m "feat(gsmenu): add EP_CONFIG endpoint group + endpoint-keyed path helpers"
```

---

### Task 2: Keymap — 11 `EP_CONFIG` rows

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (`KEYMAP`)
- Test: `tests/test_settings_fpvd.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/test_settings_fpvd.cpp`:

```cpp
TEST_CASE("keymap: pixelpilot rows route to EP_CONFIG + pixelpilot.* paths", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;
    e = fpvd_keymap_lookup("gs", "display", "video_scale");
    REQUIRE(e != nullptr);
    REQUIRE(e->endpoint == FPVD_EP_CONFIG);
    REQUIRE(std::strcmp(e->path, "pixelpilot.videoScale") == 0);
    REQUIRE(e->type == FPVD_T_PERCENT_TO_FRAC);

    e = fpvd_keymap_lookup("gs", "display", "screen_mode");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "pixelpilot.screenMode") == 0);
    REQUIRE(e->type == FPVD_T_STRING);

    e = fpvd_keymap_lookup("gs", "display", "rtp_jitter_ms");
    REQUIRE(std::strcmp(e->path, "pixelpilot.rtpJitterMs") == 0);

    e = fpvd_keymap_lookup("gs", "dvr", "dvr_reenc_bitrate");
    REQUIRE(e != nullptr);
    REQUIRE(e->endpoint == FPVD_EP_CONFIG);
    REQUIRE(std::strcmp(e->path, "pixelpilot.dvr.reencBitrate") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("gs", "dvr", "dvr_osd");
    REQUIRE(std::strcmp(e->path, "pixelpilot.dvr.osd") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);

    // color correction stays unmapped (handled by the unavailable rule)
    REQUIRE(fpvd_keymap_lookup("gs", "display", "color_correction") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "rec_enabled") == nullptr);
}
```

- [ ] **Step 2: Run test to verify it fails** — `./build-test/fpvd_tests "[keymap]"` (after rebuild) → the new REQUIREs fail (`e == nullptr`).

- [ ] **Step 3: Implement** — in `src/gsmenu/settings_fpvd.c`, add this block to the `KEYMAP[]` array (after the Dynamic Link section, before the closing `};`):

```c
    /* PixelPilot launch config → fpvd /config (pixelpilot.*); staged, applied on demand */
    { "gs","display","screen_mode",      "pixelpilot.screenMode",          FPVD_T_STRING,          FPVD_EP_CONFIG, NULL },
    { "gs","display","video_scale",      "pixelpilot.videoScale",          FPVD_T_PERCENT_TO_FRAC, FPVD_EP_CONFIG, NULL },
    { "gs","display","rtp_jitter_ms",    "pixelpilot.rtpJitterMs",         FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_mode",             "pixelpilot.dvr.mode",            FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
    { "gs","dvr","rec_fps",              "pixelpilot.dvr.framerate",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_max_size",         "pixelpilot.dvr.maxSizeMb",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_reenc_codec",      "pixelpilot.dvr.reencCodec",      FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_reenc_resolution", "pixelpilot.dvr.reencResolution", FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_reenc_fps",        "pixelpilot.dvr.reencFps",        FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_reenc_bitrate",    "pixelpilot.dvr.reencBitrate",    FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
    { "gs","dvr","dvr_osd",              "pixelpilot.dvr.osd",             FPVD_T_BOOL,            FPVD_EP_CONFIG, NULL },
```

- [ ] **Step 4: Run test to verify it passes** — `cmake --build build-test --target fpvd_tests -j 2>&1 | tail -3 && ./build-test/fpvd_tests "[keymap]"` → all pass.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd.cpp
git commit -m "feat(gsmenu): keymap routes pixelpilot Display/DVR rows to EP_CONFIG"
```

---

### Task 3: Provider interface — `is_available`, `apply`, `has_pending` wrappers

**Files:**
- Modify: `src/gsmenu/settings.h` (struct + wrapper decls)
- Modify: `src/gsmenu/settings.c` (wrapper impls)
- Test: `tests/test_settings.cpp` (the `settings_tests` target, which links `settings.c`)

- [ ] **Step 1: Write the failing test** — append to `tests/test_settings.cpp`:

```cpp
TEST_CASE("provider wrappers return safe defaults when methods are absent", "[settings][caps]") {
    pp_settings_register_stub();                 // stub omits the new methods
    REQUIRE(pp_settings_is_available("x","y","z") == true);   // default available
    REQUIRE(pp_settings_has_pending() == false);             // default no pending
    int rc = 99;
    pp_settings_apply([](int r, const char*, void* ud){ *(int*)ud = r; }, &rc);
    REQUIRE(rc == -1);                            // no apply method → error callback
}
```

- [ ] **Step 2: Run test to verify it fails** — `cmake --build build-test --target settings_tests -j 2>&1 | tail -5` → compile error: `pp_settings_is_available` / `pp_settings_apply` / `pp_settings_has_pending` undeclared.

- [ ] **Step 3: Implement** — in `src/gsmenu/settings.h`, add three fields to `pp_settings_provider_t` (after `set_visibility`):

```c
    /* Optional: returns true if the key is backed by this provider (has a
     * route). NULL → dispatcher returns true (row stays interactive). */
    bool  (*is_available)(const char *domain, const char *page, const char *key);

    /* Optional: commit staged changes (e.g. POST /apply). on_done may be NULL.
     * NULL → dispatcher calls on_done(-1, ...). */
    void  (*apply)(pp_settings_done_cb on_done, void *user_data);

    /* Optional: true if there are staged-but-unapplied changes. NULL → false. */
    bool  (*has_pending)(void);
```

and the wrapper declarations (after `pp_settings_set_visibility`):

```c
bool  pp_settings_is_available(const char *domain, const char *page,
                               const char *key);
void  pp_settings_apply(pp_settings_done_cb on_done, void *user_data);
bool  pp_settings_has_pending(void);
```

In `src/gsmenu/settings.c`, add the wrapper bodies (after `pp_settings_set_visibility`):

```c
bool pp_settings_is_available(const char *d, const char *p, const char *k) {
    if (g_provider && g_provider->is_available) {
        return g_provider->is_available(d, p, k);
    }
    return true;
}

void pp_settings_apply(pp_settings_done_cb on_done, void *user_data) {
    if (g_provider && g_provider->apply) {
        g_provider->apply(on_done, user_data);
    } else if (on_done) {
        on_done(-1, "apply not supported", user_data);
    }
}

bool pp_settings_has_pending(void) {
    return (g_provider && g_provider->has_pending) ? g_provider->has_pending() : false;
}
```

- [ ] **Step 4: Run test to verify it passes** — `cmake --build build-test --target settings_tests -j 2>&1 | tail -3 && ./build-test/settings_tests "[caps]"` → pass.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings.h src/gsmenu/settings.c tests/test_settings.cpp
git commit -m "feat(gsmenu): provider gains is_available / apply / has_pending"
```

---

### Task 4: fpvd provider — config snapshot, stage/apply, `is_available`

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c`
- Test: `tests/test_settings_fpvd_integration.cpp`

This is the core. It adds `config_snapshot` (`GET /config?pending=true`), makes `EP_CONFIG` rows `PATCH`-only, adds an apply-only job + `prov_apply`, a `config_dirty` flag + `prov_has_pending`, and `prov_is_available`, and registers them.

- [ ] **Step 1: Write the failing integration test** — in `tests/test_settings_fpvd_integration.cpp`, extend the `GsMockServer` with `/config` (GET + PATCH) and `/apply` (POST). Add to the struct's counters and `start()`:

```cpp
    std::atomic<int> config_get{0}, config_patch{0}, apply_post{0};
    std::string last_config_patch_body;
    std::string config_response =
        R"({"link":{"channel":161,"width":20},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";
```

and in `start()` (next to the existing handlers):

```cpp
        svr.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            config_get++; res.set_content(config_response, "application/json");
        });
        svr.Patch("/config", [this](const httplib::Request& req, httplib::Response& res) {
            config_patch++; last_config_patch_body = req.body;
            res.set_content("{}", "application/json");
        });
        svr.Post("/apply", [this](const httplib::Request&, httplib::Response& res) {
            apply_post++; res.set_content(R"({"applied":true})", "application/json");
        });
```

Add a test case that mirrors the existing ones exactly (they use the helpers `install_provider_pointing_at(m.port)` and `wait_first_poll(m)`, and poll counters with a `for`-loop):

```cpp
TEST_CASE("integration: pixelpilot row stages via /config, no apply; Apply posts /apply",
          "[fpvd][network][config]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    /* a DVR row stages: PATCH /config, NO /apply */
    pp_settings_set_async("gs", "dvr", "dvr_reenc_bitrate", "12000", nullptr, nullptr);
    for (int i = 0; i < 200 && m.config_patch == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.config_patch >= 1);
    REQUIRE(m.apply_post == 0);                          // staged, not applied
    REQUIRE(m.last_config_patch_body.find("\"reencBitrate\":12000") != std::string::npos);

    /* explicit Apply: POST /apply, no further PATCH */
    int patch_before = m.config_patch.load();
    pp_settings_apply(nullptr, nullptr);
    for (int i = 0; i < 200 && m.apply_post == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.apply_post >= 1);
    REQUIRE(m.config_patch == patch_before);            // apply does not PATCH
}
```

- [ ] **Step 2: Run test to verify it fails** — `cmake --build build-test --target fpvd_tests -j 2>&1 | tail -5 && ./build-test/fpvd_tests "[config]"` → fails: `apply_post == 1` (current code applies every row) and no `config_patch`.

- [ ] **Step 3: Implement** — in `src/gsmenu/settings_fpvd.c`:

(a) Add `apply_only` to the job struct (`fpvd_job_t`):

```c
typedef struct fpvd_job {
    char     path[128];
    char     value[128];
    fpvd_type_t type;
    fpvd_endpoint_t endpoint;
    char     apply_to[8];
    bool     apply_only;          /* true → skip PATCH, just POST /apply */
    pp_settings_done_cb on_done;
    void    *user_data;
} fpvd_job_t;
```

(b) Add to `fpvd_state_t`: `cJSON *config_snapshot;` (next to `gs_snapshot`) and `bool config_dirty;`.

(c) In `refresh_snapshot_unlocked`, add a third fetch. After the existing `air_url` setup, add a `config_url`, fetch it, and parse into `config_snapshot`:

```c
    char *config_url = url_join(G.base_url, "/config?pending=true");
    /* ... include config_url in the NULL-check/free with link_url/air_url ... */
    pthread_mutex_unlock(&G.mu);
    fpvd_http_result_t lr = fpvd_http_get(link_url);
    fpvd_http_result_t ar = fpvd_http_get(air_url);
    fpvd_http_result_t cr = fpvd_http_get(config_url);
    pthread_mutex_lock(&G.mu);
    free(link_url); free(air_url); free(config_url);
    /* ... existing lr/ar handling ... */
    if (cr.status == 200 && cr.body) {
        cJSON *c = cJSON_Parse(cr.body);
        if (c) { if (G.config_snapshot) cJSON_Delete(G.config_snapshot); G.config_snapshot = c; }
    }
    fpvd_http_result_free(&cr);
```

(d) In `prov_get`, make the snapshot selection 3-way:

```c
    cJSON *snap;
    switch (e->endpoint) {
    case FPVD_EP_LINK:   snap = G.gs_snapshot;     break;
    case FPVD_EP_CONFIG: snap = G.config_snapshot; break;
    default:             snap = G.air_snapshot;    break;
    }
```

(e) Restructure `run_job_unlocked` so `EP_CONFIG` stages (PATCH-only) and apply-only jobs skip the PATCH. Replace the body from the PATCH section through the apply section with:

```c
    char *patch_url = NULL, *apply_url = NULL, *body_s = NULL;
    int rc = 0;
    char err[160] = {0};

    if (!job.apply_only) {
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
        body_s = body ? cJSON_PrintUnformatted(body) : NULL;
        if (body) cJSON_Delete(body);

        patch_url = url_join(G.base_url, fpvd_write_path(job.endpoint));
        if (!patch_url) { if (body_s) free(body_s);
            schedule_done(job.on_done, job.user_data, -1, "Out of memory"); return; }

        fpvd_http_result_t r = fpvd_http_patch_json(patch_url, body_s ? body_s : "{}");
        if (r.status == 0) {
            rc = -1; snprintf(err, sizeof err, "GS unreachable");
            pthread_mutex_lock(&G.mu); bool was = G.connected; G.connected = false;
            pthread_mutex_unlock(&G.mu); if (was) notify_listener();
        } else if (r.status >= 400) {
            rc = -1; const char *m = parse_error_message(r.body);
            snprintf(err, sizeof err, "%s", m ? m : "Request rejected");
        }
        fpvd_http_result_free(&r);

        /* EP_CONFIG rows stage only: a successful PATCH marks pending dirty and
         * stops here (no apply). AIR/LINK fall through to apply immediately. */
        if (rc == 0 && job.endpoint == FPVD_EP_CONFIG) {
            pthread_mutex_lock(&G.mu); G.config_dirty = true;
            refresh_snapshot_unlocked(); pthread_mutex_unlock(&G.mu);
            schedule_done(job.on_done, job.user_data, 0, NULL);
            free(patch_url); if (body_s) free(body_s);
            return;
        }
    }

    if (rc == 0) {
        apply_url = url_join(G.base_url, fpvd_apply_path(job.endpoint));
        if (!apply_url) { free(patch_url); if (body_s) free(body_s);
            schedule_done(job.on_done, job.user_data, -1, "Out of memory"); return; }
        fpvd_http_result_t r;
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
            pthread_mutex_lock(&G.mu); bool was = G.connected; G.connected = false;
            pthread_mutex_unlock(&G.mu); if (was) notify_listener();
        } else if (r.status >= 400) {
            rc = -1; const char *m = parse_error_message(r.body);
            snprintf(err, sizeof err, "%s", m ? m : "Apply failed");
        }
        fpvd_http_result_free(&r);
        if (rc == 0 && job.apply_only) {
            pthread_mutex_lock(&G.mu); G.config_dirty = false; pthread_mutex_unlock(&G.mu);
        }
    }

    if (rc == 0) {
        pthread_mutex_lock(&G.mu); refresh_snapshot_unlocked(); pthread_mutex_unlock(&G.mu);
    }
    schedule_done(job.on_done, job.user_data, rc, err[0] ? err : NULL);
    free(patch_url); free(apply_url); if (body_s) free(body_s);
```

(f) Add `prov_apply`, `prov_is_available`, `prov_has_pending` (near `prov_set_async`):

```c
static void prov_apply(pp_settings_done_cb cb, void *ud) {
    pthread_mutex_lock(&G.mu);
    if (G.queue_n >= FPVD_QUEUE_CAP) {
        pthread_mutex_unlock(&G.mu);
        schedule_done(cb, ud, -1, "Settings queue full");
        return;
    }
    fpvd_job_t *j = &G.queue[G.queue_n++];
    memset(j, 0, sizeof *j);
    j->endpoint   = FPVD_EP_CONFIG;
    j->apply_only = true;
    j->on_done    = cb;
    j->user_data  = ud;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}

static bool prov_is_available(const char *d, const char *p, const char *k) {
    return fpvd_keymap_lookup(d, p, k) != NULL;
}

static bool prov_has_pending(void) {
    pthread_mutex_lock(&G.mu);
    bool dirty = G.config_dirty;
    pthread_mutex_unlock(&G.mu);
    return dirty;
}
```

(g) Register them in `G_PROVIDER`:

```c
    .is_available           = prov_is_available,
    .apply                  = prov_apply,
    .has_pending            = prov_has_pending,
```

- [ ] **Step 4: Run test to verify it passes** — `cmake --build build-test --target fpvd_tests -j 2>&1 | tail -3 && ./build-test/fpvd_tests "[config]"` → pass. Then run the whole suite `./build-test/fpvd_tests` → all green (the existing AIR/LINK integration tests still pass — they still PATCH+apply).

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp
git commit -m "feat(gsmenu): EP_CONFIG stages via PATCH /config; apply-only job + is_available"
```

---

### Task 5: `PP_ROW_LOCKED_UNAVAILABLE` state + greying

**Files:**
- Modify: `src/gsmenu/widgets/pp_row.h` (enum), `src/gsmenu/widgets/pp_row.c` (icon + set_locked)
- Modify: `src/gsmenu/helper.c` (row-walk), `src/gsmenu/widgets/pp_toggle.c`, `pp_dropdown.c`, `pp_slider.c` (build-time check)
- Verify: sim/device compile

No unit harness exercises LVGL widgets; this task is **compile-verified** (the behavior is mechanical greying mirrored from the existing lock states).

- [ ] **Step 1: Add the enum value** — in `src/gsmenu/widgets/pp_row.h`, extend `pp_row_lock_t`:

```c
typedef enum {
    PP_ROW_UNLOCKED = 0,
    PP_ROW_LOCKED_DYNAMIC = 1,
    PP_ROW_LOCKED_OFFLINE = 2,
    PP_ROW_LOCKED_UNAVAILABLE = 3,
} pp_row_lock_t;
```

- [ ] **Step 2: Render it** — in `src/gsmenu/widgets/pp_row.c`, add an icon define near the others (line ~90):

```c
#define PP_ROW_LOCK_ICON_UNAVAILABLE LV_SYMBOL_MINUS
```

and extend the icon selection inside `pp_row_set_locked` (the `else` branch that sets `lv_label_set_text`):

```c
        lv_label_set_text(lbl,
            state == PP_ROW_LOCKED_OFFLINE     ? PP_ROW_LOCK_ICON_OFFLINE :
            state == PP_ROW_LOCKED_UNAVAILABLE ? PP_ROW_LOCK_ICON_UNAVAILABLE :
                                                 PP_ROW_LOCK_ICON_DYNAMIC);
```

- [ ] **Step 3: Walk + build checks** — in `src/gsmenu/helper.c` `pp_page_reapply_lock_state`, add availability as the highest-priority state:

```c
        if (!pp_settings_is_available(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_UNAVAILABLE);
        } else if (!connected) {
            pp_row_set_locked(c, PP_ROW_LOCKED_OFFLINE);
        } else if (pp_settings_is_locked(h->d, h->p, h->k)) {
            pp_row_set_locked(c, PP_ROW_LOCKED_DYNAMIC);
        } else {
            pp_row_set_locked(c, PP_ROW_UNLOCKED);
        }
```

In each of `pp_toggle.c`, `pp_dropdown.c`, `pp_slider.c`, replace the build-time lock check:

```c
    if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }
```

with:

```c
    if (!pp_settings_is_available(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_UNAVAILABLE);
    } else if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }
```

- [ ] **Step 4: Verify it compiles** — `./sim.sh` (build the simulator; Ctrl-C once it launches). Expected: clean build, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_row.h src/gsmenu/widgets/pp_row.c src/gsmenu/helper.c \
        src/gsmenu/widgets/pp_toggle.c src/gsmenu/widgets/pp_dropdown.c src/gsmenu/widgets/pp_slider.c
git commit -m "feat(gsmenu): grey rows with no provider route (unavailable state)"
```

---

### Task 6: Consolidated `pixelpilot.c` page + Apply button

**Files:**
- Create: `src/gsmenu/pages/pixelpilot.c`, `src/gsmenu/pages/pixelpilot.h`
- Verify: sim/device compile

- [ ] **Step 1: Create the header** — `src/gsmenu/pages/pixelpilot.h`:

```c
#ifndef PP_PAGE_PIXELPILOT_H
#define PP_PAGE_PIXELPILOT_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *build_pixelpilot_tab(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Create the page** — `src/gsmenu/pages/pixelpilot.c` (merges Display + DVR, adds Screen Mode / RTP Jitter, the reenc-resolution + video-scale tweaks, and the Apply row):

```c
#include "pixelpilot.h"
#include "../widgets/pp_page.h"
#include "../widgets/pp_section_header.h"
#include "../widgets/pp_row.h"
#include "../widgets/pp_toggle.h"
#include "../widgets/pp_slider.h"
#include "../widgets/pp_dropdown.h"
#include "../widgets/pp_drilldown.h"
#include "../widgets/pp_toast.h"
#include "../settings.h"

/* ---- DVR playback drilldown (read-only stub, unchanged from dvr.c) ---- */
static void build_playback_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_section_header(body, "Recordings");
    pp_row_text(body, LV_SYMBOL_VIDEO, "(no recordings — stub backend)", NULL);
}
static void on_open_playback(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "Playback", build_playback_drilldown, NULL);
}

/* ---- Apply button: POST /apply (one pixelpilot restart for the batch) ----
 * (A live "dirty" marker is deferred: the snapshot-listener API has no
 * per-listener removal, so a page-scoped listener can't be torn down safely.
 * The backend exposes pp_settings_has_pending() for a future authoritative
 * indicator.) */
static void apply_done_cb(int rc, const char *err, void *user_data) {
    lv_obj_t *row = (lv_obj_t *)user_data;
    pp_row_set_busy(row, false);
    if (rc != 0) pp_toast_error(err ? err : "Apply failed");
}
static void on_apply(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row = lv_event_get_target(e);
    pp_row_set_busy(row, true);
    pp_settings_apply(apply_done_cb, row);
}

lv_obj_t *build_pixelpilot_tab(lv_obj_t *parent) {
    lv_obj_t *page = pp_page_create(parent, "gs", "pixelpilot");

    pp_section_header(page, "Display");
    pp_dropdown(page, LV_SYMBOL_EYE_OPEN, "Screen Mode",
                "gs", "display", "screen_mode",
                "1920x1080@60\n1920x1080@120\n1280x720@60\n1280x720@120\n"
                "2560x1440@60\n3840x2160@60");
    pp_slider(page, LV_SYMBOL_IMAGE, "Video Scale",
              "gs", "display", "video_scale", 50, 100);
    pp_slider(page, LV_SYMBOL_SETTINGS, "RTP Jitter (ms)",
              "gs", "display", "rtp_jitter_ms", 0, 50);
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Color correction",
              "gs", "display", "color_correction");
    pp_slider(page, LV_SYMBOL_SETTINGS, "Gain",
              "gs", "display", "cc_gain", 0, 50);
    pp_slider(page, LV_SYMBOL_SETTINGS, "Offset",
              "gs", "display", "cc_offset", -50, 50);

    pp_section_header(page, "DVR · Recording");
    pp_toggle(page, LV_SYMBOL_VIDEO, "Enabled",
              "gs", "dvr", "rec_enabled");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Mode",
                "gs", "dvr", "dvr_mode", "raw\nreencode");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Raw FPS",
                "gs", "dvr", "rec_fps", "30\n60\n90\n120");
    pp_slider(page, LV_SYMBOL_SD_CARD, "Max file size (MB)",
              "gs", "dvr", "dvr_max_size", 100, 16000);

    pp_section_header(page, "DVR · Re-encode");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Codec",
                "gs", "dvr", "dvr_reenc_codec", "h264\nh265");
    pp_dropdown(page, LV_SYMBOL_VIDEO, "Resolution",
                "gs", "dvr", "dvr_reenc_resolution", "1080p\n720p");
    pp_dropdown(page, LV_SYMBOL_REFRESH, "Re-encode FPS",
                "gs", "dvr", "dvr_reenc_fps", "30\n60");
    pp_dropdown(page, LV_SYMBOL_AUDIO, "Bitrate (kbps)",
                "gs", "dvr", "dvr_reenc_bitrate",
                "4000\n8000\n12000\n16000\n25000");

    pp_section_header(page, "DVR · Overlay");
    pp_toggle(page, LV_SYMBOL_EYE_OPEN, "Burn OSD into DVR",
              "gs", "dvr", "dvr_osd");

    pp_section_header(page, "Playback");
    lv_obj_t *pb_row = pp_row_text(page, LV_SYMBOL_PLAY, "Browse recordings…", NULL);
    lv_obj_add_event_cb(pb_row, on_open_playback, LV_EVENT_KEY, NULL);

    pp_section_header(page, "Apply");
    lv_obj_t *apply_row = pp_row_text(page, LV_SYMBOL_OK, "Apply changes", NULL);
    lv_obj_add_event_cb(apply_row, on_apply, LV_EVENT_KEY, NULL);

    /* Add focusable rows to the page's group (section headers are filtered). */
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

- [ ] **Step 3: (no test)** — page is exercised by the sim build in Task 7.

- [ ] **Step 4: (deferred)** — compiled in Task 7 once it's wired into CMake + menu.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/pages/pixelpilot.c src/gsmenu/pages/pixelpilot.h
git commit -m "feat(gsmenu): consolidated PixelPilot page (Display+DVR, Screen Mode, RTP Jitter, Apply)"
```

---

### Task 7: Wire the PixelPilot tab; delete display/dvr; CMake

**Files:**
- Modify: `src/menu.c`
- Modify: `CMakeLists.txt` (both gsmenu source lists)
- Delete: `src/gsmenu/pages/display.c`, `display.h`, `src/gsmenu/pages/dvr.c`, `dvr.h`
- Verify: sim/device compile

- [ ] **Step 1: Update `src/menu.c`** — replace the `#include` of display/dvr headers with pixelpilot, and the two tab builds + tabbar items.

Replace includes (top of file) for `pages/display.h` and `pages/dvr.h` with:

```c
#include "gsmenu/pages/pixelpilot.h"
```

Replace the `dsp`/`dvr` build blocks (lines ~73-78):

```c
    lv_obj_t *dsp = build_display_tab(root);
    lv_obj_set_flex_grow(dsp, 1);
    lv_obj_set_height(dsp, LV_PCT(100));
    lv_obj_t *dvr = build_dvr_tab(root);
    lv_obj_set_flex_grow(dvr, 1);
    lv_obj_set_height(dvr, LV_PCT(100));
```

with one:

```c
    lv_obj_t *pp = build_pixelpilot_tab(root);
    lv_obj_set_flex_grow(pp, 1);
    lv_obj_set_height(pp, LV_PCT(100));
```

Replace the tabbar items array + count (6 → 5):

```c
    pp_tabbar_item_t items[5] = {
        { "Camera",     LV_SYMBOL_IMAGE,     cam },
        { "Link",       LV_SYMBOL_WIFI,      lnk },
        { "DLink",      LV_SYMBOL_LOOP,      dl  },
        { "PixelPilot", LV_SYMBOL_VIDEO,     pp  },
        { "System",     LV_SYMBOL_SETTINGS,  sys },
    };
    pp_tabbar_t *tabbar = pp_tabbar_create(root, items, 5);
```

Then check the rest of `menu.c` for any other reference to `dsp`/`dvr` (e.g. the back-group wiring loop after line ~99) and replace those two with the single `pp`.

- [ ] **Step 2: Update `CMakeLists.txt`** — in **both** gsmenu source lists (around lines 80-89 and the second list near 135+), remove the four lines:

```cmake
        src/gsmenu/pages/display.h
        src/gsmenu/pages/display.c
        ...
        src/gsmenu/pages/dvr.h
        src/gsmenu/pages/dvr.c
```

and add:

```cmake
        src/gsmenu/pages/pixelpilot.h
        src/gsmenu/pages/pixelpilot.c
```

- [ ] **Step 3: Delete the old pages**

```bash
git rm src/gsmenu/pages/display.c src/gsmenu/pages/display.h \
       src/gsmenu/pages/dvr.c src/gsmenu/pages/dvr.h
```

- [ ] **Step 4: Verify it compiles** — `./sim.sh` (build; Ctrl-C once launched). Expected: clean build; the menu shows 5 tabs with **PixelPilot** between DLink and System, sectioned Display + DVR; color-correction / DVR Enabled rows greyed.

- [ ] **Step 5: Commit**

```bash
git add src/menu.c CMakeLists.txt
git commit -m "feat(gsmenu): one PixelPilot tab replaces Display+DVR (tabbar 6->5)"
```

---

### Task 8: Dummy provider (simulator) — seeds + caps

**Files:**
- Modify: `src/gsmenu/settings_dummy.c`
- Verify: sim runs with all PixelPilot rows live

The dummy backs the simulator. It must seed the new keys so the rows render values, and implement `is_available` (always true in sim) + `apply`/`has_pending` so the Apply button works without errors.

- [ ] **Step 1: Seed the new keys** — in `src/gsmenu/settings_dummy.c`, the seed table `g_seed[]` is keyed by bare key `{ "key", "value" }`. Add the two new keys in the `/* Display */` block (next to the existing `video_scale`):

```c
    { "screen_mode",      "1920x1080@60" },
    { "rtp_jitter_ms",    "5" },
```

And change the existing DVR resolution seed so it matches the new `1080p/720p` dropdown (the value must be one of the dropdown options or the row renders the em-dash placeholder):

```c
    { "dvr_reenc_resolution", "1080p" },   /* was "1920x1080" */
```

(The other DVR seeds — `rec_enabled`, `dvr_mode`, `rec_fps`, `dvr_max_size`, `dvr_reenc_codec`, `dvr_reenc_fps`, `dvr_reenc_bitrate`, `dvr_osd` — already exist; leave them.)

- [ ] **Step 2: Add the cap methods** — add dummy impls and register them in the dummy provider struct:

```c
static bool dummy_is_available(const char *d, const char *p, const char *k) {
    (void)d; (void)p; (void)k; return true;   /* sim: every row is live */
}
static void dummy_apply(pp_settings_done_cb cb, void *ud) {
    if (cb) cb(0, NULL, ud);                   /* sim: no-op success */
}
static bool dummy_has_pending(void) { return false; }
```

In the dummy provider struct add:

```c
    .is_available = dummy_is_available,
    .apply        = dummy_apply,
    .has_pending  = dummy_has_pending,
```

- [ ] **Step 3: (no unit test)** — verified by the sim.

- [ ] **Step 4: Verify** — `./sim.sh`; open the PixelPilot tab: every backed row shows a seeded value, color-correction / DVR Enabled are greyed, and "Apply changes" toasts nothing / clears cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/settings_dummy.c
git commit -m "feat(gsmenu): dummy seeds pixelpilot keys + is_available/apply for sim"
```

---

### Task 9: Full verification

- [ ] **Step 1: Run the whole fpvd + settings test suites**

Run: `cmake --build build-test --target fpvd_tests settings_tests -j 2>&1 | tail -3 && ./build-test/fpvd_tests && ./build-test/settings_tests`
Expected: all pass (keymap, path helpers, stage/apply integration, provider caps).

- [ ] **Step 2: Sim smoke** — `./sim.sh`; confirm the 5-tab layout, PixelPilot sections, greyed unavailable rows, dropdown values (`1080p`/`720p`, screen modes), Video Scale capped at 100, and the Apply row present at the bottom.

- [ ] **Step 3: Final commit (if any tweaks)**

```bash
git add -A && git commit -m "chore(gsmenu): finalize pixelpilot config integration" --allow-empty
```

---

## Notes for the implementer

- **Order matters:** Tasks 1→4 are the backend (each is `fpvd_tests`/`settings_tests` green); Task 5 (widgets) and 6→8 (page/menu/dummy) are compile-verified via `./sim.sh`. Do them in order — Task 7 is the first point where `pixelpilot.c` actually compiles.
- **On-device manual check (after merge + a PixelPilot_rk build is deployed):** open PixelPilot tab → change Mode + Bitrate + Video Scale → rows show staged values (read from pending) → press **Apply changes** → exactly one pixelpilot restart (one ~2 s blackout), all changes live; `pidof wfb_rx wfb_tx` unchanged (radio untouched). Validation errors surface on the Apply toast.
- **Don't** route color-correction / DVR Enabled / hotspot / restream / telemetry — they stay greyed by the general `is_available` rule until they get keymap entries.
- A live "dirty" marker on the Apply row is **deferred** (the snapshot-listener API has no per-listener removal, so a page-scoped listener isn't lifecycle-safe). The backend still exposes `pp_settings_has_pending()` so a future authoritative indicator can be added once the listener API supports removal.
```
