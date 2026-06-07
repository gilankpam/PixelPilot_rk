# AIOWidget: all-in-one ground-station link OSD overlay

**Date:** 2026-06-07
**Status:** Approved (design)

## Problem

The ground-station OSD currently composes its link telemetry from ~9 individual
widgets in `osd.json` (a `BoxWidget` background, `VideoWidget`, `VideoBitrateWidget`,
two G2G/latency `IconTplTextWidget`s, `DvrStatusWidget`, a second `BoxWidget`, and the
`RSSI`/`SNR` `TplTextWidget`s). It works, but it's a pile of hand-placed boxes with no
shared visual language, no threshold coloring, and a lot of `osd.json` boilerplate the
user must position by hand.

The `design_handoff_gs_osd/` bundle specifies a single cohesive "racing-HUD" overlay: a
bottom telemetry strip (VIDEO, WIFI CH, signal bars, LINK, BITRATE, LATENCY, RSSI, SNR
over a faint gradient rail) plus a top-right blinking REC badge, with per-metric
green/amber/red threshold recoloring. We want to ship that as **one** widget —
`AIOWidget` ("all-in-one") — that the user drops into `osd.json` with essentially no
configuration, in **two color schemes**: `white` (monochrome) and `accent`
(threshold palette).

## Scope

- **In scope:** a new `AIOWidget` Cairo widget rendering the full handoff design
  (bottom strip + REC badge), two color schemes, automatic telemetry discovery
  (no antenna tags in JSON), unit tests for the logic-heavy helpers.
- **Out of scope:**
  - Migrating the OSD to LVGL. The handoff README assumes LVGL, but the live overlay
    (`src/osd.cpp`) is Cairo; `lvosd.c` is an empty stub. We build a Cairo widget.
  - The unrelated history graphs (`BarChartWidget` for packets/stutter/latency), the
    `PopupWidget` custom message, and `ExternalSurfaceWidget` (msposd). These can
    coexist in `osd.json` alongside AIOWidget; they are not replaced.
  - Publishing a new upstream `link_quality` fact from `wfbcli`/`fpvd`. LINK % is
    derived in-widget from packet stats instead.
  - Fixing the latent tagged-matcher bug in `FactMatcher::matches()` (see Notes).
    AIOWidget uses only tagless matchers, so it never exercises that path.

## Architecture

A new class `AIOWidget : public Widget` in `src/osd.cpp`, registered in the
`Osd::loadConfig` factory (`type == "AIOWidget"`). It integrates exactly like every
other widget: it draws via `draw(cairo_t *cr)` and receives telemetry through the
existing `FactMatcher` → `setFact(idx, fact)` mechanism.

**Self-anchoring.** AIOWidget owns the whole frame. `pos_x/pos_y` default to `0,0` and
are not used for placement; internally the widget reads the live surface dimensions
(`cairo_get_target` → width/height, already available via the base class) and anchors
the bottom strip to the screen bottom and the REC badge to top-right. It therefore
adapts to the actual panel resolution.

**Factory defaulting.** The generic factory path requires `x`, `y`, and `facts`
(it calls `widget_j.at(...)` which throws if absent). AIOWidget is special-cased so
those are optional:
- `x`/`y` default to `0`.
- When `facts` is omitted, the factory injects AIOWidget's **default matcher set**
  (all tagless — see Data model). When `facts` *is* present, it overrides the defaults
  (escape hatch for unusual setups).

**Internal decomposition (single class, focused helpers).** `draw()` delegates to small,
independently testable units:

| Helper | Responsibility |
|---|---|
| `ThresholdBand resolve_band(Metric, value)` | value → `GOOD`/`WARN`/`CRIT` per the handoff tables |
| `cairo color resolve_color(band, scheme, is_neutral)` | band + scheme → final RGBA (mono vs palette) |
| `draw_metric_tile(cr, x, slot)` | accent rail + label + value + unit, baseline-aligned |
| `draw_signal_bars(cr, x, lq)` | 5 bars; fill count = `round(lq/100 × 5)` |
| `draw_rec_badge(cr)` | blink timer + local `HH:MM:SS` timecode + dot + "REC" |
| `draw_gradient_rail(cr)` | the single 2-stop bottom gradient |
| `draw_text_shadow(cr, ...)` | dark offset copy behind each label for legibility |

