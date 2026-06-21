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

1. **Ownership (B):** pixelpilot owns the JSON. gsmenu writes it directly and
   signals pixelpilot — fpvd is not involved for these six settings. Keeps the
   whole change self-contained in the PixelPilot_rk repo.
2. **Reload trigger (A):** SIGHUP. gsmenu writes the file, then
   `kill -HUP $(pidof pixelpilot)`. A handler sets a reload flag; the existing
   1 Hz main loop re-reads and applies. Mirrors the existing SIGUSR1 pattern.
3. **Color-correction target (B):** both live display and re-encode recording.
4. **Apply UX (A):** live apply. Each change writes the JSON + sends SIGHUP
   immediately; gain/offset slider moves are debounced (~200 ms, plus a final
   write on release).
5. **Recording gate:** while recording, all three DVR rows are greyed out in
   gsmenu; color-correction rows stay live.
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

New small config module (load/parse/serialize-diff) plus wiring in `main.cpp`.

- **Startup:** parse the JSON; seed the existing globals `dvr_mode`,
  `dvr_max_file_size`, `reenc_params.bitrate_kbps`, `enable_live_colortrans`,
  `live_colortrans_gain`, `live_colortrans_offset`. Remove the four CLI flags
  (`--dvr-mode`, `--dvr-max-size`, `--dvr-reenc-bitrate`, `--live-colortrans`)
  and their arg-parsing.
- **SIGHUP handler:** set a `reload_config` flag (pattern mirrors the existing
  SIGUSR1 handler at main.cpp:493).
- **1 Hz main loop (main.cpp:883):** when the flag is set, re-read the JSON,
  diff against current in-memory values, and call setters **only** for fields
  that changed:
  - DVR → existing runtime setters `dvr_set_mode()`, `dvr_reenc_set_bitrate()`,
    `dvr_set_max_size()`.
  - Color correction, re-encode path → existing
    `set_color_correction()` / `set_color_correction_enabled()` (via the
    `dvr_reenc_notify_colortrans` style C interface).
  - Color correction, **live display → new runtime gamma-LUT update path**:
    push a fresh CRTC `GAMMA_LUT` derived from gain/offset; toggling `enabled`
    installs the LUT or restores an identity LUT. The current code only sets the
    gamma LUT once at startup (main.cpp:1423), so this is the main net-new code.
- **Robustness:** malformed or missing JSON on reload → log and keep current
  values (no crash, no reset to defaults).
- DVR field changes are only ever applied when not recording (the gsmenu gate
  prevents mid-recording edits; the `dvr_set_mode()` setter would otherwise
  force-stop an active recording).

## Launch path

- `debian/pixelpilot-rk.pixelpilot.service` — drop the four removed flags from
  `ExecStart`. The `DVR_*` env vars in `debian/pixelpilot-rk.pixelpilot.default`
  become unused.
- Package ships the default `runtime.json` at the chosen path.

## gsmenu side

- **Remove from fpvd staging:** delete the `gs/dvr/*` entries from the
  `settings_fpvd.c` keymap so these rows no longer route through fpvd
  PATCH/apply.
- **Un-grey color-correction rows:** the three rows
  (`color_correction`, `cc_gain`, `cc_offset`) were `PP_ROW_LOCKED_UNAVAILABLE`;
  make them live.
- **Write path:** on change, write the JSON atomically (temp file + `rename`),
  then `kill -HUP $(pidof pixelpilot)`. Gain/offset slider moves are debounced
  (~200 ms, plus a final write on slider release).
- **Recording lock:** subscribe to the `dvr.recording` OSD fact; while true,
  lock all three DVR rows (Mode, Max file size, Re-encode bitrate) via
  `pp_row_set_locked`; unlock when it clears. Honor the existing lock gate
  semantics (early-return on unchanged state per the gsmenu row-disable gate).
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

- **Live-display runtime gamma-LUT update** is the main new, untested-in-this-
  codebase path; everything else reuses existing setters.
- **JSON path** must be confirmed writable + persistent on the GS image (open
  item above) before implementation.
