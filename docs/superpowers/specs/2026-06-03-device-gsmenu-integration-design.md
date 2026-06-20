# Finish the `feat/refactor_ui` device build (minimal)

- **Date:** 2026-06-03
- **Status:** Approved (design)
- **Component:** PixelPilot_rk device runtime (`src/main.cpp`, `src/wfbcli.cpp`, `src/input.cpp`, `src/main.h`)
- **Branch:** `feat/refactor_ui`
- **Follows:** the fpvd settings-backend migration (`2026-06-03-pixelpilot-settings-via-fpvd-gs-design.md`)

## Problem

`feat/refactor_ui`'s **device** build (`USE_SIMULATOR=OFF`) does not compile. Commit `55f928d` ("purge old menu code") deleted the old `air_*`/`gs_*` page builders and their headers, but three consumers were left dangling:

- `src/main.cpp` (lines 63–65) and `src/wfbcli.cpp` (line 23) still `#include` purged headers (`gsmenu/gs_system.h`, `gsmenu/air_actions.h`, `gsmenu/gs_actions.h`) → fatal compile error.
- `src/main.cpp` defines `airactions[]`/`gsactions[]` (lines 144–147, using `MenuAction`/`MAX_ACTIONS` from those headers) and fills them from `config["gsmenu"]["actions"]` (~1466–1510) — but the **new** menu (`src/gsmenu/pages/system.c`) hardcodes its action rows and never reads these arrays.
- `toggle_rec_enabled()` is declared (`src/input.h:30`) and called by the rec button (`src/input.cpp:222,256`) but has **no definition** on this branch (its old definition lived in the purged `gsmenu/gs_system.c`) → link error.

This was invisible during the settings migration because the simulator/test builds compile `SIMULATOR_SOURCES`, which excludes `main.cpp` and `wfbcli.cpp`.

The GS (an OpenIPC SBC ground-station, **radxa_zero3**, aarch64) currently runs a binary built from a *different* branch (`feat/idr-burst-spacing-20ms`), which still has the old menu. To smoke-test the new fpvd settings backend on the GS we need a device-buildable binary from `feat/refactor_ui`.

## Goal

Make the `feat/refactor_ui` device build **compile and link**, cross-build it for radxa_zero3, and smoke-test the new fpvd settings menu on the GS.

## Non-goals

- Re-implementing the menu integration. It is **already wired**: `src/osd.cpp:2155-2164` calls `setup_lvgl()` → `pp_menu_main()` → `lv_task_handler()` in the OSD-thread loop when `gsmenu_enabled`. The `// TODO: put gsmenu main loop here` in `main_loop()` is a stale leftover.
- Restoring the config-driven custom-actions feature (`config["gsmenu"]["actions"]`). The new menu hardcodes its actions; this YAML feature is intentionally dropped.
- Wiring backends for GS-side menu rows that lost `gs_local` (menu DVR enable/mode, `system/actions/*` reboot/factory-reset, non-fpvd `gs/*` settings). Those rows stay **inert** for now; the fpvd rows (channel, width, rx-power, video, dynamic-link) function through the new provider.

## Decisions (resolved during brainstorming)

1. **Scope: minimal** — fix the build blockers and the rec-record button; accept inert GS-side rows. Fastest path to the fpvd smoke-test.
2. **Orphaned actions block: remove it** — delete the includes, the array definitions, and the parsing block together. Consistent with the new hardcoded-actions UI.

## Changes

### 1. `src/main.cpp` — excise the orphaned GS-actions machinery
- Delete the three purged-header includes (lines 63–65: `gs_system.h`, `air_actions.h`, `gs_actions.h`).
- Delete the array definitions `airactions[]`, `airactions_count`, `gsactions[]`, `gsactions_count` (lines 144–147).
- Delete the `config["gsmenu"]["actions"]` parsing block — the inner `if (gsmenu_enabled && config["gsmenu"]["actions"]) { … }` (~1466–1510) — while **keeping** the surrounding `if (config["gsmenu"]) { if (config["gsmenu"]["enabled"]) { gsmenu_enabled = …; } }`. Mind the nested braces.
- Remove the stale `// TODO: put gsmenu main loop here` comment in `main_loop()`.

