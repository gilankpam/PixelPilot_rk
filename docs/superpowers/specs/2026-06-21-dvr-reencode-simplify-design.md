# DVR Re-encode Simplification — Design

- **Date:** 2026-06-21
- **Status:** Approved (pending spec review)
- **Builds on:** branch `fix/dvr-reencode-timing-and-osd-flicker` (PTS muxing, buffer-race fix, single-pass `imcomposite`)

## Background

The re-encode DVR exposes four knobs — codec, resolution, fps, bitrate — via `--dvr-reenc-*` CLI flags and gsmenu rows, with live-change handlers and a constant-fps pacer (`FrameProcessor` timer thread with repeat / grace / re-anchor logic).

In practice the ground station records at the input's codec/res/fps, so most of that configurability adds complexity for no benefit. With the recent fixes in place, the pipeline is: decoder → FrameProcessor (resize + OSD composite, paced to a constant fps) → MppEncoder (h265) → DVR mux. This redesign collapses the configuration to a single bitrate knob and removes the constant-fps pacer.

## Goals

- Reduce re-encode configuration to **one** setting (bitrate).
- Re-encode output: **h265**, at **screen resolution**, at **min(input, display-refresh)** fps, with the **OSD always burned in**.
- Simplify `FrameProcessor` by removing the constant-fps pacer.
- No regression to the three recent fixes (correct duration, no OSD flicker, single-pass composite).

## Non-goals

- The raw DVR path is unchanged.
- High-fps / slow-motion recording above the display refresh (explicitly capped).
- VBR rate control (stays CBR).

## Decisions

| Setting | Before | After |
|---------|--------|-------|
| codec | configurable (default h264) | fixed **h265** |
| resolution | 720p / 1080p setting | **screen resolution** (`mode.hdisplay × mode.vdisplay`) |
| fps | configurable target | **min(input, display refresh)**; cap = `mode.vrefresh` |
| bitrate | configurable (8 Mbps) | **configurable, default 8 Mbps** (kept) |
| OSD | `--dvr-osd` toggle | **always on** for re-encode |

Rationale: once codec/res/fps are pinned to the input/screen, re-encode's only remaining job is OSD burn-in, so the OSD toggle is meaningless — an OSD-free copy still comes from `raw` (or the raw side of `both`). Bitrate is the one genuine quality/size lever and stays.

## Architecture

### Collapsed pacer (Approach A)

Replace the two-thread `FrameProcessor` (processor + 60 Hz timer with repeats/grace/re-anchor) with a single loop driven by decoded-frame arrival:

```
on decoded frame (push_latest):
    if (now - last_emit) < (1 / cap_fps):   # fps cap / decimator
        drop frame; continue
    resize decoded -> screen-res NV12        # RGA; scales input -> screen
    composite OSD over it                     # single-pass imcomposite (OSD is screen-res, matches)
    push to encoder                           # fresh pool buffer (keep buffer-race-fix handoff)
    advance last_emit by 1/cap_fps            # accumulator, not "= now", to hit cap rate cleanly
```

Removed: `__TIMER_THREAD__` / `timer_loop`, repeat logic, grace/re-anchor, and the `last_copy` / `ready_fresh_` / swap machinery (all of which existed only to feed a constant rate). Kept: the buffer-race fix's "hand each composited frame to the encoder as its own pool buffer, never reuse in flight."

Output is variable-fps (≤ cap); the existing PTS-delta muxing keeps the recorded duration correct.

### fps cap / decimator

- Cap = `output_list->mode.vrefresh` (60 here). Always ≤ the encoder's ~75 fps ceiling at 1080p, so the encoder never backs up.
- Accumulator-based decimation (`next_allowed += 1/cap`) so e.g. 90 → 60 decimates accurately without the undershoot a naive "reset to now" throttle would cause; input ≤ cap passes through unchanged.
- Extracted as a **pure, host-testable helper** (mirrors `dvr_timing.h`).

### resolution

- Encode target = screen mode dims. The resize (RGA) scales the decoded input → screen res. The OSD is rendered at screen res, so it matches the frame 1:1 → the single-pass `imcomposite` is always valid (no OSD scaling / size fallback needed).
- Drop `EncResolution`, `set_resolution`, `target_res_`.

### codec / encoder

- Hardcode h265. Encoder rate-control fps = the cap (drives GOP = fps×2 and CBR distribution). CBR at the bitrate setting.

### OSD always-on

- Remove the `dvr_osd` global, `--dvr-osd`, `dvr_reenc_set_osd` / `dvr_reenc_get_osd`. The OSD thread publishes to the recorder whenever the re-encode pipeline exists: gate `set_osd_blend` on `frame_proc != nullptr` (instead of `dvr_osd && frame_proc`).

### Config surface

- **CLI keep:** `--dvr-mode raw|reencode|both`, `--dvr-reenc-bitrate` (default 8000), `--dvr-max-size`, `--dvr-framerate` (raw path).
- **CLI remove:** `--dvr-reenc-codec`, `--dvr-reenc-fps`, `--dvr-reenc-resolution`, `--dvr-osd`.
- **Compatibility:** the removed flags are accepted-and-ignored (one-time deprecation log) so existing launchers / `/etc/fpvd/config.json` still start. The GS config is cleaned up separately (fpvd repo / device).
- **gsmenu:** remove the codec/resolution/fps/osd rows, their `settings_fpvd.c` entries, and the corresponding C control functions.

## Behavior after change

- `raw`: unchanged — raw air stream, no OSD.
- `reencode`: h265, screen-res, ≤ display-refresh VFR, OSD burned in, CBR @ bitrate.
- `both`: raw (no OSD) + reencode (OSD).

## Testing

- **Host unit tests** (Catch2, like `dvr_timing_tests`): the fps-cap/decimator pure logic — 90→60 decimation cadence, ≤cap pass-through, first-frame, accumulator behavior across jitter.
- **Device verification** (established workflow — see `reference_dvr_recording_test` memory): record a clip via SIGUSR1; confirm fps ≈ min(input, 60), OSD present on every frame (0 drops via the geq green-coverage check), duration ≈ monotonic wall-clock, OSD visually correct. Cross-build is the device compile+link gate.

## Risks / open items

- Fusing resize + composite into a single `imcomposite` (src channel scales input→screen) is an optional further optimization — not required for this change.
- "Accept-and-ignore removed flags" leaves a little dead parsing; the alternative is to also update the fpvd `config.json` in the same effort (out of this repo).
