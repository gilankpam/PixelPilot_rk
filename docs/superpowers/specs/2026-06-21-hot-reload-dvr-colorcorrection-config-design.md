# Hot-reloadable DVR + Color-Correction Config via Persistent JSON

**Date:** 2026-06-21
**Status:** Approved design — ready for implementation planning

## Problem

DVR recording settings (mode, max size, re-encode bitrate) and color-correction
settings (enable, gain, offset) are currently supplied to pixelpilot only as
CLI flags. Changing any of them today means restarting the pixelpilot process
(via the gsmenu "Apply changes" button → fpvd → systemd relaunch). This is slow,
drops the video feed, and gives no live feedback when tuning color correction.

We want these six settings to live in a persistent JSON file and be **hot-applied
without a restart**, with gain/offset tunable live against the actual feed.

## Scope

In scope — move to JSON + hot reload:
- DVR: `mode`, `maxSizeMb`, `reencBitrateKbps`
- Color correction: `enabled`, `gain`, `offset` (applied to **both** the live
  display and the re-encode recording)

Explicitly out of scope:
- Screen mode, video scale, RTP jitter — these stay on the fpvd + restart path
  (they require decoder/display reinit). The gsmenu "Apply changes" button
  remains for these.
- The DVR "Enabled" toggle row stays as-is; recording start/stop remains the
  runtime SIGUSR1 toggle, not part of this JSON.

## Architecture decisions (from brainstorming)

1. **Ownership (B):** pixelpilot owns the JSON. The menu writes it directly and
   fpvd is not involved for these six settings. Keeps the whole change
   self-contained in the PixelPilot_rk repo.
2. **Apply mechanism — in-process direct calls (NOT SIGHUP).** During
   brainstorming we assumed gsmenu was a separate process and chose SIGHUP. That
   premise is wrong: **gsmenu runs in-process with pixelpilot** —
   `build_pixelpilot_tab()` is called from `src/menu.c:114`, and the only
   non-test executable is `pixelpilot` itself (`CMakeLists.txt:425`). The menu
   therefore shares the address space with the DVR/color-correction state and
   the existing `extern "C"` runtime setters in `main.cpp`. On a row change the
   menu **calls the setter directly** for instant apply and **writes the JSON**
   for persistence. No signal, no IPC, no `pidof`, no reload flag, no main-loop
   polling.
3. **Color-correction target (B):** both live display and re-encode recording.
4. **Apply UX (A):** live apply. Each change calls the runtime setter
   immediately and writes the JSON. **No debounce needed:** the keypad-driven
   widgets write exactly once on commit (the slider calls `pp_settings_set_async`
   only on ENTER-to-exit-edit, and only if the value changed — pp_slider.c:142),
   not per-step, so there is no drag write-storm. This keeps the module
   LVGL-free and deterministic.
5. **Recording gate:** while recording, all three DVR rows are greyed out in the
   menu; color-correction rows stay live. Recording state is read in-process
   (`dvr_enabled` / the `dvr.recording` fact).
6. **Startup precedence (A):** JSON is authoritative. The four CLI flags are
   removed; a default JSON ships so the file always exists.

## The runtime JSON

A single file pixelpilot owns, mirroring the menu rows 1:1 in menu-scale values
(so gsmenu is a dumb writer; all unit mapping lives in pixelpilot):

```json
{
  "dvr":             { "mode": "raw", "maxSizeMb": 4000, "reencBitrateKbps": 8000 },
  "colorCorrection": { "enabled": false, "gain": 25, "offset": -15 }
}
```

- `dvr.mode` ∈ `{ "raw", "reencode", "both" }` → `DvrMode` enum.
- `gain` 0–50 → shader float `gain / 10.0` (default 25 → **2.5**).
- `offset` −50..50 → shader float `offset / 100.0` (default −15 → **−0.15**).
  These mappings reproduce the current hardcoded defaults exactly.
- **Location:** a writable + persistent path on the GS. Proposed
  `/etc/pixelpilot/runtime.json`. **OPEN ITEM:** confirm a path that (a) survives
  reboot and (b) is writable by the gsmenu process on the GS image before
  implementation.
- A default `runtime.json` ships with the package so the file always exists
  (required by decision 6).

## pixelpilot side

A new small config module (load/parse/serialize) plus a thin `extern "C"`
color-correction apply wrapper in `main.cpp`. There is **no** signal handler and
**no** main-loop reload — applies happen via direct in-process calls from the
menu (see "menu side").

- **Startup:** parse the JSON; seed the existing globals `dvr_mode`,
  `dvr_max_file_size`, `reenc_params.bitrate_kbps`, `enable_live_colortrans`,
  `live_colortrans_gain`, `live_colortrans_offset`. The four CLI flags
  (`--dvr-mode`, `--dvr-max-size`, `--dvr-reenc-bitrate`, `--live-colortrans`)
  become **deprecated no-ops** (accept + warn + ignore, matching the existing
  `--dvr-framerate` pattern) rather than hard-removed — fpvd may still inject
  them via `EXTRA_OPTS` at relaunch, and removing the parser would break launch.
  JSON is the authoritative source; the flags no longer have any effect.
- **Apply ops vtable:** the menu cannot call the `main.cpp` runtime setters
  directly because the simulator (and host tests) build the PP page but do not
  link `main.cpp`. The runtime-config module exposes a registerable ops struct
  (`dvr_set_mode`, `dvr_set_max_size`, `dvr_reenc_set_bitrate`,
  `colortrans_apply`, `is_recording`). The device build registers the real
  `extern "C"` functions at startup; the simulator/tests leave it unregistered
  (calls become no-ops / recording reads false). This also makes the module
  fully host-testable with a fake ops struct.
