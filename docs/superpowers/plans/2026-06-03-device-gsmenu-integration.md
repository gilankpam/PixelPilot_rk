# Finish the feat/refactor_ui Device Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PixelPilot_rk's device build (`USE_SIMULATOR=OFF`) compile and link on `feat/refactor_ui`, cross-build it for the radxa_zero3 ground-station, and smoke-test the new fpvd settings menu on the GS.

**Architecture:** The new gsmenu is already driven on-device (`osd.cpp:2155-2164` runs `setup_lvgl()` → `pp_menu_main()` → `lv_task_handler()` in the OSD thread). The only blockers are dangling references to purged old-menu code: three dead `#include`s, an orphaned `config["gsmenu"]["actions"]` parsing block in `main.cpp`, and an undefined `toggle_rec_enabled()`. We delete the orphaned machinery and implement the one missing function, then cross-build and smoke-test.

**Tech Stack:** C++ (device runtime), LVGL, Rockchip MPP/DRM; Buildroot (OpenIPC sbc-groundstations, `radxa_zero3_defconfig`, aarch64) via source-override; Nix build envs.

**Spec:** `docs/superpowers/specs/2026-06-03-device-gsmenu-integration-design.md`

---

## Important context for the implementer

- **`main.cpp` and `wfbcli.cpp` are device-only** — they are NOT in `SIMULATOR_SOURCES`, so the simulator build does not compile them. Task 1's edits therefore can't be verified by the sim build; they're verified by a grep (no dangling refs remain) and, definitively, by the device cross-build in Task 3.
- **`input.cpp` and `main.h` ARE in the sim build**, so Task 2 is verified by the sim build compiling/linking.
- The device build CANNOT be compiled by the normal sim toolchain — it needs the Buildroot cross-build (Task 3). Do not try `cmake -DUSE_SIMULATOR=OFF` locally.
- Whitespace in `main.cpp` around the edited regions is **mixed tabs and spaces** — match it exactly. After each edit, prefer a follow-up `grep` to confirm the change rather than eyeballing.

### Build/verify commands

Sim build + unit tests (run from `/home/gilankpam/Projects/drone/PixelPilot_rk`):
```bash
nix-shell shell-sim.nix --run "cmake --build build-test --target pixelpilot fpvd_tests gs_rxpower_tests gs_enum_tests settings_tests -j && \
  ./build-test/fpvd_tests && ./build-test/gs_rxpower_tests && ./build-test/gs_enum_tests && ./build-test/settings_tests"
```

Device cross-build (radxa_zero3, source-override from our working tree). The Buildroot Nix FHS env only runs commands fed on **stdin** (`nix-shell --run`/`--command` silently no-op):
```bash
printf '%s\n' \
  'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
  'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
  'export DEFCONFIG=radxa_zero3_defconfig' \
  './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' \
  | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix
```
Built artifact: `output/radxa_zero3_defconfig/target/usr/bin/pixelpilot` (and the override build dir `output/radxa_zero3_defconfig/build/pixelpilot-custom/`).

---

## Task 1: Remove the orphaned old-menu machinery from main.cpp and wfbcli.cpp

**Files:**
- Modify: `src/main.cpp` (includes ~63-65, array defs ~144-147, actions block ~1462-1512, TODO ~984)
- Modify: `src/wfbcli.cpp:23`

- [ ] **Step 1: Remove the three purged-header includes in `src/main.cpp`.** Replace:

```cpp
#include "latency_probe.hpp"
#include "gsmenu/gs_system.h"
#include "gsmenu/air_actions.h"
#include "gsmenu/gs_actions.h"
#include "gsmenu/settings.h"
#include "menu.h"
```

with:

```cpp
#include "latency_probe.hpp"
#include "gsmenu/settings.h"
#include "menu.h"
```

- [ ] **Step 2: Remove the orphaned action-array definitions in `src/main.cpp`.** Replace:

```cpp
OsSensors os_sensors; // TODO: pass as argument to `main_loop`
MenuAction airactions[MAX_ACTIONS];
size_t airactions_count;
MenuAction gsactions[MAX_ACTIONS];
size_t gsactions_count;

// Add global variables for plane id overrides
```

with:

```cpp
OsSensors os_sensors; // TODO: pass as argument to `main_loop`

// Add global variables for plane id overrides
```

- [ ] **Step 3: Remove the orphaned `config["gsmenu"]["actions"]` parsing block in `src/main.cpp`.** This block populates the arrays just deleted and is never read by the new menu. Replace the whole region (note the mixed tabs/spaces — copy exactly):