**Widget state:** latest value per scalar slot; a `map<ant_id → {value, last_seen_ms}>`
for each antenna-aggregated metric (RSSI/SNR/freq); REC blink phase + last-toggle
timestamp; recording-start timestamp for the local timecode. All timestamps use the
GS-local monotonic clock (the GS↔drone clocks are unsynchronized; every time/latency
value in this app is GS-local).

## Data model & fact mapping

AIOWidget's default matchers are **all tagless** (name only). Per `FactMatcher::matches()`
(src/osd.cpp:551), a tagless matcher returns `true` for every fact with that name,
regardless of tags — so one matcher receives every antenna's value, and each delivered
`Fact` still carries its own `id` + `ant_id` tags (`fact.getTags()`), which the widget
reads to bucket per antenna.

| Slot | Default fact(s) (tagless) | Handling |
|---|---|---|
| **VIDEO** `1080p60` | `video.height`, `video.displayed_frame` | `"%dp%d"` (height, fps). fps is per-second-derived using the existing windowed-bucket accumulator (the `VideoWidget` approach). Neutral (always white). |
| **WIFI CH** `149` | `wfbcli.rx.ant_stats.freq` | MHz→channel in-widget: 5 GHz `(freq−5000)/5`; 2.4 GHz `(freq−2407)/5`. Outside known bands → show raw MHz. Neutral. Value taken from any live antenna (all equal). |
| **LINK** `92 %` | `wfbcli.rx.packets.all.delta`, `…lost.delta`, `…fec_rec.delta` | `lq = clamp(0..100, round(100 × (all − lost) / max(all,1)))` over a short rolling window (reuse the `Bucket`/`Stats` accumulator, src/osd.cpp:609–764). Drives the LINK value **and** the signal-bar fill. |
| **BITRATE** `24.5 Mb/s` | `gstreamer.received_bytes` | Per-second sum → Mb/s (the `VideoBitrateWidget` math). |
| **LATENCY** `28 ms` | `video.latency.total_ms` | Direct. |
| **RSSI** `−62 dBm` | `wfbcli.rx.ant_stats.rssi_avg` | Bucket per `ant_id`; at draw, **max across live antennas**. |
| **SNR** `18 dB` | `wfbcli.rx.ant_stats.snr_avg` | Bucket per `ant_id`; at draw, **max across live antennas**. |
| **REC** `00:14:32` | `dvr.recording` (bool) | Badge visible only while true. On false→true transition, stamp GS-local start; render elapsed `HH:MM:SS`. Dot blinks (1.1 s hard step). |

**Antenna aggregation rules (RSSI/SNR/freq):**
- Bucket incoming values by `ant_id` (from `fact.getTags()`), storing value + last-seen
  GS-local timestamp.
- **Staleness eviction:** entries not refreshed within ~2500 ms are dropped, so a
  disconnected antenna can't pin the max.
- **Link filtering:** if facts arrive under more than one `id`, prefer the `id`
  containing `"video"`; otherwise aggregate whatever arrives. No JSON needed.
- RSSI/SNR aggregate = **max** (best antenna actually carrying the link). freq = any
  live antenna.

`wfbcli` confirms the tag shape (src/wfbcli.cpp:87–109): ant_stats facts are published
with `tags = {id, ant_id}` (n_tags=2); `rx.packets.*.delta` with `tags = {id}` (n_tags=1).

**Threshold tables** (verbatim from the handoff; baked into a static table keyed by
metric, internal-only for now):

