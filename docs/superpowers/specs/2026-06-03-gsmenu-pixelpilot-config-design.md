# gsmenu ⇆ fpvd-GS PixelPilot config integration

- **Date:** 2026-06-03
- **Status:** Approved (design)
- **Component:** `src/gsmenu` settings backend + pages (PixelPilot_rk)
- **Depends on:** fpvd-GS `pixelpilot` config API — `fpvd/docs/api.md` "PixelPilot managed service"
  (`PATCH /config` → `pixelpilot.*`, `POST /apply`, `GET /config`). Deployed & verified on the GS.

## Problem

fpvd-GS now spawns/supervises the `pixelpilot` binary and models its GS-local launch
knobs in a `pixelpilot` config block reached via `PATCH /config` + `POST /apply`. The
gsmenu's settings backend (`settings_fpvd.c`) routes drone settings through `/air/*` and
GS radio through `/link`, but has **no route for the pixelpilot config** — so the
Display (Video Scale) and DVR rows are unbacked placeholders on device (they fall back to
the dummy provider and read "unknown"). This wires them up.

A pixelpilot config change restarts pixelpilot — a brief (~2 s) video blackout. Applying
each row change individually would blackout once *per row*, so changes must be **staged
and applied together**.

## Goal

1. **Route pixelpilot config through fpvd-GS** — add a third endpoint group so the
   Display/DVR rows write to `PATCH /config` (`pixelpilot.*`) and read from `GET /config`.
2. **One PixelPilot page** — consolidate the separate Display and DVR tabs into a single
   **PixelPilot** tab with Display + DVR sections; add **Screen Mode** and **RTP Jitter**
   rows.
3. **Stage → Apply** — pixelpilot rows stage into fpvd's pending (`PATCH /config`, no
   apply); a single **Apply** button at the bottom commits the batch (`POST /apply` → one
   pixelpilot restart).
4. **Mark un-backable rows unavailable** — color correction and DVR "Enabled" stay
   visible but greyed (they have no fpvd lever this round).

## Non-goals

- Modeling color correction (`color_correction`/`cc_gain`/`cc_offset`) or DVR "Enabled".
  Color correction is an in-process GL shader with no pixelpilot CLI lever; DVR enable is
  a runtime SIGUSR1 toggle. Both stay visible-but-unavailable; their backends are a
  separate effort.
