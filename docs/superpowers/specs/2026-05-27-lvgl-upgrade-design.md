# LVGL upgrade: v9.2.2 → v9.5.0

**Date:** 2026-05-27
**Branch:** `feat/refactor_ui`
**Status:** Approved (spec). Implementation plan TBD.

## Context

PixelPilot_rk pins LVGL at v9.2.2 (Oct 2024) as a git submodule. Upstream is at v9.5.0 (Feb 2026) — three minor releases ahead. We use LVGL for two surfaces: the on-screen OSD (bandwidth/latency/FPS readouts) and the GSMenu (ground-station settings UI). Rendering goes through a custom path — `osd.cpp:2109` creates a display via `lv_display_create(width, height)` against an in-memory buffer that is then blended with live video through the cairo/GLES2 pipeline. Input on real hardware is GPIO buttons routed through a custom KEYPAD indev (`input.cpp:602`); the SDL simulator uses LVGL's built-in SDL keyboard driver patched locally to filter indevs by `read_cb`.

41 source files reference LVGL, ~159 unique `lv_*` symbols. One local patch (`patches/lvgl-sdl-keyboard.patch`) auto-applied by CMake. `lv_conf.h` was templated from a `v9.3.0-dev` header at some prior point but is otherwise current.

## Goal

Land the version bump, fix everything the bump breaks, and adopt the four v9.3–v9.5 features that actually fit this codebase. One PR, one branch.

## Scope

### In scope

1. **Submodule bump** to LVGL v9.5.0.
2. **`lv_conf.h` reconciliation** — start fresh from the v9.5.0 template, re-apply our deltas (color depth 32, `LV_STDLIB_CLIB` malloc/string/sprintf, widget enable list, logging).
3. **Patch refresh** — regenerate `patches/lvgl-sdl-keyboard.patch` against the v9.5.0 source tree; keep CMake's auto-apply flow.
4. **Compile + runtime fixes** for whatever the v9.5.0 API changes touch across `src/`.
5. **Property interface migration** — refactor the four custom GSMenu widgets (`src/gsmenu/widgets/pp_row.c`, `pp_dropdown.c`, `pp_tabbar.c`, `pp_drilldown.h`) to the v9.5 property API where it shortens or clarifies the code. Skip places where the imperative form is already terser.
6. **Native blur / drop-shadow** on the GSMenu background container only. Not on the OSD (overlay perf budget on live video).
7. **`LV_STATE_ALT` scaffolding** — add the style handler and a stubbed toggle in `src/gsmenu/styles.c`. No user-facing day/night theme in this PR; just leave the seam clean.
8. **`lv_conf.h` perf toggles** — enable ARM NEON SW rendering, tiled rendering, triple buffer.

### Out of scope

- Wayland rewrite, DRM+EGL driver, NanoVG GPU backend, GStreamer widget, multi-touch gestures, RISC-V SIMD, glTF, WebP decoder. None of these apply: rendering uses a custom in-memory display, input is GPIO, and the project has no 3D/touch surface.
- Refactoring `osd.cpp` / `lvosd.c` beyond what compile errors force.
- Changes to the cairo/GLES2 composition, color-correction pipeline, DVR re-encode, or frame pacer.
- A full day/night theme — only the `LV_STATE_ALT` plumbing.
- Upstreaming the SDL keyboard fix to lvgl/lvgl (separate effort, not blocking).

## Workstream sequencing (commits within the single PR)

Each step ends in a buildable state so bisection still works.

1. **Bump submodule + refresh patch.** Point `lvgl/` at `v9.5.0`; regenerate `patches/lvgl-sdl-keyboard.patch`; verify CMake's `git apply --check --reverse` logic still passes. Commit: `chore(lvgl): bump submodule to v9.5.0 and refresh sdl keyboard patch`.
2. **Reconcile `lv_conf.h`.** Copy `lvgl/lv_conf_template.h` from v9.5.0; re-apply our deltas. Leave the four perf toggles **off** at this step — pure behavior preservation. Commit: `chore(lvgl): reconcile lv_conf.h with v9.5.0 template`.
3. **Fix compile breakages.** Drive simulator build (`./sim.sh`) first, then cross-compile. One commit per logical fix.
4. **Fix runtime breakages in simulator.** Exercise OSD + every GSMenu page; fix anything that compiles but behaves wrong. One commit per fix.
5. **Enable perf toggles.** Flip NEON, tiled rendering, triple buffer on in `lv_conf.h`. Re-verify in simulator. Commit: `perf(lvgl): enable NEON SW render, tiled rendering, triple buffer`.
6. **Property API migration.** Refactor `pp_row.c`, `pp_dropdown.c`, `pp_tabbar.c`, `pp_drilldown.h` one at a time; simulator re-check between each. One commit per widget.
7. **Native blur on GSMenu background.** Apply blur/drop-shadow to the menu container; verify perf is still acceptable in simulator. Commit: `feat(gsmenu): native blur on menu background`.
8. **`LV_STATE_ALT` scaffolding.** Add the alt-state handler in `styles.c` plus a feature-flagged stub call site. Commit: `feat(gsmenu): scaffold LV_STATE_ALT for future day/night theme`.