| Metric  | good (green)  | warn (amber)   | crit (red)  |
|---------|---------------|----------------|-------------|
| LINK %  | ≥ 70          | 40–69          | < 40        |
| BITRATE | ≥ 15 Mb/s     | 8–14.9 Mb/s    | < 8 Mb/s    |
| LATENCY | ≤ 50 ms       | 51–100 ms      | > 100 ms    |
| RSSI    | ≥ −70 dBm     | −71 … −80 dBm  | < −80 dBm   |
| SNR     | ≥ 12 dB       | 6–11 dB        | < 6 dB      |

VIDEO and WIFI CH are informational and never threshold-colored.

## Layout, fonts & scaling

**Geometry (reference 1920×1080, from the handoff).** Bottom strip: full width,
height 150, padding `0 46 26`, inner row baseline-aligned. Metric tile: padding `0 26`,
internal gap 2; accent rail 30×4 (radius 2); label below; value row below
(value + optional unit, baseline-aligned, gap 6). Signal bars: 5 bars, width 8, gap 5,
max height 42, radius 2, heights ramping 32%→100%; fill = `round(lq/100 × 5)`; empty
bars at 26% white. REC badge: top 38, right 48, gap 12; dot 14×14 (radius 3).
Left group `VIDEO`, `WIFI CH`; flex spacer; right group
`signal-bars LINK BITRATE LATENCY RSSI SNR`.

**Scaling & anchoring.** All geometry is expressed against a 1080-tall reference and
multiplied by `scale = surface_height / 1080.0`, then anchored from the live surface
(strip → bottom, badge → top-right). 720p is the clean case: same 16:9 aspect, so
uniform height-scaling reproduces the layout with no distortion (everything ×0.667).
Non-16:9 panels still scale by height (strip spans full width); 16:9 is the optimization
target. To keep thin elements crisp at fractional scale, **round to integer pixels with
floors**: rail height ≥ 2px, bar width ≥ 3px, radius ≥ 1px, shadow offset ≥ 1px. Text
sizes may remain fractional (rasterizer handles it).

**Typography (Barlow Condensed).** The current OSD uses Cairo's *toy* font API with the
system "Roboto" face (src/osd.cpp:2015). The handoff needs Barlow Condensed at multiple
weights including 800 italic at 46px, which the toy API can't reliably select. Plan:
- Bundle a Barlow Condensed TTF in the OSD assets dir; load via Cairo's **FreeType**
  backend (`cairo_ft_font_face_create_for_ft_face` from an `FT_Face`) and select per
  label with explicit size. Italic is faked via a Cairo shear matrix if a true italic
  face isn't bundled.
- **Fallback:** if the font file is missing, fall back to the toy API with a condensed
  sans face and log a warning (same graceful degradation as the icon loader). The widget
  still renders, just not pixel-perfect.
- This introduces a FreeType dependency for the OSD build. The plan will verify
  availability (FreeType is already in-tree via LVGL/font tooling); if a CMake addition
  is needed, the plan notes it.

**Text shadow (legibility over video).** Each label is drawn twice: a dark copy at +1px
offset behind the white/colored text, encapsulated in `draw_text_shadow()`. Token:
`0 1px 3px rgba(0,0,0,0.7)` approximated by the offset dark pass.

**Tokens.** good `#1fe084`, warn `#ffb300`, crit `#ff2e3e`, primary text `#ffffff`;
label text 62% white, unit text 66% white, neutral rail 50% white, empty bar 26% white;
gradient `rgba(10,11,14,0)` 0% → `0.10` 55% → `0.34` 100%.

## Color schemes

JSON `color_scheme: "white" | "accent"`, default `"accent"`. The scheme only changes the
color-resolution step (`resolve_color`); layout, geometry, and data are identical.

- **`accent`** — racing-HUD palette. Link-metric values, their accent rails, and filled
  signal bars recolor by threshold band (green/amber/red). Neutral tiles (VIDEO, WIFI CH)
  stay white with a 50%-white rail. REC dot red.
- **`white`** — fully monochrome: every value, label, rail, and filled signal bar is
  white (empty bars stay 26% white). No threshold recoloring. **Exception:** the REC dot
  stays red — it's a recording-status indicator, not a threshold metric, and red keeps it
  glanceable even in mono. (Overridable later if desired.)