- Hotspot / restream / telemetry rows — out of scope (greyed by the general unavailable
  rule, same as today's non-functional state).
- A per-page "discard staged changes" action (`POST /reset` wipes *all* GS pending, not
  just pixelpilot — unsafe to expose here).
- Dynamic screen-mode enumeration via pixelpilot `--screen-mode-list` (hardcoded list now).

## Decisions (resolved during brainstorming)

| Decision | Choice |
|---|---|
| Scope | Video Scale + all 8 DVR rows, plus new Screen Mode + RTP Jitter |
| Un-backable rows | Keep visible, mark unavailable (greyed + hint) |
| Unavailable rule | **General**: a row with no keymap entry is unavailable (self-maintaining); also greys hotspot/restream/telemetry |
| Read snapshot | Third `GET /config` fetch (GS-local, cheap); reads from **pending** |
| Re-enc Resolution | Dropdown → `1080p`/`720p` (pixelpilot's real values; drop bogus `854x480`) |
| Video Scale range | Cap slider at `50–100` (pixelpilot rejects factor > 1.0) |
| Page layout | One **PixelPilot** tab; sections Display + DVR (Recording/Re-encode/Overlay) |
| Apply model | Stage (`PATCH` only) + a single **Apply** button (`POST /apply`) |

## Architecture — third endpoint group `EP_CONFIG`

`settings_fpvd.c` already abstracts routing behind `fpvd_endpoint_t` + three pure path
helpers. The integration adds one group, mirroring how `EP_LINK` was added.

### Endpoint routing

`settings_fpvd_internal.h`: add `FPVD_EP_CONFIG` to `fpvd_endpoint_t`. Change the three
path helpers to take a `fpvd_endpoint_t` (they only use `entry->endpoint` today) so
`run_job_unlocked` can reuse them instead of its inline `LINK ? … : …` ternary.

| Helper | `EP_AIR` | `EP_LINK` | `EP_CONFIG` |
|---|---|---|---|
| `fpvd_write_path` | `/air/config` | `/link` | `/config` |
| `fpvd_apply_path` | `/air/apply` | `/link/apply` | `/apply` |
| `fpvd_read_path` | `/air/config` | `/link` | `/config` |

### Keymap (11 `EP_CONFIG` rows)

```c
/* PixelPilot launch config → fpvd /config (pixelpilot.*); staged, applied on demand */
{ "gs","display","screen_mode",       "pixelpilot.screenMode",          FPVD_T_STRING,          FPVD_EP_CONFIG, NULL },
{ "gs","display","video_scale",       "pixelpilot.videoScale",          FPVD_T_PERCENT_TO_FRAC, FPVD_EP_CONFIG, NULL },
{ "gs","display","rtp_jitter_ms",     "pixelpilot.rtpJitterMs",         FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_mode",              "pixelpilot.dvr.mode",            FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
{ "gs","dvr","rec_fps",               "pixelpilot.dvr.framerate",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_max_size",          "pixelpilot.dvr.maxSizeMb",       FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_reenc_codec",       "pixelpilot.dvr.reencCodec",      FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_reenc_resolution",  "pixelpilot.dvr.reencResolution", FPVD_T_ENUM,            FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_reenc_fps",         "pixelpilot.dvr.reencFps",        FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_reenc_bitrate",     "pixelpilot.dvr.reencBitrate",    FPVD_T_INT,             FPVD_EP_CONFIG, NULL },
{ "gs","dvr","dvr_osd",               "pixelpilot.dvr.osd",             FPVD_T_BOOL,            FPVD_EP_CONFIG, NULL },
```

No new conversion types: `video_scale` reuses `FPVD_T_PERCENT_TO_FRAC` (UI 100 ↔ JSON 1.0);
`dvr_reenc_bitrate` is plain `INT` (dropdown values are bare kbps, matching the JSON);
`dvr_reenc_resolution` is `FPVD_T_ENUM` once the dropdown emits `1080p`/`720p`;
`screen_mode` is `FPVD_T_STRING` (sent verbatim, read back as-is).

Rows keep their `gs/display/*` and `gs/dvr/*` triples even though they render on one page
— the keymap looks up each row's explicit triple, independent of the visual page.

### Reads — `config_snapshot` from `GET /config?pending=true`

Add `cJSON *config_snapshot` to `fpvd_state_t`; in `refresh_snapshot_unlocked`, add a
third fetch **`GET /config?pending=true`** alongside `/link` + `/air/config`. It returns
the full GS config, so `pixelpilot.screenMode` / `pixelpilot.dvr.*` resolve **directly**
(no `{"link":…}`-style wrapping). `prov_get` becomes a 3-way snapshot select
(`LINK→gs_snapshot`, `CONFIG→config_snapshot`, else `air_snapshot`).

**Why pending, not effective:** with deferred apply a staged value is in *pending* but not
yet *effective*. Reading effective would make a just-changed row snap back to the old
value on the next refresh. Reading pending shows staged changes immediately and stays
correct after apply (pending == effective). It's a GS-local call (no drone round-trip),
so the extra fetch is cheap.

### Apply model — stage then Apply

- **Stage:** for `EP_CONFIG` jobs, `run_job_unlocked` does **`PATCH /config` only** — it
  skips the apply step. (AIR/LINK keep immediate PATCH+apply.) Each pixelpilot row change
  accumulates in fpvd's pending.
- **Apply:** a new provider method `apply` + `pp_settings_apply()` wrapper enqueues an
  **apply-only job** (a job with `apply_only = true`); `run_job_unlocked` for such a job
  skips the PATCH and does `POST /apply`. fpvd's granular apply restarts **only**
  pixelpilot (one ~2 s blackout for the whole batch; the radio link is untouched).
- **Apply button:** a full-width action row at the very bottom of the PixelPilot page
  (below all sections), label **"Apply changes"**, using the existing action-row pattern
  (cf. System's Reboot rows). Press → busy spinner → `pp_settings_apply()` → success/error
  toast. Validation errors surface here (fpvd runs deep validation at `/apply`, not at
  `PATCH`) via the existing `parse_error_message`.
- **Dirty hint:** a lightweight local "•" on the Apply row, set when any pixelpilot row is
  changed this session, cleared on a successful apply. (Authoritative pending-vs-effective
  diffing is a future refinement; the local flag covers the change-then-apply flow.)

Only `pixelpilot.*` stages through the gsmenu, so `POST /apply` restarts only pixelpilot.
Staged-but-unapplied changes persist in fpvd's pending across menu sessions (applied on the
next Apply press); there is no per-page discard.

## Page consolidation

New `src/gsmenu/pages/pixelpilot.c` (`build_pixelpilot_tab`) creates one page
(`pp_page_create(parent, "gs", "pixelpilot")`) and absorbs the Display + DVR rows into
sections, plus the Apply row:

```
PixelPilot
─ Display ──────────────
  Screen Mode        (gs/display/screen_mode)      dropdown WxH@fps
  Video Scale        (gs/display/video_scale)      slider 50–100
  RTP Jitter (ms)    (gs/display/rtp_jitter_ms)    slider 0–50
  Color correction   (gs/display/color_correction) — unavailable
  Gain               (gs/display/cc_gain)          — unavailable
  Offset             (gs/display/cc_offset)        — unavailable
─ DVR · Recording ──────
  Enabled            (gs/dvr/rec_enabled)          — unavailable
  Mode               (gs/dvr/dvr_mode)             raw|reencode
  Raw FPS            (gs/dvr/rec_fps)              30|60|90|120
  Max file size (MB) (gs/dvr/dvr_max_size)         slider 100–16000
─ DVR · Re-encode ──────
  Codec              (gs/dvr/dvr_reenc_codec)      h264|h265
  Resolution         (gs/dvr/dvr_reenc_resolution) 1080p|720p
  Re-encode FPS      (gs/dvr/dvr_reenc_fps)        30|60
  Bitrate (kbps)     (gs/dvr/dvr_reenc_bitrate)    4000|8000|12000|16000|25000
─ DVR · Overlay ────────
  Burn OSD into DVR  (gs/dvr/dvr_osd)              toggle
─ Playback ─────────────
  Browse recordings… (drilldown, read-only — unchanged)
─────────────────────────
  [ Apply changes • ]    (action row; POST /apply)
```

`menu.c` replaces the two `build_display_tab` + `build_dvr_tab` calls/tabs with one
`build_pixelpilot_tab` → tabbar **6 → 5** items (`Camera, Link, DLink, PixelPilot,
System`). Delete `pages/display.{c,h}` and `pages/dvr.{c,h}`.

**Screen Mode dropdown** (hardcoded common modes): `1920x1080@60`, `1920x1080@120`,
`1280x720@60`, `1280x720@120`, `2560x1440@60`, `3840x2160@60`.

## Unavailable mechanism (general rule)

- `settings.h`/`settings.c`: add `is_available(d,p,k)` to `pp_settings_provider_t` and a
  `pp_settings_is_available()` forwarding wrapper (defaults to `true` when a provider
  omits the method).
- `settings_fpvd.c`: `is_available` = `fpvd_keymap_lookup(d,p,k) != NULL`.
  `settings_dummy.c`: returns `true` (sim keeps all rows live); extend the dummy seed with
  the new pixelpilot keys.
- `widgets/pp_row.{h,c}`: add `PP_ROW_LOCKED_UNAVAILABLE` to `pp_row_lock_t` (greyed +
  a one-line hint).
- `helper.c` row-walk + `widgets/pp_dropdown.c`/`pp_toggle.c`/`pp_slider.c`: check
  availability first — `!available → PP_ROW_LOCKED_UNAVAILABLE` (and skip wiring the
  change handler); else fall through to the existing offline / dynamic-lock / unlocked
  states.

On device this greys the color trio + DVR Enabled (no keymap entry) and the other still-
unbacked rows (hotspot/restream/telemetry) — honest, and each un-greys the instant it
gains a keymap entry.

## Errors & edge cases

| Case | Behavior |
|---|---|
| Bad value at apply (rare; widgets are constrained) | `POST /apply` 400 → `parse_error_message` shows the field message on the Apply toast |
| GS unreachable | Existing transport-failure path: `connected=false`, error toast |
| `EP_CONFIG` + dynamic-link | Never locked (the lock check only fires for `EP_AIR`) |
| Navigate away with staged changes | Pending persists in fpvd; applied on next Apply press; no auto-discard |
| Rapid edits to the same row | Coalesced in the queue by path+endpoint (existing behavior) |

## Tests

Extend the existing `settings_fpvd` unit tests (no host I/O):

- **Keymap:** `gs/display/{screen_mode,video_scale,rtp_jitter_ms}` and `gs/dvr/*` resolve
  to `EP_CONFIG` with the correct `pixelpilot.*` paths and types.
- **Path helpers:** `EP_CONFIG` → `/config`, `/apply`, `/config`; AIR/LINK unchanged.
- **build_patch_body:** `("pixelpilot.dvr.reencBitrate","8000",INT)` →
  `{"pixelpilot":{"dvr":{"reencBitrate":8000}}}`.
- **Snapshot read:** `pixelpilot.dvr.osd=true` → `"on"`; `pixelpilot.videoScale=1.0`
  (PERCENT_TO_FRAC) → `"100"`; `pixelpilot.screenMode="1920x1080@60"` → verbatim.
- **is_available:** `true` for a mapped key; `false` for `gs/display/color_correction`.
- **Apply-only job:** an apply job issues `POST /apply` and no `PATCH`.

(Manual on-device: change several pixelpilot rows → rows show staged values → press Apply
→ one pixelpilot restart, all changes live; verify wfb_rx/wfb_tx PIDs unchanged.)

## Files

**New**
- `src/gsmenu/pages/pixelpilot.c` + `pixelpilot.h` — `build_pixelpilot_tab`.
- Test additions in the `settings_fpvd` test target.

**Delete**
- `src/gsmenu/pages/display.c` + `display.h`
- `src/gsmenu/pages/dvr.c` + `dvr.h`

**Modify**
- `src/gsmenu/settings_fpvd_internal.h` — `FPVD_EP_CONFIG`; path-helper signatures.
- `src/gsmenu/settings_fpvd.c` — keymap (+11); path helpers 3-way; `config_snapshot` +
  `GET /config?pending=true`; `prov_get` 3-way; `run_job_unlocked` (helpers; `EP_CONFIG`
  PATCH-only; apply-only job); `apply` + `is_available` methods.
- `src/gsmenu/settings.h` / `settings.c` — `apply` + `is_available` on the provider;
  `pp_settings_apply()` / `pp_settings_is_available()` wrappers.
- `src/gsmenu/settings_dummy.c` — `is_available`/`apply` stubs; seed new pixelpilot keys.
- `src/gsmenu/widgets/pp_row.{h,c}` — `PP_ROW_LOCKED_UNAVAILABLE`.
- `src/gsmenu/helper.c` — availability in the row-walk.
- `src/gsmenu/widgets/pp_dropdown.c` / `pp_toggle.c` / `pp_slider.c` — availability check.
- `src/menu.c` — one PixelPilot tab (6→5).
- `CMakeLists.txt` — swap `display.c`/`dvr.c` → `pixelpilot.c` across device/sim/test targets.

## Known limitations / out of scope

- Color correction, DVR Enabled, hotspot/restream/telemetry remain unavailable (greyed)
  until their backends exist.
- Screen-mode list is hardcoded (no `--screen-mode-list` enumeration yet).
- Dirty indicator is a local session flag, not an authoritative pending-vs-effective diff.
- No per-page discard of staged changes (global `/reset` only).
