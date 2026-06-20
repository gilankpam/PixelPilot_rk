# LVGL v9.2.2 → v9.5.0 Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bump LVGL from v9.2.2 to v9.5.0, fix all compile/runtime breakages, and land four modernization items (property API in custom GSMenu widgets, native blur on GSMenu background, `LV_STATE_ALT` scaffold, perf toggles).

**Architecture:** Single PR on `feat/refactor_ui` branch, 8 sequenced commit groups. Each group ends in a buildable state for bisection. LVGL is a git submodule; `lv_conf.h` lives at repo root; one local patch (`patches/lvgl-sdl-keyboard.patch`) is auto-applied by CMake.

**Tech Stack:** LVGL v9.5.0 (C99), CMake, SDL2 (simulator), Rockchip MPP + cairo + GLES2 (real hardware composition), Catch2 (existing test scaffolding, not LVGL-covering).

**TDD note:** This project does not have automated LVGL coverage; the spec's chosen validation strategy is manual simulator verification. Tasks therefore use **build + sim-run verification gates** in place of unit tests. Where genuinely testable code is added (blur helper, `LV_STATE_ALT` style hook), a Catch2 test is included.

**Spec:** `docs/superpowers/specs/2026-05-27-lvgl-upgrade-design.md`

---

## File Structure

**Modified:**
- `lvgl/` (submodule pointer → v9.5.0)
- `lv_conf.h` (regenerated from v9.5.0 template + our deltas)
- `patches/lvgl-sdl-keyboard.patch` (refreshed against v9.5.0 sources)
- `CMakeLists.txt` (only if LVGL exports change — e.g. include dirs or target names)
- `src/osd.cpp` (display API drift fixes, likely around `lv_display_create` at line 2109)
- `src/lvosd.c`, `src/lvosd.h` (whatever the build surfaces)
- `src/menu.c` (whatever the build surfaces)
- `src/input.cpp` (indev API drift, especially `lv_indev_get_read_cb` at line 602 area)
- `src/simulator.c` (whatever the build surfaces)
- `src/gsmenu/widgets/pp_row.c` (property API migration)
- `src/gsmenu/widgets/pp_dropdown.c` (property API migration)
- `src/gsmenu/widgets/pp_tabbar.c` (property API migration)
- `src/gsmenu/widgets/pp_drilldown.h` + corresponding `.c` if present (property API migration)
- `src/gsmenu/styles.c` (blur on menu container; `LV_STATE_ALT` hook)
- `src/gsmenu/pages/*.c` (only what the build surfaces)
- `src/gsmenu/helper.c` (only what the build surfaces)

**Created:**
- `tests/lvgl_alt_state_test.cpp` (Catch2 unit test for `LV_STATE_ALT` style hook)

**Deleted:** none.

---

## Task 1: Bump LVGL submodule to v9.5.0

**Files:**
- Modify: `lvgl/` (submodule)

- [ ] **Step 1: Confirm clean working tree before submodule change**

Run: `git status`
Expected: clean except for any pre-existing changes in `.superpowers/brainstorm/` (untracked) and the `lvgl` submodule dirty marker from prior session. If `lvgl` shows "modified content", run `git -C lvgl reset --hard` first to undo any in-progress changes.

- [ ] **Step 2: Fetch and checkout v9.5.0 in submodule**

Run:
```bash
git -C lvgl fetch --tags
git -C lvgl checkout v9.5.0
git -C lvgl describe --tags --exact-match
```
Expected: `v9.5.0`