```cpp
		// GSMENU settings
		if (config["gsmenu"]) {
            if (config["gsmenu"]["enabled"]) {
                gsmenu_enabled = config["gsmenu"]["enabled"].as<bool>();
            }
		if (gsmenu_enabled && config["gsmenu"]["actions"]) {
			if (config["gsmenu"]["actions"]["air"]) {
				const YAML::Node& actionsNode = config["gsmenu"]["actions"]["air"];
				airactions_count = 0;
				
				for (YAML::const_iterator it = actionsNode.begin(); 
					it != actionsNode.end() && airactions_count < MAX_ACTIONS; 
					++it) {
					
					std::string label = (*it)["label"].as<std::string>();
					std::string cmd = (*it)["action"].as<std::string>();
					
					// Access the global array at the current index
					strncpy(airactions[airactions_count].label, label.c_str(), MAX_LABEL_LEN - 1);
					airactions[airactions_count].label[MAX_LABEL_LEN - 1] = '\0';
					
					strncpy(airactions[airactions_count].action, cmd.c_str(), MAX_ACTION_LEN - 1);
					airactions[airactions_count].action[MAX_ACTION_LEN - 1] = '\0';
					
					airactions_count++;
				}
				spdlog::debug("Parsed {} GS Actions", airactions_count);
			}
			if (config["gsmenu"]["actions"]["ground"]) {
				const YAML::Node& actionsNode = config["gsmenu"]["actions"]["ground"];
				gsactions_count = 0;
				
				for (YAML::const_iterator it = actionsNode.begin(); 
					it != actionsNode.end() && gsactions_count < MAX_ACTIONS; 
					++it) {
					
					std::string label = (*it)["label"].as<std::string>();
					std::string cmd = (*it)["action"].as<std::string>();
					
					// Access the global array at the current index
					strncpy(gsactions[gsactions_count].label, label.c_str(), MAX_LABEL_LEN - 1);
					gsactions[gsactions_count].label[MAX_LABEL_LEN - 1] = '\0';
					
					strncpy(gsactions[gsactions_count].action, cmd.c_str(), MAX_ACTION_LEN - 1);
					gsactions[gsactions_count].action[MAX_ACTION_LEN - 1] = '\0';
					
					gsactions_count++;
				}
				spdlog::debug("Parsed {} GS Actions", gsactions_count);
			}
		}
		}
```

with (keep only the `enabled` parse + the outer brace):

```cpp
		// GSMENU settings
		if (config["gsmenu"]) {
            if (config["gsmenu"]["enabled"]) {
                gsmenu_enabled = config["gsmenu"]["enabled"].as<bool>();
            }
		}
```

- [ ] **Step 4: Remove the stale TODO comment in `main_loop()` (`src/main.cpp`).** Replace:

```cpp
    while (!signal_flag) {
        // TODO: put gsmenu main loop here
        msg_manager.check_message();
```

with:

```cpp
    while (!signal_flag) {
        msg_manager.check_message();
```

- [ ] **Step 5: Remove the dead include in `src/wfbcli.cpp`.** Replace:

```cpp
#include "gsmenu/gs_system.h"
#include "wfbcli.hpp"
```

with:

```cpp
#include "wfbcli.hpp"
```

- [ ] **Step 6: Verify no purged references remain.** Run:

```bash
cd /home/gilankpam/Projects/drone/PixelPilot_rk
grep -rInE 'gsmenu/(gs_system|air_actions|gs_actions)\.h|MenuAction|MAX_ACTIONS|airactions|gsactions|MAX_LABEL_LEN|MAX_ACTION_LEN|put gsmenu main loop' src --include='*.c' --include='*.cpp' --include='*.h'
```
Expected: **no output** (all references gone).