After this, `main.cpp` references no purged symbol (`MenuAction`/`MAX_ACTIONS`/`airactions`/`gsactions` appear nowhere else — verified).

### 2. `src/wfbcli.cpp` — remove the dead include
- Delete `#include "gsmenu/gs_system.h"` (line 23). No symbol from it is used.

### 3. Implement `toggle_rec_enabled()`
- Add `extern "C"` declarations for the DVR control functions to `src/main.h` (they are defined `extern "C"` in `main.cpp:~650` but declared in no header):
  ```c
  extern "C" void dvr_start_all(void);
  extern "C" void dvr_stop_all(void);
  ```
- Define `toggle_rec_enabled()` in `src/input.cpp` (it is compiled in both device and simulator builds, and its callers live there). `dvr_enabled` is already reachable (`extern int dvr_enabled;` at `input.cpp:28`, declared in `dvr.h:55`):
  ```c
  void toggle_rec_enabled(void) {
  #ifndef USE_SIMULATOR
      if (dvr_enabled) dvr_stop_all();
      else             dvr_start_all();
  #endif
      /* Simulator: no-op. The sim rec path (input.cpp:254) already
         flips dvr_enabled, so doing nothing here avoids a double toggle. */
  }
  ```

### 4. Iterate the cross-build, then smoke-test
- Cross-build for radxa_zero3 from the local working tree via Buildroot source-override (no push/pin needed):
  ```bash
  cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam
  # this FHS env only runs commands fed on stdin (nix-shell --run no-ops):
  printf '%s\n' \
    'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
    'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
    'export DEFCONFIG=radxa_zero3_defconfig' \
    './build.sh pixelpilot-rebuild' \
    | nix-shell ./shell.nix
  ```
  Artifact: `output/radxa_zero3_defconfig/target/usr/bin/pixelpilot` (aarch64).
- If further purged-symbol references surface (the first failed build halted early), resolve each the same minimal way — delete a dead include, or stub/redirect a dangling symbol — and rebuild until it links cleanly.
- Deploy for smoke test (GS `/` is overlayfs rw, `/tmp` is tmpfs):
  ```bash
  scp output/radxa_zero3_defconfig/target/usr/bin/pixelpilot root@10.18.0.1:/tmp/pixelpilot
  ssh root@10.18.0.1 '/etc/init.d/S99pixelpilot stop; /tmp/pixelpilot ...'   # run with the same args/env as pixelpilot.sh
  ```
  (Non-persistent; the installed `/usr/bin/pixelpilot` is untouched. The GS `fpvd` daemon is already running, so the fpvd menu rows have a backend.)

## Verification

There is no clean unit-test seam for a "make it link" change, so verification is layered:

1. **Simulator + existing tests stay green** — the changes are device-guarded (`#ifndef USE_SIMULATOR`) plus a sim no-op, so they must not regress the sim build or the settings/unit suites:
   ```bash
   nix-shell shell-sim.nix --run "cmake --build build-test --target pixelpilot fpvd_tests gs_rxpower_tests gs_enum_tests settings_tests -j && \
     ./build-test/fpvd_tests && ./build-test/gs_rxpower_tests && ./build-test/gs_enum_tests && ./build-test/settings_tests"
   ```
2. **Device cross-build compiles + links** for radxa_zero3 (the primary objective).
3. **On-GS smoke test:** menu opens; fpvd rows (e.g. `air/camera/bitrate`, `gs/wfbng/gs_channel`) read live values from the GS fpvd and writes apply (observe via the GS fpvd `/status` or the value round-tripping in the menu). Inert rows (DVR enable, system actions) are expected to no-op.

## Known limitations / out of scope

- GS-side menu rows without an fpvd backend (menu DVR enable/mode, `system/actions/*`, non-fpvd `gs/*`) are inert until their backends are wired — a separate task.
- The config-driven `config["gsmenu"]["actions"]` feature is removed, not preserved.
- The smoke-test deploy is non-persistent (`/tmp`); a persistent install (overlay `/usr/bin` swap or a rootfs/sysupgrade reflash) and resolving the `feat/refactor_ui` vs `feat/idr-burst-spacing-20ms` branch divergence are separate follow-ups before a real release.