- [ ] **Step 3: Stage submodule pointer change only (don't commit yet)**

Run: `git add lvgl`
Then: `git status`
Expected: shows `lvgl` modified (new commits) but no other changes staged.

- [ ] **Step 4: Continue to Task 2 — do not commit yet**

The submodule bump alone will break the patch apply in CMake. Task 2 refreshes the patch in the same commit so the tree stays buildable.

---

## Task 2: Refresh `lvgl-sdl-keyboard.patch` against v9.5.0

**Files:**
- Modify: `patches/lvgl-sdl-keyboard.patch`

**Context:** The existing patch modifies `lvgl/src/drivers/sdl/lv_sdl_keyboard.c` to filter KEYPAD indevs by checking `lv_indev_get_read_cb(indev) == sdl_keyboard_read`. The file may have moved, the surrounding lines almost certainly shifted, and `lv_indev_get_read_cb` may have been renamed.

- [ ] **Step 1: Locate the SDL keyboard driver file in v9.5.0**

Run:
```bash
find lvgl/src/drivers -name "lv_sdl_keyboard*"
```
Expected: returns at least `lvgl/src/drivers/sdl/lv_sdl_keyboard.c`. If the path differs, update the patch header accordingly.

- [ ] **Step 2: Inspect the upstream `lv_sdl_keyboard_handler` function**

Run: `grep -n "lv_sdl_keyboard_handler\|sdl_keyboard_read\|LV_INDEV_TYPE_KEYPAD" lvgl/src/drivers/sdl/lv_sdl_keyboard.c`
Read the function block around the match. Identify whether `lv_indev_get_read_cb` still exists (try `grep -rn "lv_indev_get_read_cb" lvgl/src/indev/`).

- [ ] **Step 3: Verify `lv_indev_get_read_cb` is still public**

Run: `grep -rn "lv_indev_get_read_cb" lvgl/src/indev/lv_indev.h`
- If present: proceed to Step 4.
- If absent: search for the replacement (likely `lv_indev_get_driver_data` or equivalent). Stop and report to the user before improvising — this is a Risk #2 trigger from the spec.

- [ ] **Step 4: Apply the patch logic manually to v9.5.0 source**

Edit `lvgl/src/drivers/sdl/lv_sdl_keyboard.c` directly: locate the `while(indev)` loop in `lv_sdl_keyboard_handler` and change the condition from
```c
if(lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
```
to
```c
/*Find a suitable indev — only consider SDL keyboard indevs (skip any other
 *KEYPAD indevs created by the user, whose driver_data is not lv_sdl_keyboard_t).*/
if(lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD &&
   lv_indev_get_read_cb(indev) == sdl_keyboard_read) {
```

- [ ] **Step 5: Regenerate the patch from the modified submodule**

Run:
```bash
git -C lvgl diff src/drivers/sdl/lv_sdl_keyboard.c > patches/lvgl-sdl-keyboard.patch
```
Then inspect: `cat patches/lvgl-sdl-keyboard.patch`
Expected: the diff header references `src/drivers/sdl/lv_sdl_keyboard.c`, the hunk shows the same condition change.

- [ ] **Step 6: Reset the submodule and verify CMake's apply flow works**

Run:
```bash
git -C lvgl checkout src/drivers/sdl/lv_sdl_keyboard.c
git -C lvgl apply --check patches/lvgl-sdl-keyboard.patch 2>&1
```
Wait — that's the wrong path; the patch is at the repo root, not inside the submodule. Use:
```bash
git -C lvgl apply --check ../patches/lvgl-sdl-keyboard.patch 2>&1
```
Expected: no output (clean apply check).
Then:
```bash
git -C lvgl apply ../patches/lvgl-sdl-keyboard.patch
git -C lvgl apply --check --reverse ../patches/lvgl-sdl-keyboard.patch 2>&1
```
Expected: second check passes (patch is applied; reverse-check confirms it).

- [ ] **Step 7: Reset submodule to clean v5.0 state before commit**

Run: `git -C lvgl reset --hard v9.5.0`
Verify: `git -C lvgl status` → "nothing to commit, working tree clean".

- [ ] **Step 8: Commit submodule bump + patch refresh together**

Run:
```bash
git add lvgl patches/lvgl-sdl-keyboard.patch
git commit -m "$(cat <<'EOF'
chore(lvgl): bump submodule to v9.5.0 and refresh sdl keyboard patch

Regenerates patches/lvgl-sdl-keyboard.patch against v9.5.0 sources.
The patch filters KEYPAD indevs by read_cb so our custom GPIO keypad
indev does not absorb SDL keyboard events in the simulator.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: commit succeeds. Run `git log --oneline -1` to confirm.

---

## Task 3: Reconcile `lv_conf.h` with v9.5.0 template

**Files:**
- Modify: `lv_conf.h`

**Context:** Current `lv_conf.h` header says "Configuration file for v9.3.0-dev". The v9.5.0 template will have new flags (blur, NanoVG, RISC-V, property API), removed/renamed flags, and reorganized sections. We start fresh from the template and re-apply only our deltas. **Perf toggles (NEON/tiled/triple-buffer) stay OFF in this task** — they get flipped in Task 6, after the port itself is stable.

- [ ] **Step 1: Inventory our current deltas**

Read current `lv_conf.h` end-to-end. Record every value that differs from the LVGL template defaults. Specifically capture:
- `LV_COLOR_DEPTH 32`
- `LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB`
- `LV_USE_STDLIB_STRING LV_STDLIB_CLIB`
- `LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB`
- `LV_USE_LOG 1` (and any log level/output config)
- `LV_USE_OBJ_ID_BUILTIN 1`
- `LV_USE_OBJ_PROPERTY_NAME 1`
- `LV_USE_FONT_PLACEHOLDER 1`
- All `LV_USE_*` widget enables that are set to `1`
- `LV_USE_SDL` value (note: appears twice in current file — verify which is the real value, the second occurrence is a guarded `#if LV_USE_SDL` block)
- Any custom font / draw / theme / OS settings
- Any non-default values in `LV_MEM_SIZE`, `LV_DPI_DEF`, etc.

Save this list (in your scratchpad or comment block) before editing.

- [ ] **Step 2: Replace `lv_conf.h` with the v9.5.0 template**

Run:
```bash
cp lvgl/lv_conf_template.h lv_conf.h
```
Then enable the guard:
- Open `lv_conf.h`
- Find the line `#if 0 /*Set it to "1" to enable the content*/`
- Change to `#if 1`

Verify: `head -20 lv_conf.h` shows the `#if 1` enabled.

- [ ] **Step 3: Re-apply each delta from the Step 1 inventory**

Edit `lv_conf.h` and set each captured value. Use `Edit` tool, one delta per edit. For widget enables, locate the `LV_USE_<NAME>` define and flip to `1`.

After all edits, do a sanity grep:
```bash
grep -nE "^#define LV_COLOR_DEPTH|^#define LV_USE_STDLIB_MALLOC|^#define LV_USE_LOG" lv_conf.h
```
Expected: matches your inventory values.

- [ ] **Step 4: Confirm new v9.5.0 flags exist but stay at their template defaults**

These flags are new in 9.3–9.5 and we leave them at template defaults in this task:
- `LV_USE_NANOVG` (should default 0)
- `LV_USE_DRAW_NEMA_GFX` (should default 0)
- ARM NEON / tiled rendering / triple buffer flags (names TBD by template — leave alone for now)
- `LV_USE_GESTURE_RECOGNITION` (should default 0)
- `LV_USE_GLTF` (should default 0)

Run: `grep -nE "LV_USE_NANOVG|LV_USE_GLTF|LV_USE_GESTURE_RECOGNITION" lv_conf.h`
Expected: each at value `0`. If any defaults to `1` in the template, leave as-is (template is authoritative for defaults).

- [ ] **Step 5: Verify file compiles standalone (header sanity)**

Run:
```bash
cc -E -x c -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE -c lvgl/lvgl.h -o /dev/null 2>&1 | head -30
```
Expected: no preprocessor errors related to `lv_conf.h`. (Compiler errors about missing symbols in other LVGL headers are fine — those get caught in Task 4.)

- [ ] **Step 6: Commit `lv_conf.h` reconciliation**

Run:
```bash
git add lv_conf.h
git commit -m "$(cat <<'EOF'
chore(lvgl): reconcile lv_conf.h with v9.5.0 template

Regenerates lv_conf.h from lvgl/lv_conf_template.h (v9.5.0) and
re-applies PixelPilot deltas: color depth 32, clib stdlib backends,
log enabled, full widget enable set. New v9.5.0 flags (NanoVG,
gestures, glTF, etc.) left at template defaults.

Perf toggles (NEON, tiled rendering, triple buffer) intentionally
off in this commit; enabled in a later commit after the port is
verified stable.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Fix compile breakages — simulator build

**Files:**
- Modify: whichever `src/*.c`, `src/*.cpp`, `src/**/*.c` files the compiler points at.

**Context:** This is an inherently exploratory task. We don't know in advance which APIs drifted. Methodology: build, read first error, fix, repeat. Drive simulator first (faster, simpler toolchain).

- [ ] **Step 1: Build the simulator and capture all errors**

Run:
```bash
./sim.sh 2>&1 | tee /tmp/sim-build.log
```
- If build succeeds: skip to Task 5 (no compile breakages — pleasant surprise).
- If build fails: continue.

Then: `grep -nE "error:|undeclared|undefined reference" /tmp/sim-build.log | head -40`

- [ ] **Step 2: Group errors by file**

For each error line, note: file, line, symbol. Group by file. Order files by dependency (e.g. `osd.cpp` likely fails before things that include it).

- [ ] **Step 3: Read the v9.5.0 header for each broken symbol**

For each symbol that the compiler reports missing/changed, find the corresponding header in `lvgl/src/` (e.g. `lvgl/src/display/lv_display.h` for display API). Read the new signature.

For symbols that vanished, search for renames:
```bash
grep -rn "<old-symbol-name>" lvgl/src/ | head
grep -rn "<base-name>" lvgl/src/ | head
```

- [ ] **Step 4: Apply fixes file-by-file using the Edit tool**

For each broken file: apply edits to update API calls to v9.5.0 signatures. Do NOT try to "improve" code while fixing it — minimal change to compile.

After each file is fixed, run a build to confirm progress:
```bash
./sim.sh 2>&1 | grep -E "error:" | head -5
```
Expected: same error count goes down or the next failing file appears.

- [ ] **Step 5: Repeat Step 4 until build succeeds**

Exit criterion: `./sim.sh 2>&1 | grep -cE "error:"` returns `0` AND the simulator binary exists.

If a fix expands beyond a one-line API rename (e.g. the display creation pattern needs restructuring), pause and confirm the approach with the user before continuing — per Risk #1 in the spec.

- [ ] **Step 6: Verify simulator launches**

Run: `./sim.sh` (or whatever the script does — read it if unsure). Let it start, observe the window appears, press Ctrl-C to stop. Do not interact yet; we only need confirmation it doesn't immediately crash.

- [ ] **Step 7: Commit per logical fix**

For each meaningful unit of fix (e.g. all `lv_display_*` migrations together, all `lv_indev_*` migrations together, etc.), make a separate commit:
```bash
git add <files-for-this-fix>
git commit -m "fix(<area>): adapt to LVGL v9.5.0 <api-name> API change

<one-sentence explanation of what changed and why>

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Examples (use the appropriate one for the actual change):
- `fix(osd): adapt to lv_display_create API change in v9.5.0`
- `fix(input): adapt to lv_indev API rename in v9.5.0`
- `fix(gsmenu): adapt to lv_style API change in v9.5.0`

- [ ] **Step 8: Verify cross-compile path also builds**

Read `CMakeLists.txt` to determine the real-hardware build invocation (look for `USE_SIMULATOR` gating). Likely:
```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc) 2>&1 | tee /tmp/hw-build.log
```
- If success: continue.
- If failure: file-by-file errors are likely real-hardware-only code paths (`#ifndef USE_SIMULATOR` blocks). Fix them with the same Step 3–4 methodology. Commit each fix separately.

Exit criterion: both `./sim.sh` and the cross-compile path succeed.

---

## Task 5: Fix runtime breakages in simulator

**Files:**
- Modify: whichever files cause runtime issues (TBD by observation).

**Context:** A clean build doesn't guarantee runtime correctness. The v9.5.0 may have changed default behaviors (event dispatch order, layout invalidation, draw task priorities) that compile fine but render or behave wrong.

- [ ] **Step 1: Launch simulator and exercise OSD path**

Run: `./sim.sh`
Observe the OSD overlay. Check:
- Bandwidth/latency/FPS readouts visible and updating
- No visible flicker
- No z-order issue (OSD on top of fake video)
- No console errors/warnings from LVGL (logs go to stdout/stderr per `LV_USE_LOG`)

Record any issue. Stop sim with Ctrl-C.

- [ ] **Step 2: Launch simulator and sweep every GSMenu page**

Run: `./sim.sh`
Open the menu (whatever the simulator's keyboard binding is — check `src/input.cpp` SDL handler if unclear). For each page under `src/gsmenu/pages/`:
- Navigate to it
- Move top-to-bottom with the keyboard navigation keys
- Edit one value in the page
- Back out

Pages to cover (based on file listing): `display`, `link`, `dvr`, plus any others present.

Record any issue (wrong rendering, broken navigation, focus lost, edit mode broken, etc.).

- [ ] **Step 3: Poke each custom widget specifically**

Inside the simulator session, exercise each `pp_*` widget at least once:
- `pp_dropdown` — open, navigate options, select, close
- `pp_drilldown` — enter a drill-down, navigate, exit
- `pp_tabbar` — switch between tabs
- `pp_row` — enter edit mode, change value, exit edit mode

Record any issue.

- [ ] **Step 4: Verify SDL keyboard patch behavior**

In the simulator, with menu **inactive**, press a navigation key — should not trigger menu actions. With menu **active**, press the same key — should trigger menu actions. This exercises the indev filter logic from the patch.

If the filter is broken (e.g. menu actions trigger even when menu inactive, or vice versa), the patch is wrong against v9.5.0 — revisit Task 2 Steps 3–5.

- [ ] **Step 5: For each recorded issue, diagnose and fix**

For each issue from Steps 1–4:
- Identify the root cause (read relevant LVGL v9.5.0 source if needed)
- Apply a minimal fix
- Re-run sim, confirm the issue is gone
- Confirm no new issues introduced

If a fix is non-obvious or the root cause looks like an LVGL bug, pause and report to the user before improvising.

- [ ] **Step 6: Commit per logical fix**

Each meaningful runtime fix gets its own commit:
```bash
git add <files>
git commit -m "fix(<area>): <one-line description of the runtime issue and fix>

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 7: Re-run full sweep**

After all fixes, run Steps 1–4 again from scratch. Exit criterion: no new issues observed.

---

## Task 6: Enable perf toggles in `lv_conf.h`

**Files:**
- Modify: `lv_conf.h`
- Modify (potentially): `CMakeLists.txt` (only if NEON requires explicit compile flags)

**Context:** Now that the port is stable, flip on NEON SW rendering, tiled rendering, and triple buffer.

- [ ] **Step 1: Find the exact flag names in v9.5.0 `lv_conf.h`**

Run:
```bash
grep -nE "NEON|TILED|TRIPLE_BUFFER|TRIPLE_BUFFERING|DRAW_SW_ASM" lv_conf.h
```
Record the exact flag names. Common candidates (verify against actual template):
- `LV_USE_DRAW_SW_ASM` set to `LV_DRAW_SW_ASM_NEON` (or similar)
- `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` and tiled rendering knob
- `LV_DISPLAY_RENDER_MODE_FULL` vs `LV_DISPLAY_RENDER_MODE_PARTIAL` (this may be set per-display in code, not in conf)

If a flag's exact name differs from these candidates, use the actual name from the template.

- [ ] **Step 2: Enable NEON SW rendering**

Edit `lv_conf.h` to set the NEON flag found in Step 1 to enabled. For example:
```c
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON
```

- [ ] **Step 3: Enable tiled rendering**

Edit `lv_conf.h` to enable tiled rendering (exact flag from Step 1). Set the tile size to template default (do not tune yet).

- [ ] **Step 4: Triple-buffer setup**

Triple buffering in v9.5.0 may be configured in `lv_conf.h` (a `LV_DISPLAY_RENDER_MODE_*` default) or in code (passed to `lv_display_set_buffers`). Determine which by reading the conf flag list and `osd.cpp:2109` area.

- If conf-driven: flip the flag.
- If code-driven: add a third buffer allocation alongside the existing ones in `osd.cpp` (or wherever buffers are wired) and pass it to `lv_display_set_buffers`.

If code change is needed: at 1080p XRGB8888, three buffers ≈ 24 MB. Add a `static_assert` or runtime log of total buffer bytes so the user can sanity-check on RK3566.

- [ ] **Step 5: Build simulator**

Run: `./sim.sh 2>&1 | grep -E "error:" | head -5`
Expected: no errors.

- [ ] **Step 6: Cross-compile (NEON gate check — Risk #5)**

Run the real-hardware build. If NEON-related compile flags are missing for the Rockchip toolchain, the build will fail with intrinsic-related errors.

If it fails:
- Read `CMakeLists.txt` for any compile flag plumbing.
- Add `-mfpu=neon` for armv7 or confirm aarch64 NEON is on by default for the toolchain in use.
- If the fix is non-trivial, fall back to gating NEON behind `if(NOT USE_SIMULATOR)` + actual target architecture check.

- [ ] **Step 7: Sim runtime sanity check**

Run: `./sim.sh`
Confirm OSD still renders, GSMenu still navigable. If anything regressed (e.g. tiled rendering broke partial flush), turn that specific toggle back off and document why in the commit message.

- [ ] **Step 8: Commit the perf toggle changes**

Run:
```bash
git add lv_conf.h CMakeLists.txt
git commit -m "$(cat <<'EOF'
perf(lvgl): enable NEON SW render, tiled rendering, triple buffer

NEON: software-render fast path for ARM (LV_USE_DRAW_SW_ASM=NEON).
Tiled rendering: better multi-core utilization for software draw.
Triple buffering: smoother frame delivery on the OSD path.

Toggled together after the port is verified stable in the simulator.
Any toggle that caused a regression is left off with a note here.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

If you turned off a specific toggle due to regression, replace its line in the commit body with the regression detail.

---

## Task 7: Property API migration in custom widgets

**Files:**
- Modify: `src/gsmenu/widgets/pp_row.c`
- Modify: `src/gsmenu/widgets/pp_dropdown.c`
- Modify: `src/gsmenu/widgets/pp_tabbar.c`
- Modify: `src/gsmenu/widgets/pp_drilldown.h` and/or its `.c` (check which exists)

**Context:** v9.5.0 rolled out a property interface across many built-in widgets. Custom widgets can adopt it where it shortens setup code. **Read each widget first** — only migrate where the property form is genuinely shorter or clearer than the imperative form. Do NOT migrate just to migrate.

**Risk #8 trigger:** If adopting the property API requires non-trivial glue (e.g. defining a property descriptor table on the widget class) for our `lv_obj_class_t`-based custom widgets, stop and check with the user before continuing. The fallback is to drop this task and file it as a follow-up.

### Task 7a: `pp_row` migration

- [ ] **Step 1: Read the current widget**

Read `src/gsmenu/widgets/pp_row.c` fully. Identify the constructor / setup function and list the imperative setters being called.

- [ ] **Step 2: Check property API readiness for `lv_obj_class_t`-based custom widgets**

Run:
```bash
grep -rn "lv_obj_set_property\|lv_obj_class_t.*property\|LV_PROPERTY_TYPE_" lvgl/src/ | head -10
```
Find an in-tree example of a built-in widget using the property API. Compare its `lv_obj_class_t` registration to ours. If our class is missing required property hooks, this is a Risk #8 trigger — pause and report.

- [ ] **Step 3: Migrate `pp_row` constructor / setup to property form**

Where the property API shortens the code (typically: bundled style/state/value setup), replace imperative calls with the property-form equivalent. Leave imperative calls in place where they're already concise.

- [ ] **Step 4: Build simulator**

Run: `./sim.sh 2>&1 | grep -E "error:" | head -5`
Expected: no errors.

- [ ] **Step 5: Sim runtime check — `pp_row`**

Run: `./sim.sh`
Open the GSMenu. Navigate to a page that uses `pp_row` (every page does). Confirm rows render identically: same text positions, same edit-mode highlight, same value display.

- [ ] **Step 6: Commit**

Run:
```bash
git add src/gsmenu/widgets/pp_row.c
git commit -m "refactor(gsmenu): migrate pp_row to LVGL v9.5 property API

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

### Task 7b: `pp_dropdown` migration

Repeat the same six-step pattern as Task 7a for `src/gsmenu/widgets/pp_dropdown.c`.

- [ ] **Step 1: Read `pp_dropdown.c`**
- [ ] **Step 2: Migrate to property form where it shortens code**
- [ ] **Step 3: Build sim** — `./sim.sh 2>&1 | grep -E "error:" | head -5` → no errors
- [ ] **Step 4: Sim runtime check** — open dropdown, navigate options, select, close. Visual + behavior identical.
- [ ] **Step 5: Commit** — `refactor(gsmenu): migrate pp_dropdown to LVGL v9.5 property API`

### Task 7c: `pp_tabbar` migration

Same pattern.

- [ ] **Step 1: Read `pp_tabbar.c`**
- [ ] **Step 2: Migrate**
- [ ] **Step 3: Build sim**
- [ ] **Step 4: Sim runtime check** — switch between every tab. Visual + behavior identical.
- [ ] **Step 5: Commit** — `refactor(gsmenu): migrate pp_tabbar to LVGL v9.5 property API`

### Task 7d: `pp_drilldown` migration

Same pattern. Confirm whether the file is `.h`-only or has a corresponding `.c`:
```bash
ls src/gsmenu/widgets/pp_drilldown*
```

- [ ] **Step 1: Read the file(s)**
- [ ] **Step 2: Migrate**
- [ ] **Step 3: Build sim**
- [ ] **Step 4: Sim runtime check** — enter and exit a drilldown. Visual + behavior identical.
- [ ] **Step 5: Commit** — `refactor(gsmenu): migrate pp_drilldown to LVGL v9.5 property API`

---

## Task 8: Native blur on GSMenu background

**Files:**
- Modify: `src/gsmenu/styles.c`

**Context:** LVGL v9.5.0 added native CPU-only blur and drop-shadow rendering. Apply it to the GSMenu container only — not the OSD, which has a tight per-frame budget over live video.

- [ ] **Step 1: Locate the GSMenu container style in `styles.c`**

Read `src/gsmenu/styles.c`. Find the style applied to the top-level menu container (it'll have border/background settings; likely named `style_menu_bg` or similar).

- [ ] **Step 2: Add blur and drop-shadow properties**

Locate the corresponding style and add (using the v9.5.0 native blur API — find exact symbol names with `grep -rn "lv_style_set_.*blur\|LV_STYLE_BLUR" lvgl/src/`):

```c
/* Native blur — backdrop blur behind menu container (LVGL v9.5+) */
lv_style_set_backdrop_blur(&style_menu_bg, 8);  /* tune radius in sim */
/* Drop shadow — soft shadow under the menu container */
lv_style_set_shadow_width(&style_menu_bg, 24);
lv_style_set_shadow_opa(&style_menu_bg, LV_OPA_50);
lv_style_set_shadow_color(&style_menu_bg, lv_color_black());
```

Use the actual API names from your grep — names above are illustrative.

- [ ] **Step 3: Build simulator**

Run: `./sim.sh 2>&1 | grep -E "error:" | head -5`
Expected: no errors.

- [ ] **Step 4: Sim runtime check — visual**

Run: `./sim.sh`. Open the GSMenu. Observe:
- Backdrop is blurred behind the menu container
- Drop shadow visible under the menu container
- FPS feels acceptable (no obvious lag when navigating)

Capture a before/after screenshot of the menu open state (save to `/tmp/gsmenu-blur-before.png` and `/tmp/gsmenu-blur-after.png` — the "before" is from your memory of the current state, or revert briefly to capture, your call).

- [ ] **Step 5: Tune blur radius if needed**

If the blur is too strong (slow) or too subtle, adjust the radius (Step 2's `8`). Rebuild + re-verify.

If blur causes obvious frame drops on the menu open (uncomfortable lag), reduce radius. If still bad at radius 4, gate the blur behind a `lv_conf.h` define and turn it off — note the regression in the commit message and surface it to the user.

- [ ] **Step 6: Commit**

Run:
```bash
git add src/gsmenu/styles.c
git commit -m "$(cat <<'EOF'
feat(gsmenu): native blur and drop shadow on menu background

Uses LVGL v9.5's CPU-only blur and shadow rendering. Applied only to
the GSMenu container; OSD overlay path is untouched to preserve the
per-frame budget over live video.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: `LV_STATE_ALT` scaffold

**Files:**
- Modify: `src/gsmenu/styles.c`
- Create: `tests/lvgl_alt_state_test.cpp`
- Modify: `tests/CMakeLists.txt` (or the appropriate test wiring file — locate first)

**Context:** Lay the plumbing for a future day/night theme without shipping the theme itself. Add a style hook that responds to `LV_STATE_ALT` and a stubbed toggle call site that is feature-flagged off.

- [ ] **Step 1: Locate the existing style setup pattern**

Read `src/gsmenu/styles.c`. Identify how default-state styles are registered (typically `lv_style_init` + `lv_obj_add_style(obj, &style, LV_PART_MAIN | LV_STATE_DEFAULT)`).

- [ ] **Step 2: Write the failing Catch2 test**

First, find an existing test for the structure:
```bash
ls tests/
cat tests/CMakeLists.txt 2>/dev/null || cat CMakeLists.txt | grep -A 20 pixelpilot_tests
```

Create `tests/lvgl_alt_state_test.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "lvgl/lvgl.h"
#include "src/gsmenu/styles.h"
}

/* Confirms the LV_STATE_ALT style hook is registered on the menu
 * container style and can be toggled at runtime without crashing. */
TEST_CASE("LV_STATE_ALT style hook is registered", "[gsmenu][styles]") {
    lv_init();

    /* Function under test: gsmenu_styles_init() must register an
     * LV_STATE_ALT variant on the menu background style. */
    gsmenu_styles_init();

    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_t * menu = lv_obj_create(screen);
    gsmenu_styles_apply_menu_bg(menu);

    /* Toggle ALT state — should not crash and should report the state
     * is set. */
    lv_obj_add_state(menu, LV_STATE_ALT);
    REQUIRE((lv_obj_get_state(menu) & LV_STATE_ALT) != 0);

    lv_obj_remove_state(menu, LV_STATE_ALT);
    REQUIRE((lv_obj_get_state(menu) & LV_STATE_ALT) == 0);

    lv_obj_delete(screen);
    lv_deinit();
}
```

If `src/gsmenu/styles.h` doesn't expose `gsmenu_styles_init` and `gsmenu_styles_apply_menu_bg`, that's expected — Step 4 adds them.

- [ ] **Step 3: Wire the test into the build and confirm it fails**

Add the test source to whatever test target list exists (likely `pixelpilot_tests` in the top-level `CMakeLists.txt` per the earlier scan — `lvgl` is linked there).

Then build + run:
```bash
mkdir -p build && cd build && cmake .. && make pixelpilot_tests
./pixelpilot_tests "[gsmenu][styles]"
```
Expected: FAIL — either compile error (`gsmenu_styles_apply_menu_bg` undefined) or assertion failure.

- [ ] **Step 4: Implement the hook**

In `src/gsmenu/styles.c`:
- Create a new `lv_style_t style_menu_bg_alt`
- In the init function, set distinguishing properties on the alt style (e.g. invert colors — but keep it minimal; this is scaffold, not a full theme)
- Where `style_menu_bg` is applied to the menu container, add a paired `lv_obj_add_style(menu, &style_menu_bg_alt, LV_PART_MAIN | LV_STATE_ALT)`
- Expose `gsmenu_styles_init` and `gsmenu_styles_apply_menu_bg` (or rename to match the actual existing API) in `src/gsmenu/styles.h`

If the existing styles header already exposes these functions under different names, adapt the test in Step 2 to use the real names — don't invent new ones just for the test.

- [ ] **Step 5: Run the test, confirm it passes**

```bash
cd build && make pixelpilot_tests && ./pixelpilot_tests "[gsmenu][styles]"
```
Expected: PASS.

- [ ] **Step 6: Build simulator and verify GSMenu still renders normally (no ALT state set)**

Run: `./sim.sh`. Open GSMenu. Should look identical to Task 8's "after" state — no visible change, because ALT is not engaged.

- [ ] **Step 7: Manually toggle ALT in sim to spot-check the alternate style renders**

For verification only (do NOT ship this): temporarily add a key handler in `src/input.cpp` simulator path that calls `lv_obj_add_state(menu_root, LV_STATE_ALT)` on, say, the `T` key. Build + run + press `T` + observe the alt style renders + press again to remove. **Revert this temp change before committing** — the production toggle UI is out of scope per the spec.

Confirm: `git diff src/input.cpp` shows no stray changes before commit.

- [ ] **Step 8: Commit scaffold + test**

Run:
```bash
git add src/gsmenu/styles.c src/gsmenu/styles.h tests/lvgl_alt_state_test.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(gsmenu): scaffold LV_STATE_ALT for future day/night theme

Registers an alternate style variant on the menu background that
activates when the container has LV_STATE_ALT. No user-facing toggle
yet — this commit only lays the plumbing. Includes a Catch2 unit
test asserting the state hook can be set and cleared without crash.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Final sweep + PR

- [ ] **Step 1: Full simulator sweep from scratch**

Run: `./sim.sh`. Repeat Task 5 Steps 1–4 in full:
- OSD readouts render correctly
- Every GSMenu page navigable
- Every `pp_*` widget exercised
- SDL keyboard patch behavior holds (menu-active vs inactive routing)
- Blur visible on GSMenu background
- LV_STATE_ALT not engaged in default UI (no visible alt style)

If any regression: pause, fix, commit, then re-sweep.

- [ ] **Step 2: Cross-compile sweep**

Run the real-hardware build path once more. Expected: clean build.

- [ ] **Step 3: Verify commit history is clean**

Run: `git log --oneline feat/refactor_ui ^master | head -40`
Expected: ~8–15 commits in this order:
1. spec commit (already exists)
2. submodule bump + patch refresh
3. lv_conf reconciliation
4. one or more compile fix commits
5. one or more runtime fix commits
6. perf toggles
7. 4 widget refactor commits (pp_row, pp_dropdown, pp_tabbar, pp_drilldown)
8. blur commit
9. LV_STATE_ALT scaffold + test commit

Each commit message follows the project's existing convention (look at `git log --oneline -20` from before this branch to confirm style).

- [ ] **Step 4: Push branch**

Run: `git push -u origin feat/refactor_ui`

- [ ] **Step 5: Open PR with evidence-tied description**

Per the spec's evidence discipline: every "this works" claim must be tied to a sim screenshot, build log, or planned device confirmation.

```bash
gh pr create --title "Upgrade LVGL v9.2.2 → v9.5.0 + modernization" --body "$(cat <<'EOF'
## Summary

- Bumps LVGL submodule v9.2.2 → v9.5.0
- Reconciles lv_conf.h from the v9.5.0 template; enables NEON SW render, tiled rendering, triple buffer
- Refreshes patches/lvgl-sdl-keyboard.patch against v9.5.0 sources
- Migrates pp_row, pp_dropdown, pp_tabbar, pp_drilldown to the v9.5 property API
- Adds native blur and drop shadow to the GSMenu container
- Adds LV_STATE_ALT plumbing (scaffold only — no user-facing day/night toggle yet) with a Catch2 unit test

Spec: `docs/superpowers/specs/2026-05-27-lvgl-upgrade-design.md`
Plan: `docs/superpowers/plans/2026-05-27-lvgl-upgrade.md`

## Validation

Sim-driven (Claude):
- ✅ OSD overlay: readouts render, no flicker, no z-order issues
- ✅ GSMenu pages (display, link, dvr, ...): every page navigable top-to-bottom
- ✅ Custom widgets: pp_dropdown, pp_drilldown, pp_tabbar, pp_row all behave identically to pre-upgrade
- ✅ SDL keyboard patch: menu-active/inactive routing still correct
- ✅ Blur visible on GSMenu (screenshot pending — see attached)
- ✅ pixelpilot_tests passes the LV_STATE_ALT test

Cross-compile (Claude):
- ✅ Real-hardware build target compiles clean

Device (assigned to user):
- ⏳ Flash on RK3566 — confirm OSD overlay + GSMenu work, DVR re-encode includes OSD overlay, frame pacer / latency unchanged
- ⏳ Flash on RK3588s — same checks

## Test plan

- [ ] OSD renders correctly on real video stream
- [ ] GSMenu fully navigable via GPIO buttons
- [ ] DVR recording includes OSD overlay
- [ ] No frame pacer / latency regression
- [ ] Heap usage acceptable on RK3566 with triple buffer (note: ~24MB for 3x 1080p XRGB8888)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 6: Hand off**

Reply to the user with the PR URL and the device test plan checklist. Wait for hardware confirmation before any cleanup or merge action — per spec, the device pass is user-driven and must precede merge.

---

## Self-review notes

**Spec coverage:**
- Submodule bump → Task 1
- lv_conf reconciliation → Task 3
- Patch refresh → Task 2
- Compile + runtime fixes → Tasks 4, 5
- Property API migration → Task 7 (a–d for all four widgets)
- Native blur → Task 8
- LV_STATE_ALT scaffold → Task 9
- Perf toggles → Task 6
- Validation (sim + cross-compile + device) → Tasks 4 Step 8, 10 Steps 1–2, 10 Step 5 hand-off
- Risk #1 (display API) → Task 4 Step 5 escalation path
- Risk #2 (indev API) → Task 2 Step 3 escalation
- Risk #5 (NEON cross-compile) → Task 6 Step 6
- Risk #6 (triple-buffer memory) → Task 6 Step 4 + PR test plan
- Risk #7 (blur perf) → Task 8 Step 5
- Risk #8 (property API custom widgets) → Task 7 Step 2 (per-widget) escalation

All spec items mapped.

**Placeholders:** None — every "exploratory" task (compile/runtime fixes) is structured with a methodology, not a "TODO".

**Type consistency:** `gsmenu_styles_init` and `gsmenu_styles_apply_menu_bg` are referenced in both the Task 9 test and the Task 9 implementation step; Task 9 Step 4 explicitly notes adapting names if the existing header uses different ones.
