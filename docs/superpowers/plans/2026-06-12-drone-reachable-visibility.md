# Drone-Reachable Visibility Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make drone-config rows in the settings menu unlock within seconds of the drone coming online, instead of 30-60s, by wiring the never-called visibility API to menu open/close and refreshing immediately on the hidden→visible transition.

**Architecture:** Two small changes per the spec (`docs/superpowers/specs/2026-06-12-drone-reachable-visibility-design.md`). (1) The fpvd settings worker (`src/gsmenu/settings_fpvd.c`) gets a `refresh_now` flag set by `prov_set_visibility()` on the hidden→visible transition; the worker's idle loop checks it before and after waiting, so the menu opening triggers an immediate `/air`+`/gs` snapshot refresh. (2) The two menu open/close sites (`toggle_screen()` in `src/input.cpp`, `on_tabbar_cancel()` in `src/menu.c`) call `pp_settings_set_visibility()`, which until now had zero production callers — leaving the poll cadence stuck at the 60s "hidden" interval forever.

**Tech Stack:** C (pthread worker), Catch2 v3 integration test with cpp-httplib mock fpvd-GS server, host sim build via `nix-shell shell-sim.nix` + `build-sim/` (Release; see GOTCHA 2 in the build notes — Debug+ASAN breaks C-source test exes).

**Build/test commands (run from repo root `/home/gilankpam/Projects/drone/PixelPilot_rk`):**
- Build tests: `nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j'`
- Run suite: `nix-shell shell-sim.nix --run './build-sim/fpvd_tests "[network]"'`
- Compile-check UI: `nix-shell shell-sim.nix --run 'cmake --build build-sim --target pixelpilot -j'`

---

### Task 1: `refresh_now` — immediate refresh on hidden→visible

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (struct `fpvd_state_t` ~line 32, `worker_main` ~line 781, `prov_set_visibility` ~line 976, `pp_settings_register_fpvd` ~line 1020)
- Test: `tests/test_settings_fpvd_integration.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_settings_fpvd_integration.cpp` (after the last `TEST_CASE`). The mock's `air_get_override` is a `std::atomic<int>` read per-request, so flipping it mid-test takes effect on the next probe. After `install_provider_pointing_at()` the worker is idle in its 60s "hidden" wait (`G.visible` starts false), and a bare `pp_settings_set_visibility(true)` without the feature merely restarts the wait at the 3s visible cadence — so observing `is_reachable` flip within 2s discriminates the immediate refresh from the 3s tick.

```cpp
TEST_CASE("integration: hidden->visible triggers an immediate refresh", "[fpvd][network]") {
    GsMockServer srv;
    srv.air_get_override = 503;            /* drone down at registration */
    srv.start();
    install_provider_pointing_at(srv.port);
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);

    srv.air_get_override = 0;              /* drone comes back up */
    pp_settings_set_visibility(false);     /* ensure a clean hidden state */
    pp_settings_set_visibility(true);      /* menu opens -> immediate probe */

    /* Must flip well before the 3s visible tick (and the 60s hidden one). */
    bool reachable = false;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!reachable && std::chrono::steady_clock::now() < end) {
        reachable = pp_settings_is_reachable("air", "camera", "fps");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(reachable == true);

    pp_settings_set_visibility(false);     /* don't leave 3s polling running */
    srv.stop();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j' \
&& nix-shell shell-sim.nix --run './build-sim/fpvd_tests "integration: hidden->visible triggers an immediate refresh"'
```

Expected: FAIL on `REQUIRE(reachable == true)` after the 2s poll window (the worker stays in its 60s hidden wait; the visibility signal only restarts the wait).

- [ ] **Step 3: Implement `refresh_now`**

Four edits in `src/gsmenu/settings_fpvd.c`:

(a) Add the field to `fpvd_state_t`, next to `visible` (~line 38):

```c
    bool     stop;
    bool     visible;
    bool     refresh_now;         /* hidden→visible: probe now, don't wait a tick */
    bool     gs_connected;        /* fpvd-GS HTTP round-trips succeed */
```

(b) In `worker_main()`, check the flag at the top of the idle-wait loop — before waiting, so a flag set while the worker was busy running a job isn't missed once the CV signal is consumed; the post-wakeup loop iteration re-checks it for the signaled case. Replace the existing wait loop body (~lines 785-801):

```c
        while (!G.stop && G.queue_n == 0) {
            if (G.refresh_now) {
                G.refresh_now = false;
                bool notify = refresh_snapshot_unlocked();
                if (notify) {
                    pthread_mutex_unlock(&G.mu);
                    notify_listener();
                    pthread_mutex_lock(&G.mu);
                }
                continue;
            }
            int wait_ms = G.gs_connected ? (G.visible ? 3000 : 60000) : 2000;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int wr = pthread_cond_timedwait(&G.cv, &G.mu, &ts);
            if (wr == ETIMEDOUT) {
                bool notify = refresh_snapshot_unlocked();
                if (notify) {
                    pthread_mutex_unlock(&G.mu);
                    notify_listener();
                    pthread_mutex_lock(&G.mu);
                }
            }
        }
```

(Only the leading `if (G.refresh_now) { ... continue; }` block is new; the rest is unchanged.)

(c) Set the flag on the hidden→visible transition in `prov_set_visibility()` (~line 976):

```c
static void prov_set_visibility(bool v) {
    pthread_mutex_lock(&G.mu);
    if (v && !G.visible) G.refresh_now = true;
    G.visible = v;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
}
```

