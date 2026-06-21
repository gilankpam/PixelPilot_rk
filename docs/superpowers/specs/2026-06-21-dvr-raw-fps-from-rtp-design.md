# Raw DVR: derive frame timing from RTP timestamps (drop Raw FPS) — Design

- **Date:** 2026-06-21
- **Status:** Approved (pending spec review)
- **Builds on:** the DVR work in PR #7 (`fix/dvr-reencode-timing-and-osd-flicker`): re-encode PTS muxing, the `dvr_*_90k` pure-helper pattern, and `Dvr::frame(frame, pts_ms)`.

## Background

The raw DVR copies the over-the-air H.265 bitstream verbatim into MP4 (`dvr_raw->frame(frame)`, `main.cpp`). Because it never decodes, it has no per-frame timing, so it stamps every frame with a fixed `90000/video_framerate` duration where `video_framerate` is a user-declared value (`--dvr-framerate`, gsmenu "Raw FPS"). This is **required** for raw/both modes (`main.cpp` startup gate) and is a footgun: declare it wrong and the recording plays at the wrong speed.

But the timing is already in the stream. The HEVC depayloader parses the per-frame **RTP timestamp** (`hevc_depayloader.cpp` — `on_payload(..., uint32_t rtp_ts)`, `cur_ts_`) and emits exactly one access unit per frame. RTP video timestamps are in **90 kHz** units — exactly minimp4's duration unit. So each frame's MP4 duration is simply the **delta between consecutive RTP timestamps**, no declared fps needed. The timestamp is currently dropped at the receiver's frame callback.

This mirrors the re-encode PTS fix: re-encode self-times from the GS monotonic clock; raw will self-time from the sender's RTP clock.

## Goals

- Raw DVR frame durations derived from RTP-timestamp deltas — recordings play at correct speed automatically.
- Remove the Raw FPS configuration: the `--dvr-framerate` flag (accept-and-ignore), the gsmenu "Raw FPS" row, and the "must provide --dvr-framerate" startup gate.
- No change to the re-encode path.

## Non-goals

- Re-encode timing (already PTS-based).
- Jitter buffering / reordering (the depayloader/receiver already handle assembly).
- Supporting non-90 kHz RTP video clocks (H.265 RTP standard is 90 kHz; see Risks).

## Decisions

| Aspect | Before | After |
|--------|--------|-------|
| Raw frame duration | fixed `90000/declared_fps` | `rtp_ts − last_rtp_ts` (90 kHz, wrap-safe), clamped |
| First frame / bad delta | n/a | nominal fallback (1/60 s) |
| `--dvr-framerate` | required for raw/both | **accept-and-ignore** (deprecated), warn once |
| gsmenu "Raw FPS" (`rec_fps`) | dropdown | **removed** |
| startup gate | errors if missing in raw/both | **removed** |

## Architecture

### 1. Thread the RTP timestamp to the raw DVR (two callback signatures)

- `HevcDepayloader::FrameCallback`: `void(const uint8_t* au, size_t len)` → `void(const uint8_t* au, size_t len, uint32_t rtp_ts)`. In `flush_au()`, pass `cur_ts_` at both `cb_(...)` call sites.
- `RtpVideoReceiver::NEW_FRAME_CALLBACK`: `void(std::shared_ptr<std::vector<uint8_t>>)` → `void(std::shared_ptr<std::vector<uint8_t>>, uint32_t rtp_ts)`. The receiver forwards the depayloader's `rtp_ts`.
- `main.cpp` `g_video_frame_cb`: accept `uint32_t rtp_ts`; pass it to the raw DVR. (The decoder-feed path ignores it.)

### 2. Raw timing entry on `Dvr`

- Add `void Dvr::frame_rtp(std::shared_ptr<std::vector<uint8_t>> frame, uint32_t rtp_ts_90k)` (or extend the RPC with an `rtp_ts` + a `use_rtp_timing` flag). The raw DVR uses this; the re-encode DVR keeps `Dvr::frame(frame, pts_ms)` unchanged.
- In `RPC_FRAME`: if RTP timing → duration via the new helper; else if `pts_ms >= 0` → re-encode delta (unchanged); else → nominal (legacy, now unused for raw).
- Track `uint32_t last_rtp_ts_` + `bool have_rtp_ts_` on `Dvr`; reset in `start()`/`split()` (so each segment's first frame uses the nominal fallback).

### 3. Pure, host-testable helper

```cpp
// src/dvr_timing.h (next to dvr_frame_duration_90k)
// Raw-DVR per-frame MP4 duration (90 kHz) from consecutive RTP timestamps
// (uint32, 90 kHz, wrap-safe). First frame / duplicate / >1 s gap -> nominal.
inline int dvr_rtp_duration_90k(uint32_t ts, uint32_t last_ts, bool have_last, int fallback_fps) {
    int nominal = (fallback_fps > 0) ? 90000 / fallback_fps : 1500;
    if (!have_last) return nominal;
    uint32_t d = ts - last_ts;                 // wrap-safe forward delta
    if (d == 0 || d > 90000) return nominal;   // duplicate ts / >1 s gap
    return (int)d;
}
```

`fallback_fps` is an internal constant (60) — not user config.

### 4. Config removal

- `--dvr-framerate`: accept-and-ignore with a one-time `spdlog::warn` (still in launch args / config.json).
- Remove the raw-required startup gate (`main.cpp` "must be provided when raw DVR is enabled").
- Remove the gsmenu "Raw FPS" row (`pages/pixelpilot.c` `rec_fps`) + its `settings_fpvd.c` / `settings_dummy.c` entries, and any dead `simulator.c` stub.
- `video_framerate` global: drop, or keep only as the internal fallback default (60).

## Behavior after change

- `raw` / `both`: raw copy with per-frame durations from RTP timestamps → correct playback speed with zero config; broken/missing RTP ts falls back to 60 fps nominal.
- `reencode`: unchanged.

## Testing

- **Host unit tests** (Catch2, like `dvr_fps_cap`/`dvr_timing`): `dvr_rtp_duration_90k` — first frame → nominal; steady 60 fps deltas (1500) pass through; 30 fps (3000); 32-bit wrap across the boundary; duplicate ts (0) → nominal; >1 s gap → nominal; `fallback_fps<=0` → 1500.
- **Device cross-build** gate (the callback-signature changes span device-only files).
- **On-device verification** (the established workflow): record a raw clip; confirm duration ≈ wall-clock and playback speed is correct **without** `--dvr-framerate`; and (timebase sanity) that observed RTP deltas ≈ 1500 at 60 fps. Test `both` mode (raw + reenc) too.

## Risks / open items

- **RTP clock rate assumption (primary risk):** assumes the video RTP clock is 90 kHz (H.265 RTP standard). If the wfb/OpenIPC sender used a different rate, durations would be mis-scaled — verify on-device (deltas ≈ 1500 at 60 fps). If it's ever non-90 kHz, rescale by the SDP clock rate.
- **Sender must stamp RTP timestamps correctly.** Compliant encoders do; a sender with constant/garbage ts falls back to the 60 fps nominal (same worst case as a wrong manual fps, but auto-handled).
- `rec_fps` may be wired only as a GS-staged setting (fpvd), not a direct call — confirm the exact `rec_fps` ↔ `--dvr-framerate` mapping during implementation before removing both ends.