Discipline: do not advance to step N+1 until step N builds clean (or runs clean, for steps 4+). If a step turns out to require a bigger refactor than expected, stop and check with the user rather than expanding scope silently.

## Risk map

| # | Risk | Probability | Mitigation |
|---|------|-------------|------------|
| 1 | `lv_display` / display-buffer API drift across 9.3→9.5 affecting `osd.cpp:2109` | High | Read the v9.5 `lv_display.h` in full before editing `osd.cpp`; mirror the v9.5 demo example pattern; verify each call signature. |
| 2 | `lv_indev` callback signatures changing under the SDL keyboard patch (`lv_indev_get_read_cb`) | High | Patch refresh is step 1 specifically so we hit this before anything else. |
| 3 | Custom widgets (`pp_*`) poking style/state internals that changed | Medium | Fix-on-failure during step 3; don't try to predict. |
| 4 | Tiled rendering interacts badly with our in-memory display flush path | Medium | Enable in step 5 with simulator measurement; if it regresses, leave off and document why. |
| 5 | NEON enablement breaks on the Rockchip cross-compile toolchain | Low-medium | Verify CMake passes the right flags; gate NEON behind `if(NOT USE_SIMULATOR)` and the cross toolchain if needed. |
| 6 | Triple-buffer memory cost (~24MB at 1080p XRGB8888) tight on RK3566 | Low | Measure heap usage in simulator; fall back to double-buffer if borderline. |
| 7 | CPU blur expensive on Rockchip ARM | Low | Limit to the menu container; if still expensive, gate via `lv_conf.h`. |
| 8 | Property API not ready for custom widgets without extra glue | Low | If glue is non-trivial for `pp_*` widgets, drop step 6 from the PR and file a follow-up — explicitly check with the user first. |

**Rollback story:** Every step is a separate commit on `feat/refactor_ui`. If hardware testing reveals a regression that can't be reproduced in simulator, revert to the last green commit and ship that as the PR while debugging the rest separately.

## Validation plan

### Simulator pass (Claude drives)

Build via `./sim.sh`. For each step that changes runtime behavior, exercise:

- **OSD path:** confirm bandwidth/latency/FPS readouts render against the simulator's video source; no flicker, no z-order issues.
- **GSMenu pages, full sweep:** open menu, navigate every page in `src/gsmenu/pages/` (`display`, `link`, `dvr`, plus any others wired in). For each: open, navigate top-to-bottom via the keyboard indev, edit one value, back out.
- **Custom widgets:** specifically poke each `pp_*` widget — dropdown expand/collapse, drilldown enter/exit, tabbar switch, row edit mode.
- **Input edge case for the patch:** verify the SDL keyboard patch's behavior still holds (menu-active vs menu-inactive input routing; multiple KEYPAD indevs in flight).

Capture before/after screenshots of the GSMenu when blur lands (step 7).

### Cross-compile pass (Claude drives)

After each step, run the real-hardware cross-compile. Compile-only — no execution.

### Device pass (user drives, end of PR)

User flashes on RK3566 and/or RK3588s and confirms:

- OSD overlays correctly on real video (cairo/GLES2 composition still works).
- GSMenu opens, every page navigable with GPIO buttons (custom KEYPAD indev still routes correctly).
- DVR re-encode still includes the OSD overlay.
- Frame pacer / latency feel unchanged.

If any check fails: user reports observation, Claude diagnoses. If unreproducible in simulator, triage — ship the green subset, follow-up the rest.

### Evidence discipline

Per `verification-before-completion`: every "this works" claim in the PR description must be tied to a sim screenshot, a successful build log, or user device confirmation. No "should work" handwaves.

## Definition of done

- `lvgl` submodule pinned at the v9.5.0 tag.
- `lv_conf.h` reconciled from the v9.5.0 template with our deltas applied; perf toggles (NEON, tiled rendering, triple buffer) enabled.
- `patches/lvgl-sdl-keyboard.patch` refreshed; CMake apply/reverse-check passes.
- `./sim.sh` builds and runs; OSD + every GSMenu page navigable with no regressions Claude can observe in the simulator.
- Real-hardware cross-compile succeeds.
- User confirms hardware behavior on at least one of RK3566 / RK3588s.
- The four modernization items landed: property API in `pp_*` widgets, blur on GSMenu background, `LV_STATE_ALT` scaffold, perf toggles.