(d) Initialize it in `pp_settings_register_fpvd()`, next to the `G.visible` reset (~line 1020):

```c
    G.visible   = false;
    G.refresh_now = false;
```

- [ ] **Step 4: Run the test to verify it passes**

```
nix-shell shell-sim.nix --run 'cmake --build build-sim --target fpvd_tests -j' \
&& nix-shell shell-sim.nix --run './build-sim/fpvd_tests "integration: hidden->visible triggers an immediate refresh"'
```

Expected: PASS (the flip is observed within a few hundred ms — one local-mock HTTP round-trip).

- [ ] **Step 5: Run the full fpvd suite to check for regressions**

```
nix-shell shell-sim.nix --run './build-sim/fpvd_tests'
```

Expected: all tests pass. Note: the `[network]` tests spin real servers/threads and were historically order-sensitive — if an unrelated test fails, re-run before assuming this change broke it, then investigate.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/settings_fpvd.c tests/test_settings_fpvd_integration.cpp
git commit -m "feat(gsmenu): refresh fpvd snapshot immediately on hidden->visible"
```

---

### Task 2: Wire `pp_settings_set_visibility()` to menu open/close

`pp_settings_set_visibility()` (`src/gsmenu/settings.c:113`) currently has **no production callers** — only `tests/test_settings.cpp`. `G.visible` therefore stays false forever and the poll cadence is permanently 60s. The two (and only) sites that flip `menu_active` get the calls. The dispatcher null-checks the provider and the provider's `set_visibility` slot, so ordering vs. provider registration is safe, and the dummy/stub providers (which lack `set_visibility`) are unaffected.

**Files:**
- Modify: `src/input.cpp:443-450` (`toggle_screen`, the only `menu_active = true` site)
- Modify: `src/menu.c:30-36` (`on_tabbar_cancel`, the only `menu_active = false` site)

- [ ] **Step 1: Add the visibility call to menu open**

In `src/input.cpp`, add the include after the existing `#include "input.h"` (line 19) — `settings.h` carries its own `extern "C"` guards:

```cpp
#include "input.h"
#include "gsmenu/settings.h"
```

Then in `toggle_screen()` (line 443):

```cpp
extern "C" void toggle_screen(void) {
    if( ! menu_active ) {
        lv_scr_load(pp_menu_screen);
        lv_indev_set_group(indev_drv,main_group);
        lv_obj_invalidate(pp_menu_screen);
        menu_active = true;
        pp_settings_set_visibility(true);
    }
}
```

- [ ] **Step 2: Add the visibility call to menu close**

In `src/menu.c`, add the include after `#include "menu.h"` (line 6):

```c
#include "menu.h"
#include "gsmenu/settings.h"
```

Then in `on_tabbar_cancel()` (line 30):

```c
static void on_tabbar_cancel(lv_event_t *e) {
    (void)e;
    /* User pressed HOME while focus was on the tab strip — close the
     * menu and return to the OSD screen. */
    lv_screen_load(pp_osd_screen);
    menu_active = false;
    pp_settings_set_visibility(false);
}
```

- [ ] **Step 3: Compile-check the sim UI build**

`src/input.cpp` and `src/menu.c` build into the simulator GUI target (it can't run headless here — compile-clean is the gate for UI changes):

```
nix-shell shell-sim.nix --run 'cmake --build build-sim --target pixelpilot -j 2>&1 | tail -5'
```

Expected: build succeeds with no warnings about implicit declarations of `pp_settings_set_visibility`.

- [ ] **Step 4: Run the settings test suites for regressions**

```
nix-shell shell-sim.nix --run 'cmake --build build-sim --target settings_tests fpvd_tests -j' \
&& nix-shell shell-sim.nix --run './build-sim/settings_tests' \
&& nix-shell shell-sim.nix --run './build-sim/fpvd_tests'
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/input.cpp src/menu.c
git commit -m "fix(gsmenu): wire settings visibility to menu open/close

pp_settings_set_visibility was never called from production code, so the
fpvd worker always polled at the 60s hidden cadence — drone rows stayed
locked for up to a minute after the drone came online."
```

---

### Task 3: On-target verification (GS hardware)

No code changes — verifies the end-to-end behavior on real hardware. Cross-build and deploy per the GS workflow (temp `/tmp` deploy is fine for verification; see `reference_gs_deploy` memory: nix stdin-only cross-build, launch chain).

**Files:** none (deploy + observe)

- [ ] **Step 1: Cross-build the aarch64 `pixelpilot` and deploy to the GS (`root@10.18.0.1`) per the GS deploy workflow** (this also catches device-only link errors the sim can't, e.g. extern "C" linkage issues)

- [ ] **Step 2: Verify recovery latency**

With the drone (`root@192.168.10.152`) powered off: open the menu → drone rows show locked. Power the drone on. Expected: rows unlock within ~3-8s (3s visible tick + probe round-trip) — previously 30-60s.

- [ ] **Step 3: Verify menu-open refresh**

Close the menu, toggle the drone's state (off), wait ~10s, reopen the menu. Expected: rows re-lock within ~1-6s of opening (immediate probe fires; the failing probe takes up to its 5s curl timeout) rather than sitting stale for up to 60s.

- [ ] **Step 4: Report results** — if latencies match, the feature is done; revert the temp deploy or promote to `/usr` per the user's call.