- **Existing runtime setters reused as-is** (already `extern "C"` in main.cpp):
  - DVR → `dvr_set_mode()`, `dvr_reenc_set_bitrate()`, `dvr_set_max_size()`.
  - Color correction, re-encode path → `set_color_correction()` /
    `set_color_correction_enabled()`.
  - Color correction, live display → `gamma_lut_enable(&lut_ctrl, offset, gain)`
    / `gamma_lut_disable(&lut_ctrl)` (both already exist in `drm.c`; the display
    thread already branches on `enable_live_colortrans` per frame at
    main.cpp:424, so toggling the global switches the OSD compositing path
    automatically).
- **New `extern "C"` wrapper** `pp_colortrans_apply(int enabled, float gain,
  float offset)` that ties the three together: set the globals, update the live
  display LUT, and update the re-encode path. This is the only net-new pixelpilot
  apply code (the DVR setters already do everything needed).
- **Robustness:** a malformed or missing JSON at startup → log and fall back to
  the built-in defaults (no crash).
- DVR field changes are only ever applied when not recording (the menu gate
  prevents mid-recording edits; the `dvr_set_mode()` setter would otherwise
  force-stop an active recording).

## Launch path

- The systemd unit (`debian/pixelpilot-rk.pixelpilot.service`) does not pass
  these four flags directly today (fpvd injects them via `EXTRA_OPTS`), so the
  unit needs no change. The flags are handled as deprecated no-ops in the parser.
- Package ships the default `runtime.json` at the chosen path so the file always
  exists.

## Menu side (in-process gsmenu)

The menu routes the six keys through a new in-process runtime-config module
instead of the fpvd provider. The menu widgets are unchanged; they keep calling
`pp_settings_set/get`. Routing happens inside the fpvd provider: when a key is
one of the six runtime keys, the provider delegates to the runtime-config module
rather than staging to fpvd.

- **New module `settings_runtime_cfg.{c,h}`** (in `src/gsmenu/`): owns the JSON
  file (read at startup, atomic write on change via temp file + `rename`), the
  value↔float mapping for gain/offset, and dispatch into the registered apply ops
  (`dvr_set_mode`, `dvr_set_max_size`, `dvr_reenc_set_bitrate`,
  `colortrans_apply`, `is_recording`). Exposes a predicate
  `pp_runtime_cfg_owns(domain, page, key)` plus `set`/`get` for the six keys.
  No LVGL dependency; apply + persist happen synchronously per committed change.
- **Provider routing (`settings_fpvd.c`):** in `set`/`get`/`set_async` and
  `is_available`, if `pp_runtime_cfg_owns(...)` route to the runtime-config
  module. These keys are **not** staged and do **not** participate in
  `has_pending`/`apply`.
- **Remove from fpvd keymap:** delete the three `gs/dvr/*` entries from the
  `settings_fpvd.c` keymap so they no longer stage through fpvd PATCH/apply.
- **Un-grey color-correction rows:** with `is_available` now returning true for
  `color_correction`, `cc_gain`, `cc_offset`, the rows that were
  `PP_ROW_LOCKED_UNAVAILABLE` become live.
- **Recording lock:** while recording, lock all three DVR rows (Mode, Max file
  size, Re-encode bitrate) via `pp_row_set_locked(PP_ROW_LOCKED_DYNAMIC)`,
  driven by the in-process recording state (`dvr_enabled` / the `dvr.recording`
  fact). Honor the existing lock-gate semantics (early-return on unchanged
  state). Color-correction rows stay live.
- **Apply button:** unchanged — still commits the remaining restart-based
  display rows (screen mode, video scale, RTP jitter) through fpvd.

## Testing

- **Host sim / Catch2 unit tests** (`USE_SIMULATOR=ON`):
  - JSON parse: valid, missing file, malformed, partial/absent fields.
  - Diff-and-apply: only changed fields fire their setters; unchanged fields do
    not.
  - Mapping: gain 25 → 2.5, offset −15 → −0.15, and range endpoints.
- **gsmenu tests:**
  - Keymap: assert `gs/dvr/*` no longer staged; color-correction rows enabled.
  - Recording gate: DVR rows lock when `dvr.recording` is true, unlock when
    false.
- **Manual on GS:**
  - Record via `kill -USR1 $(pidof pixelpilot)`; confirm DVR rows grey out.
  - Tune gain/offset; confirm both the live feed and the recording change.
  - Reboot; confirm values persist from the JSON.

## Risks / call-outs

- **Runtime color-correction toggle while displaying** uses existing functions
  (`gamma_lut_enable`/`gamma_lut_disable` already exist in `drm.c`; the display
  thread already branches on `enable_live_colortrans` per frame). The new code is
  only the thin `pp_colortrans_apply` wrapper that calls them together — verify
  on hardware that toggling enable mid-stream cleanly swaps the OSD compositing
  path (main.cpp:424) without a frame glitch.
- **JSON path** must be confirmed writable + persistent on the GS image (open
  item above) before implementation.
- **Linker note:** the menu (C) calls `extern "C"` functions defined in
  `main.cpp`. The simulator build does not link `main.cpp`, so the runtime-config
  module must compile in the simulator with these calls stubbed/weak — see the
  C-linkage memory note. Plan tests accordingly (logic tested host-side; the
  direct setter calls exercised only in the device build).