## Configuration (`osd.json`)

A single entry in the `widgets` array. Minimal form (works out of the box):

```jsonc
{ "name": "All-in-one link OSD", "type": "AIOWidget" }              // accent scheme
{ "name": "All-in-one link OSD", "type": "AIOWidget", "color_scheme": "white" }
```

Recognized keys: `name` (required), `type` (required), `color_scheme` (optional,
`"accent"` default). Everything else — `x`, `y`, `scale`, `font`, and the per-slot
`facts` bindings — is an internal default and normally omitted. Advanced overrides
remain available (`x`/`y` origin, `scale` multiplier, `font` filename relative to
`assets_dir`, explicit `facts[]` to replace the default matcher set) but are not part of
the intended everyday config.

**Coexistence / migration.** AIOWidget supersedes the current strip widgets
(`Metrics background`, `CPU load`, `Video FPS and resolution`, `Video link throughput`,
`G2G latency` ×2, `DVR status`, `RSSI & SNR background`, `RSSI`, `SNR`). The plan ships
a before/after `osd.json` example: delete those entries, add the one AIOWidget block.
The history `BarChartWidget`s, `PopupWidget`, and `ExternalSurfaceWidget` are left
untouched and draw alongside. Existing `osd.json` keeps working until the user opts in.

## Testing

Catch2 host-sim build (`USE_SIMULATOR=ON`, the `build-test` workflow). The decomposition
makes the logic-heavy pieces testable without a display; pure helpers are exposed under
`#ifdef TEST`, mirroring the existing `TestTplTextWidget` pattern.

- `resolve_band` — exhaustive boundary tests against the handoff tables
  (LINK 70=good/69=warn/40=warn/39=crit; RSSI −70=good/−71=warn/−80=warn/−81=crit;
  and the BITRATE/LATENCY/SNR edges). The handoff **nominal** state asserts all-good;
  the **degraded** state asserts the green/amber/red mix.
- `resolve_color` — `white` returns white for every band; `accent` returns the palette;
  REC-dot-red holds in both schemes.
- LINK derivation — feed synthetic packet deltas → assert `lq` and the
  `round(lq/100 × 5)` bar count for 0,1,…,5.
- Antenna aggregation — feed multiple `ant_id` values → assert max selection; advance
  the fake clock past the staleness window → assert eviction; multiple `id`s → assert
  `"video"` preference.
- Channel conversion — 5745 MHz→149, 2412 MHz→1, out-of-band→raw passthrough.
- REC timecode — drive a fake monotonic clock across false→true→elapsed → assert
  `HH:MM:SS` formatting and badge visibility toggling.
- Draw smoke test — render to an offscreen Cairo image surface at 1080p and 720p;
  assert no crash and non-empty output (mirrors existing widget draw tests).

## Files touched

- `src/osd.cpp` — add `AIOWidget` class + helpers; add the `type == "AIOWidget"` factory
  branch with x/y/facts defaulting and default-matcher injection.
- `src/osd.hpp` — add a `#ifdef TEST` test shim exposing the pure helpers (band/color/
  link/channel/timecode/aggregation), following `TestTplTextWidget`.
- `tests/` — new Catch2 test file for the helpers.
- OSD assets — bundle `BarlowCondensed.ttf` (and wire `assets_dir` lookup).
- `CMakeLists.txt` — only if FreeType isn't already linked into the OSD target.
- `osd.json` — example/migrated config (documentation; not auto-applied).

## Notes / observed issues (out of scope)

- `FactMatcher::matches()` (src/osd.cpp:556) has a latent bug in the *tagged* path: it
  compares the `find` result against `tags.end()` instead of `fact_tags.end()`.
  AIOWidget uses only tagless matchers and never hits this branch, so it is left
  unchanged here.
- WIFI CH conversion is exact for standard 5 GHz channels and approximate for 2.4 GHz
  sub-bands; the raw-MHz fallback guarantees it never displays garbage.
