# Restart PixelPilot Action Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Restart PixelPilot" action row to the System tab. ENTER opens a confirm drilldown; Confirm runs `systemctl restart pixelpilot.service` via the gs_local provider; Cancel closes.

**Architecture:** New provider key `("gs","actions","restart_pixelpilot")` handled by `settings_gs_local`, routed via the router's GS-only rule. UI uses a `pp_drilldown` overlay with two rows (Confirm / Cancel) — no new widget.

**Tech Stack:** C11, LVGL, pthread (existing gs_local worker). No new third-party deps.

---

## File structure

**Modified files:**

| Path | Change |
|---|---|
| `src/gsmenu/settings_gs_local.c` | Add `GS_KEY_RESTART_PIXELPILOT` to enum + `resolve_key` + `run_job`. |
| `src/gsmenu/settings_router.c` | Add `("gs","actions","restart_pixelpilot")` to the `gs_only` rule. |
| `src/gsmenu/pages/system.c` | Add "Restart PixelPilot" row + confirm-drilldown handler. |
| `src/gsmenu/settings_dummy.c` | Seed a no-op for the new action so the simulator runs the confirm flow. |
| `tests/test_settings_router.cpp` | Add one test: action key routes to GS child only. |

No new files.

---

## Task 1: Wire the action through gs_local + router

**Files:**
- Modify: `src/gsmenu/settings_gs_local.c`
- Modify: `src/gsmenu/settings_router.c`
- Modify: `tests/test_settings_router.cpp`

- [ ] **Step 1: Add the router test case (failing)**

Open `tests/test_settings_router.cpp`. Add a new `TEST_CASE` after the existing "gs-only key hits only gs" case:

```cpp
TEST_CASE("router: gs-actions key routes to gs child only", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "actions", "restart_pixelpilot", "trigger", capture_done, &cap);
    REQUIRE(g_drone.sets.empty());
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<2>(g_gs.sets[0]) == "restart_pixelpilot");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}
```

- [ ] **Step 2: Run router tests — expect the new case to fail**

```bash
nix-shell shell-sim.nix --run 'cmake --build build_sim --target router_tests && ./build_sim/router_tests "[router]"'
```

