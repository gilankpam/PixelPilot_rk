# AIOWidget telemetry & layout refinements

**Date:** 2026-06-07
**Status:** Approved (design)
**Builds on:** `docs/superpowers/specs/2026-06-07-aiowidget-design.md` (AIOWidget is already implemented and deployed).

## Problem

Four refinements to the shipped AIOWidget, requested after seeing it run:

1. **VIDEO tile should show the *configured* air mode, not the live decode.** Today VIDEO is `video.height` + measured `video.displayed_frame` rate. It should instead show the resolution + fps that the air unit is *configured* for (from fpvd `GET /air/config`) — a static value like `1080p60` — and update when the user changes resolution/fps in gsmenu.
2. **Add a live FPS tile** beside LATENCY (the measured framerate we're removing from VIDEO shouldn't be lost — it's useful as its own metric).
3. **Fixed layout:** changing digit widths currently shift the whole right-hand metric group horizontally (jitter). The layout should be fixed so numbers changing never move the OSD.
4. **Signal bars should represent RSSI (signal strength), not LINK quality** — the conventional meaning of "signal bars". LINK quality stays as its own number tile.

## Scope

- **In scope:** the four changes above, all within `AIOWidget` (`src/osd.cpp`) + its pure-logic unit (`src/osd_aio_logic.*`), plus one new small in-process "air-info bridge" that publishes air-config facts.
- **Out of scope:** any change to fpvd, to gsmenu's settings/keymap, or to the `video.height`/`video.displayed_frame`/`wfbcli.*` fact *producers*. We only change which facts AIOWidget consumes and add a fact publisher that reuses gsmenu's existing fpvd polling. The two color schemes, antenna aggregation, REC badge, gradient, and scaling are unchanged.

## Architecture overview

Two seams change:

1. **A new air-info bridge** (in the gsmenu/settings layer, same process as the OSD) publishes the configured air resolution + fps as OSD facts, sourced from gsmenu's existing fpvd `GET /air/config` polling.
2. **AIOWidget** re-points two slots, adds one slot (live FPS), repoints the signal bars to RSSI, and switches to a fixed-width tile layout. Three new pure helpers; one removed.

---

## 1. Air-info bridge (new)

gsmenu and the OSD run in the **same pixelpilot process** and share the OSD facts pub/sub system. gsmenu's fpvd backend (`settings_fpvd.c`) already polls `GET /air/config` on a background worker (every ~3 s with the menu open, ~60 s closed, **and immediately after any apply**) into `air_snapshot`, firing a snapshot listener (`notify_listener()` → fanout, dispatched on the LVGL thread) on every refresh. The air resolution/fps are exposed through the public settings API:
- `pp_settings_get("air", "camera", "size")` → resolution string, e.g. `"1920x1080"`
- `pp_settings_get("air", "camera", "fps")` → fps string, e.g. `"60"`

**The bridge** is a small module that, at startup (right after `pp_settings_register_fpvd()`), registers one listener via `pp_settings_set_snapshot_listener(cb, NULL)`. On each fire it:
1. reads the current `air/camera/size` and `air/camera/fps` via `pp_settings_get` (freeing the returned strings),
2. publishes two OSD facts:
   - `air.video.resolution` (string) — raw `WxH`, via `osd_publish_str_fact`
   - `air.video.fps` (int) — parsed from the fps string, via `osd_publish_int_fact`

It uses **only public APIs** (`pp_settings_get` + `osd_publish_*_fact`), so it adds no coupling into `settings_fpvd.c` internals and no new HTTP client. Republishing the same value is harmless (facts overwrite). The listener fires on the LVGL thread, which runs continuously regardless of menu visibility, so the OSD gets fresh values during flight; and it fires right after a gsmenu apply, so **"changed via gsmenu" propagates automatically**.

**Lifecycle/edge cases:**
- Before the first successful poll (boot, or fpvd unreachable), no facts are published → AIOWidget shows `--` for VIDEO (per design decision).
- If `air/camera/size` or `fps` is absent/NULL in the snapshot, skip publishing that fact (leave it undefined).
- The dummy/stub settings providers (host sim) don't poll fpvd, so the bridge simply never fires there — fine.

---

## 2. AIOWidget slot model (changed)

The `Slot` enum and the factory's default tagless-matcher list (must stay 1:1, in order) become **12 entries**:

| idx | Slot | Default fact | Notes |
|----|------|--------------|-------|
| 0 | `SLOT_VIDEO_RES` | `air.video.resolution` (str) | **changed** from `video.height` |
| 1 | `SLOT_VIDEO_FPS` | `air.video.fps` (int) | **changed** from `video.displayed_frame`; now the *configured* fps (static) |
| 2 | `SLOT_FREQ` | `wfbcli.rx.ant_stats.freq` | |
| 3 | `SLOT_PKT_ALL` | `wfbcli.rx.packets.all.delta` | |
| 4 | `SLOT_PKT_LOST` | `wfbcli.rx.packets.lost.delta` | |
| 5 | `SLOT_PKT_FEC` | `wfbcli.rx.packets.fec_rec.delta` | reserved |
| 6 | `SLOT_BITRATE` | `gstreamer.received_bytes` | |
| 7 | `SLOT_LATENCY` | `video.latency.total_ms` | |
| 8 | `SLOT_FPS_LIVE` | `video.displayed_frame` (uint) | **new**; per-second rate via the `fps` RunningAverage |
| 9 | `SLOT_RSSI` | `wfbcli.rx.ant_stats.rssi_avg` | |
| 10 | `SLOT_SNR` | `wfbcli.rx.ant_stats.snr_avg` | |
| 11 | `SLOT_REC` | `dvr.recording` | |

`setFact` changes:
- `SLOT_FPS_LIVE`: `fps.add(getUintValue())` then store the per-second rate as the slot value (this is exactly the rate logic the old `SLOT_VIDEO_FPS` used — it **moves here**). The `fps` `RunningAverage` member is therefore **kept** (Section 2 of the parent spec's "delete fps" note is superseded).
- `SLOT_VIDEO_FPS`: now a static int — stored directly (`default:` branch).
- `SLOT_VIDEO_RES`: string — stored directly (`default:` branch).
- A string accessor `arg_s(idx)` is added: returns the value if defined and `T_STRING`, else `""` (must check type to avoid `Fact::assertType` aborting).

The strip's left group stays `VIDEO`, `WIFI CH`. The right group order becomes:

```
[signal bars (RSSI)]  LINK  BITRATE  LATENCY  FPS  RSSI  SNR
```

---

## 3. Tile rendering changes

**VIDEO tile** (neutral): `res = arg_s(SLOT_VIDEO_RES)`. If empty → value `--`. Else `format_video_mode(res, arg_u(SLOT_VIDEO_FPS))`.

**FPS tile** (new, deviation-colored): `live = arg_u(SLOT_FPS_LIVE)`, `cfg = arg_u(SLOT_VIDEO_FPS)`.
- If the live-fps fact is undefined → value `--`, neutral white.
- Else value = `to_string(live)`; band = `fps_band(live, cfg)`; color = `resolve_color(band, scheme, false)`.
- Label `FPS`, no unit.

**Signal bars** (repointed to RSSI): `rssi = rssi_agg.best(now)`.
- Filled count = `rssi ? rssi_to_bars(*rssi) : 0`.
- Filled color = `rssi ? resolve_color(resolve_band(Rssi, *rssi), scheme, false) : white`; empty bars 26% white (unchanged).
- Position unchanged (left of the right group). No longer uses LINK%.

**LINK tile** unchanged (still `link_quality_pct(window_sum(pkt_all), window_sum(pkt_lost))`, threshold-colored).

---

## 4. Fixed (no-jitter) layout

Today the right group is measured from the **live** values then placed at `W − pad_x − total`, so any digit-width change shifts the whole group. Fix: lay out from **fixed reserved widths** independent of the live value.

- Each `Tile` gains a `reserve` field — a per-metric sample string sized to the widest expected value: LINK `100`, BITRATE `888.8`, LATENCY `888`, FPS `888`, RSSI `-888`, SNR `88`; left group VIDEO `1080p120`, WIFI CH `8888`.
- `measure_tile` and `draw_tile` compute the value-field width from `reserve` (not the live value). The live value is rendered within that fixed field; the unit (if any) is drawn at a fixed offset after the reserved value field. Tile width = `pad + max(rail_w, value_field + unit_field) + pad`.
- Result: every tile's width — and therefore the right group's total width and left edge, and each tile's value/unit positions — is constant. Numbers changing never move anything.
- Value alignment within the reserved field defaults to left-aligned (matching the handoff); left-vs-right is a visual detail to settle during on-device tuning.
- The signal-bars cluster is already fixed width (5 bars). The REC badge is already stable (`HH:MM:SS` is always 8 chars).

---

## 5. Pure-logic unit changes (`osd_aio_logic`, host-tested)

**Add:**
```cpp
// "1920x1080", 60 -> "1080p60". Parse height from WxH; if not WxH-parseable,
// return resolution unchanged; if fps <= 0, omit the fps suffix; empty -> "".
std::string format_video_mode(const std::string& resolution, int fps);

// RSSI dBm -> 0..5 bars, linear: -90 -> 0, -55 -> 5, clamped.
int rssi_to_bars(int rssi_dbm);

// Live vs configured fps -> band. configured <= 0 -> Neutral (no reference).
// ratio >= 0.90 -> Good; >= 0.70 -> Warn; else Crit.
Band fps_band(int live_fps, int configured_fps);
```

**Remove:** `int signal_bar_count(int)` and its test — the signal bars now use `rssi_to_bars`, and LINK% is shown as a number via `link_quality_pct` (which stays). No remaining caller.

---

## Data flow (end to end)

```
fpvd GET /air/config (gsmenu worker poll + post-apply)
  → air_snapshot → notify_listener (LVGL thread)
    → air-info bridge: pp_settings_get(air/camera/size, air/camera/fps)
      → osd_publish_str_fact("air.video.resolution"), osd_publish_int_fact("air.video.fps")
        → AIOWidget SLOT_VIDEO_RES / SLOT_VIDEO_FPS → VIDEO tile "1080p60"

video.displayed_frame → SLOT_FPS_LIVE (RunningAverage rate) ─┐
air.video.fps         → SLOT_VIDEO_FPS ─────────────────────┴→ fps_band → FPS tile color

wfbcli.rx.ant_stats.rssi_avg → rssi_agg.best → rssi_to_bars + RSSI band → signal bars
```

## Testing

- **Host unit tests (`aio_logic_tests`, tag `[aio]`):**
  - `format_video_mode`: `1920x1080`/60→`1080p60`, `1280x720`/120→`720p120`, `960x540`/60→`540p60`, fps≤0→`1080p`, non-WxH (`"foo"`)→`foo`, empty→`""`.
  - `rssi_to_bars`: `-55`→5, `-90`→0, `-40`→5 (clamp), `-100`→0 (clamp), `-62`→4, `-70`→3, `-80`→1 (values per the linear map; adjust expectations if endpoints are tuned).
  - `fps_band`: configured 0→Neutral; 60/60→Good; 54/60→Good (0.90); 53/60→Warn; 42/60→Warn (0.70); 41/60→Crit.
  - Remove the `signal_bar_count` test.
- **Cross-build + on-device** (verification is visual): change resolution/fps in gsmenu → VIDEO tile updates to the new `WxHp<fps>` within a refresh; FPS tile tracks live framerate and colors by deviation; signal bars track RSSI and recolor with the RSSI tile; confirm numbers changing never shift the layout; VIDEO shows `--` before fpvd connects.

## Files touched

- `src/osd_aio_logic.hpp` / `.cpp` — add `format_video_mode`, `rssi_to_bars`, `fps_band`; remove `signal_bar_count`.
- `tests/test_aio_logic.cpp` — add tests for the three new functions; remove the `signal_bar_count` test.
- `src/osd.cpp` — `AIOWidget`: rename `SLOT_VIDEO_H`→`SLOT_VIDEO_RES`, add `SLOT_FPS_LIVE`, repoint slot facts in the factory default matchers, `setFact` changes, `arg_s` accessor, FPS tile, signal-bars-from-RSSI, fixed-width tile layout (`reserve`).
- New air-info bridge: `src/gsmenu/osd_air_bridge.{c,h}` (or equivalent), registered at gsmenu/settings init; added to the relevant CMake source lists (the real `pixelpilot` target; not the host logic test).
- README — note VIDEO now reflects the configured air mode + the new FPS tile + bars = RSSI.

## Notes / risks

- **Bridge thread:** the snapshot listener dispatches on the LVGL thread; `osd_publish_*_fact` is thread-safe (queue + mutex), so publishing from there is fine. If, in practice, the listener does not fire with the menu closed, the fallback is to publish directly from `refresh_snapshot_unlocked()` in `settings_fpvd.c` (worker thread) — same facts, at the cost of a small coupling to `osd.h`.
- **rssi_to_bars endpoints** (−90/−55) and the **fps_band ratios** (0.90/0.70) are first-pass values; expect to tune them once seen on the goggle.