- [ ] **Step 7: Confirm the sim build + unit tests still pass** (these files aren't in the sim, so this only proves nothing shared broke):

```bash
nix-shell shell-sim.nix --run "cmake --build build-test --target pixelpilot fpvd_tests settings_tests -j && ./build-test/fpvd_tests && ./build-test/settings_tests"
```
Expected: builds; `fpvd_tests` 94/30 and `settings_tests` 31 cases PASS.

- [ ] **Step 8: Commit.**

```bash
git add src/main.cpp src/wfbcli.cpp
git commit -m "device: drop orphaned old-menu actions machinery + dead purged-header includes"
```

---

## Task 2: Implement toggle_rec_enabled()

The rec-record button (`input.cpp:222,256`) calls `toggle_rec_enabled()`, which has no definition on this branch. Implement it to drive the DVR via the existing `extern "C"` interface, with the device-only calls guarded so the simulator build (which compiles `input.cpp`) stays clean.

**Files:**
- Modify: `src/main.h` (add DVR control declarations)
- Modify: `src/input.cpp` (define `toggle_rec_enabled`)

- [ ] **Step 1: Declare the DVR control functions in `src/main.h`.** They are defined `extern "C"` in `main.cpp` (~line 650) but declared in no header. Add them after the existing playback declarations. Replace:

```cpp
void normal_playback();
void pause_playback();
void resume_playback();
```

with:

```cpp
void normal_playback();
void pause_playback();
void resume_playback();

/* DVR live-control C interface (defined extern "C" in main.cpp). */
extern "C" void dvr_start_all(void);
extern "C" void dvr_stop_all(void);
```

- [ ] **Step 2: Define `toggle_rec_enabled()` in `src/input.cpp`.** `input.cpp` already includes `main.h` (line 17) and declares `extern int dvr_enabled;` (line 28). Add the definition immediately before `toggle_screen()`. Replace:

```cpp
void toggle_screen(void) {
    if( ! menu_active ) {
```

with:

```cpp
void toggle_rec_enabled(void) {
#ifndef USE_SIMULATOR
    /* Device: toggle the on-board DVR via the C interface in main.cpp. */
    if (dvr_enabled) dvr_stop_all();
    else             dvr_start_all();
#endif
    /* Simulator: no-op. The sim rec path (input.cpp send_button_event)
     * already flips dvr_enabled itself, so doing nothing here avoids a
     * double toggle. */
}

void toggle_screen(void) {
    if( ! menu_active ) {
```

- [ ] **Step 3: Build the simulator to confirm `input.cpp` + `main.h` still compile/link.** (`input.cpp` is in the sim; the device `#ifndef` branch is excluded there, so `dvr_start_all/stop_all` are not referenced and need no sim definition.)

```bash
nix-shell shell-sim.nix --run "cmake --build build-test --target pixelpilot settings_tests -j && ./build-test/settings_tests"
```
Expected: `pixelpilot` links; `settings_tests` 31 cases PASS.

- [ ] **Step 4: Confirm `toggle_rec_enabled` is now defined exactly once.** Run:

```bash
grep -rIn 'toggle_rec_enabled' /home/gilankpam/Projects/drone/PixelPilot_rk/src --include='*.c' --include='*.cpp'
```
Expected: the two existing call sites in `input.cpp` (222, 256) plus exactly one definition (`void toggle_rec_enabled(void) {`).

- [ ] **Step 5: Commit.**

```bash
git add src/main.h src/input.cpp
git commit -m "device: implement toggle_rec_enabled (DVR toggle via C iface; sim no-op)"
```

---

## Task 3: Cross-build for radxa_zero3 until it links

This is the real compile+link gate for the device-only changes. The first cross-build before these fixes failed on `gsmenu/gs_system.h`; it may surface further residual references once that's past — resolve each the same minimal way (delete a dead include, or stub/redirect a dangling symbol) and rebuild.

**Files:**
- Possibly modify: residual device sources flagged by the compiler/linker (unknown until the build runs).

- [ ] **Step 1: Run the cross-build.**

```bash
printf '%s\n' \
  'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
  'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
  'export DEFCONFIG=radxa_zero3_defconfig' \
  './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' \
  | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix 2>&1 | tee /tmp/pp_gs_build.log | tail -40
```
This takes a few minutes (it recompiles LVGL + the app for aarch64).

- [ ] **Step 2: Check the result.**

```bash
grep -E "PPBUILD_DONE rc=|error:|fatal error|Error [0-9]" /tmp/pp_gs_build.log | tail -20
BIN=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/target/usr/bin/pixelpilot
ls -l "$BIN"; date -r "$BIN"
readelf -h "$BIN" 2>/dev/null | grep -E 'Class|Machine'
```
Expected on success: `PPBUILD_DONE rc=0`, no `error:`/`fatal error`, the target binary mtime is **now**, `Class: ELF64`, `Machine: AArch64`.

- [ ] **Step 3: If the build failed, fix residual dangling references and rebuild.** For each compile error `fatal error: gsmenu/<x>.h: No such file or directory`, open the offending file and remove the dead include (the new menu lives in `src/menu.c` + `src/gsmenu/pages/*`, so old `air_*`/`gs_*` includes are dead). For each link error `undefined reference to '<sym>'`, check whether `<sym>` was a purged old-menu function; if it's genuinely needed, define a minimal version next to its caller (mirroring Task 2), otherwise remove the dead call. Re-run Step 1 after each fix. Repeat until `PPBUILD_DONE rc=0`. Commit any residual fixes:

```bash
git add -p   # stage only the residual source fixes you made (NOT osd2.json / waybeam_venc/)
git commit -m "device: resolve residual purged-symbol references for the cross-build"
```
(If no residual fixes were needed, skip the commit.)

- [ ] **Step 4: Re-confirm the sim build + full test suite are still green** (the residual fixes, if any, may have touched shared files):

```bash
nix-shell shell-sim.nix --run "cmake --build build-test --target pixelpilot fpvd_tests gs_rxpower_tests gs_enum_tests settings_tests -j && \
  ./build-test/fpvd_tests && ./build-test/gs_rxpower_tests && ./build-test/gs_enum_tests && ./build-test/settings_tests"
```
Expected: all green (`fpvd_tests` 94/30, `gs_rxpower_tests` 32/6, `gs_enum_tests` 8/2, `settings_tests` 31).

---

## Task 4: Smoke-test on the GS

Non-persistent test from `/tmp` (the GS `/` is overlayfs-rw and `/tmp` is tmpfs; the installed `/usr/bin/pixelpilot` is left untouched). The GS `fpvd` daemon is already running, so the fpvd menu rows have a live backend.

**Files:** none (manual verification).

- [ ] **Step 1: Copy the binary to the GS.**

```bash
scp /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/target/usr/bin/pixelpilot \
    root@10.18.0.1:/tmp/pixelpilot
ssh root@10.18.0.1 'chmod +x /tmp/pixelpilot && md5sum /tmp/pixelpilot'
```

- [ ] **Step 2: Inspect how the service launches pixelpilot (to reuse its args/env).**

```bash
ssh root@10.18.0.1 'cat /usr/bin/pixelpilot.sh; echo ---; cat /etc/default/pixelpilot'
```
Note `PIXELPILOT_ARGS` / env (CONFIG, OSD_PATH, SCREEN_MODE, …).

- [ ] **Step 3: Stop the running instance and launch the fresh binary with the same env/args.**

```bash
ssh -t root@10.18.0.1 '/etc/init.d/S99pixelpilot stop; \
  set -a; . /etc/default/pixelpilot 2>/dev/null; set +a; \
  /tmp/pixelpilot $PIXELPILOT_ARGS'
```
Expected: app starts, video shows, no crash. (Run interactively so you can watch logs and exit with Ctrl-C.)

- [ ] **Step 4: Verify the fpvd settings menu works.** Open the gsmenu (the GS's menu button / mapped key). Check:
  - The menu renders over the video.
  - `Link → Channel` / `Camera → Bitrate` / `Camera → Codec` show the **current** values (read live from the GS fpvd via `/link` and `/air/config`).
  - Changing a value applies — confirm out-of-band against the GS fpvd:
    ```bash
    ssh root@10.18.0.1 'curl -s http://127.0.0.1:8080/link; echo; curl -s http://127.0.0.1:8080/air/config | head -c 400'
    ```
    The changed field should reflect the new value (channel/width via `/link`; video fields via `/air/config`).
  - Inert rows (DVR enable via menu, System reboot/factory actions) may no-op — expected per spec.

- [ ] **Step 5: Restore the installed service when done.**

```bash
ssh root@10.18.0.1 'rm -f /tmp/pixelpilot; /etc/init.d/S99pixelpilot start'
```

- [ ] **Step 6: Record the outcome** in the PR / notes: did the device build link, did the menu render, did fpvd reads/writes round-trip on the GS.

---

## Self-review

**Spec coverage**

| Spec requirement | Task |
|---|---|
| Remove 3 purged-header includes (main.cpp 63-65) | Task 1 Step 1 |
| Remove `airactions/gsactions` defs (144-147) | Task 1 Step 2 |
| Remove `config["gsmenu"]["actions"]` block; keep `gsmenu_enabled` parse | Task 1 Step 3 |
| Drop stale `main_loop` TODO | Task 1 Step 4 |
| Remove dead `wfbcli.cpp` include | Task 1 Step 5 |
| Declare `dvr_start_all/stop_all` in `main.h` | Task 2 Step 1 |
| Implement `toggle_rec_enabled` (device toggle / sim no-op) | Task 2 Step 2 |
| Iterate radxa_zero3 cross-build to a clean link | Task 3 |
| Sim build + settings/unit tests stay green | Task 1 Step 7, Task 2 Step 3, Task 3 Step 4 |
| On-GS `/tmp` smoke test of fpvd rows | Task 4 |
| Inert GS-side rows acceptable | Noted in Task 4 Step 4 |

**Placeholder scan:** No TBD/TODO placeholders. (Task 3 is inherently iterative — "fix residual references" — because the first build halts early; the step gives the exact decision procedure and stop condition (`PPBUILD_DONE rc=0`), which is the honest shape of a make-it-link task, not a vague placeholder.)

**Type/consistency:** `toggle_rec_enabled(void)`, `dvr_start_all(void)`, `dvr_stop_all(void)`, `dvr_enabled` (int) are used identically across Task 2 and the existing call sites. The `main.h` declarations are `extern "C"` to match `main.cpp`'s `extern "C"` definitions. The actions-block removal leaves the `if (config["gsmenu"]) { if (...enabled...) {…} }` structure brace-balanced.