Expected: the new test fails (gs child's `sets` is empty because the router currently routes `gs/actions/*` to drone, not gs).

- [ ] **Step 3: Extend the router's GS-only rule**

Edit `src/gsmenu/settings_router.c`, around line 130. Change:

```c
    bool gs_only =
        (strcmp(d, "gs") == 0 &&
         ((strcmp(p, "link")    == 0 && strcmp(k, "rx_power")  == 0) ||
          (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) ||
          (strcmp(p, "pp")      == 0 && strcmp(k, "codec")     == 0)));
```

to:

```c
    bool gs_only =
        (strcmp(d, "gs") == 0 &&
         ((strcmp(p, "link")    == 0 && strcmp(k, "rx_power")  == 0) ||
          (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) ||
          (strcmp(p, "pp")      == 0 && strcmp(k, "codec")     == 0) ||
          (strcmp(p, "actions") == 0)));
```

(All `gs/actions/*` keys are GS-only. Future actions added under this page will route correctly without further router changes.)

- [ ] **Step 4: Run router tests — expect pass**

```bash
nix-shell shell-sim.nix --run './build_sim/router_tests "[router]"'
```

Expected: all 8 tests pass.

- [ ] **Step 5: Extend the gs_local enum and resolver**

Edit `src/gsmenu/settings_gs_local.c`.

First, in the `gs_key_t` enum (around line 21-27), add the new entry before `GS_KEY_NONE`:

```c
typedef enum {
    GS_KEY_CHANNEL,
    GS_KEY_BANDWIDTH,
    GS_KEY_RXPOWER,
    GS_KEY_CODEC,
    GS_KEY_HDMI_MODE,
    GS_KEY_RESTART_PIXELPILOT,
    GS_KEY_NONE,
} gs_key_t;
```

Then in `resolve_key` (around line 89-97), add the new mapping. The full function becomes:

```c
static gs_key_t resolve_key(const char *d, const char *p, const char *k) {
    if (strcmp(d, "gs") == 0) {
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "gs_channel") == 0) return GS_KEY_CHANNEL;
        if (strcmp(p, "wfbng") == 0 && strcmp(k, "bandwidth")  == 0) return GS_KEY_BANDWIDTH;
        if (strcmp(p, "link")  == 0 && strcmp(k, "rx_power")   == 0) return GS_KEY_RXPOWER;
        if (strcmp(p, "pp")    == 0 && strcmp(k, "codec")      == 0) return GS_KEY_CODEC;
        if (strcmp(p, "display") == 0 && strcmp(k, "hdmi_mode") == 0) return GS_KEY_HDMI_MODE;
        if (strcmp(p, "actions") == 0 && strcmp(k, "restart_pixelpilot") == 0) return GS_KEY_RESTART_PIXELPILOT;
    }
    return GS_KEY_NONE;
}
```

- [ ] **Step 6: Extend `run_job` to handle the new key**

Still in `src/gsmenu/settings_gs_local.c`. Inside `run_job`'s switch (around line 148), add a new case BEFORE the `GS_KEY_NONE` case:

```c
    case GS_KEY_RESTART_PIXELPILOT: {
        int xst = run_systemctl_restart("pixelpilot.service");
        if (xst != 0) {
            r.rc = -1; r.err = strdup("pixelpilot restart failed");
        } else {
            r.rc = 0; r.err = NULL;
            /* In practice this process is dying right now; the listener and
             * on_done dispatches below may never reach the UI. That's fine. */
        }
        break;
    }
```

This branch uses the existing `run_systemctl_restart()` helper. It does NOT call any file writer and does NOT set `needs_restart` (which would re-run systemctl for `wifibroadcast.service`).

The trailing snapshot update at the bottom of `run_job` (the switch around line 211) does NOT need a `GS_KEY_RESTART_PIXELPILOT` case — there's no snapshot value to update. The existing `default: break;` in that switch covers it.

- [ ] **Step 7: Sanity-build everything that links gs_local**

```bash
nix-shell shell-sim.nix --run 'cmake --build build_sim --target pixelpilot router_tests'
```

Expected: clean build, no warnings about unhandled enum cases. `router_tests` still passes.

- [ ] **Step 8: Commit**

```bash
git add src/gsmenu/settings_gs_local.c src/gsmenu/settings_router.c tests/test_settings_router.cpp
git commit -m "$(cat <<'EOF'
feat(gsmenu): GS action — restart_pixelpilot via gs_local + router

New provider key (gs,actions,restart_pixelpilot) — execs systemctl restart
pixelpilot.service in the gs_local worker. Router gs-only rule generalized
to all gs/actions/* keys so future actions don't need a router edit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: System page row + confirm drilldown

**Files:**
- Modify: `src/gsmenu/pages/system.c`
- Modify: `src/gsmenu/settings_dummy.c`

- [ ] **Step 1: Add the dummy seed**

Edit `src/gsmenu/settings_dummy.c`. Find the `g_seed[]` array (starts around line 21). Add a new entry alongside the existing seeded values — the entry's `key` is the suffix the dummy provider looks up (it's keyed by `key` only, not `(domain,page,key)`):

```c
    { "restart_pixelpilot", "" },   /* no-op trigger in sim */
```

Insert it in the section that already groups GS-side keys (look at where `rx_power` and `hdmi_mode` were added in commit `ee29f14` — put this nearby).

- [ ] **Step 2: Add the action row + confirm handler in system.c**

Edit `src/gsmenu/pages/system.c`. Add two new static functions above `build_system_tab` (above the existing `on_action`, around line 34):

```c
static void on_restart_confirm_yes(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    pp_drilldown_close();
    pp_settings_set_async("gs", "actions", "restart_pixelpilot", "trigger",
                          action_done_cb, NULL);
}

static void on_restart_confirm_no(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    pp_drilldown_close();
}

static void build_restart_drilldown(lv_obj_t *body, void *user) {
    (void)user;
    pp_row_text(body, LV_SYMBOL_WARNING,
                "The menu will close while PixelPilot restarts.", NULL);
    lv_obj_t *yes = pp_row_text(body, LV_SYMBOL_OK,    "Confirm", NULL);
    lv_obj_t *no  = pp_row_text(body, LV_SYMBOL_CLOSE, "Cancel",  NULL);
    lv_obj_add_event_cb(yes, on_restart_confirm_yes, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(no,  on_restart_confirm_no,  LV_EVENT_KEY, NULL);
}

static void on_open_restart(lv_event_t *e) {
    if (lv_event_get_key(e) != LV_KEY_ENTER) return;
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *page = lv_obj_get_parent(row);
    pp_drilldown_open(page, "Restart PixelPilot?", build_restart_drilldown, NULL);
}
```

Then in `build_system_tab`, in the "Actions" section (after `pp_section_header(page, "Actions");` around line 69), add the new row BEFORE the existing "Reboot air" row so the action is the first item:

```c
    pp_section_header(page, "Actions");
    lv_obj_t *r;
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Restart PixelPilot", NULL);
    lv_obj_add_event_cb(r, on_open_restart, LV_EVENT_KEY, NULL);
    r = pp_row_text(page, LV_SYMBOL_REFRESH, "Reboot air", NULL);
    /* ... existing rows unchanged ... */
```

- [ ] **Step 3: Build the simulator**

```bash
nix-shell shell-sim.nix --run 'cmake --build build_sim --target pixelpilot'
```

Expected: clean build, `pixelpilot` binary updated.

- [ ] **Step 4: Smoke-run the simulator**

```bash
nix-shell shell-sim.nix --run './build_sim/pixelpilot &'
sleep 3
pkill -f 'build_sim/pixelpilot' || true
```

Expected: process starts, lives ~3 seconds, exits cleanly when killed. (If you can interact with the sim, navigate to System → Actions → Restart PixelPilot to verify the drilldown opens. In the dummy provider it just no-ops on Confirm.)

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/pages/system.c src/gsmenu/settings_dummy.c
git commit -m "$(cat <<'EOF'
feat(gsmenu/system): Restart PixelPilot row with confirm drilldown

ENTER opens a drilldown with Confirm/Cancel rows. Confirm fires the
(gs,actions,restart_pixelpilot) provider call; on device, gs_local
execs systemctl restart pixelpilot.service.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review notes

- **Spec coverage** — Design's four UI bullets map directly: gs_local key (Task 1 step 5-6), router rule (Task 1 step 3), system page row + drilldown (Task 2 step 2), dummy seed (Task 2 step 1). Router test covers routing; UI tested via sim smoke run.
- **No placeholders.**
- **Type consistency** — `GS_KEY_RESTART_PIXELPILOT` used in enum + resolver + run_job. Action key string `"restart_pixelpilot"` matches across system.c, gs_local, router test, dummy seed. Service name `"pixelpilot.service"` matches the systemd unit.
- **Why not test the drilldown?** It's pure LVGL composition (open/close + two key handlers). The router test covers routing; the dummy run covers integration. A modal-specific Catch2 test would need LVGL display setup that's heavier than the value it adds.
