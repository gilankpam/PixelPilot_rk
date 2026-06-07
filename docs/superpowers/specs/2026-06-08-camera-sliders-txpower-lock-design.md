# Camera sliders (fractional GOP, bitrate slider) + TX-power lock — design

**Date:** 2026-06-08
**Branch:** `feat/refactor_ui`
**Status:** approved-for-planning

## Overview

Three GS-menu adjustments:

1. **TX Power editable during dynamic link** — currently locked when DLink is on.
2. **Camera GOP size** — fractional with a variable step: `0.0 → 1.0` in `0.1` steps, then `1.0` steps up to `30`.
3. **Camera bitrate** — change from a dropdown to a slider: `0.5 – 26.0 Mbps` in `0.5` steps, shown as `"12.5 Mbps"`.

Changes 2 and 3 both need `pp_slider` to handle non-plain-integer values, so they share one slider generalization.

## 1. TX Power lock

`prov_is_locked` (settings_fpvd.c) locks an AIR-endpoint field when its backend path is in the static `LOCKED_PATHS` list **and** `dynamicLink.enabled` is true. TX Power (`gs/wfbng/txpower` → `link.txpower`, AIR endpoint) is locked because `link.txpower` is in that list.

**Change:** remove `"link.txpower"` from `LOCKED_PATHS`. The air unit accepts TX Power changes while DLink is on (confirmed), so this won't produce error toasts. No other code references that path for locking. The other locked paths (`link.mcs`, `link.fec`, `link.width`, `video.bitrate`, `video.qpDelta`, `video.roi`) are unchanged.

## 2 + 3. `pp_slider` generalization

`pp_slider` keeps an **integer "raw" value internally** (as today) but gains a config describing how raw maps to display and how stepping works. Exposed via a new `pp_slider_ex(parent, icon, label, domain, page, key, cfg)`. The existing `pp_slider(parent, icon, label, domain, page, key, min, max)` becomes a thin wrapper that fills a default config — **every existing call site is unchanged** (link.c, dynamiclink.c, system.c, pixelpilot.c, camera.c GOP→ uses `_ex`).

### Config

```c
typedef struct {
    int32_t raw_min, raw_max;   /* internal integer range */
    int32_t step;               /* coarse step in raw units */
    int32_t fine_step;          /* step below fine_threshold (0 => uniform) */
    int32_t fine_threshold;     /* raw value at/above which `step` applies */
    int32_t disp_div;           /* display value = raw / disp_div (1 => raw) */
    int8_t  decimals;           /* display decimal places */
    const char *unit;           /* e.g. "Mbps"; NULL/"" for none */
    pp_slider_ser_t serialize;  /* PP_SER_INT (send raw) | PP_SER_FLOAT_DIV (send raw/disp_div) */
} pp_slider_cfg_t;
```

`pp_slider(min,max)` wrapper default: `raw_min=min, raw_max=max, step=step_for(min,max), fine_step=0, disp_div=1, decimals=0, unit=NULL, serialize=PP_SER_INT` — identical behavior to today.

### Variable step (symmetric)

- Incrementing: `s = (raw >= fine_threshold) ? step : fine_step`.
- Decrementing: `s = (raw >  fine_threshold) ? step : fine_step`.
- `fine_step == 0` means uniform (`s = step` always).

This yields `… 0.9 ↔ 1.0 ↔ 2.0 …` for GOP (no `1.x` values), matching the spec.

### Display + serialization

- Display string: `raw/disp_div` formatted with `decimals` decimals, plus a trailing unit element if `unit` is set (the existing value label + a small dim unit label, matching the design's stepper unit styling). Whole numbers may render without trailing decimals for readability (e.g. GOP `2` not `2.0`); below-1 GOP shows one decimal (`0.5`).
- Init read (`pp_settings_get`): parse the backend string into raw — `round(atof(v) * disp_div)` for `PP_SER_FLOAT_DIV`, `atoi(v)` for `PP_SER_INT` — then clamp to `[raw_min, raw_max]`.
- Apply: serialize raw per `serialize` — `PP_SER_INT` → `"%d"` of raw; `PP_SER_FLOAT_DIV` → `raw/disp_div` as a minimal float string (e.g. `"0.5"`, `"2"`).

Hold-to-accelerate (`pp_slider_accel`) multiplies the current per-press step; `pp_slider_set_relation` (bounds) is unaffected (GOP/bitrate don't use it).

### GOP size

`pages/camera.c`: replace `pp_slider(... "gopsize", 1, 20)` with `pp_slider_ex` using:
`raw_min=0, raw_max=300, step=10, fine_step=1, fine_threshold=10, disp_div=10, decimals=1, unit=NULL, serialize=PP_SER_FLOAT_DIV`.
Backend key `air/camera/gopsize` is already `FPVD_T_FLOAT` — no backend change.

### Bitrate

- `pages/camera.c`: replace the `pp_dropdown(... "bitrate", "5M\n10M\n…")` with `pp_slider_ex` using:
  `raw_min=500, raw_max=26000, step=500, fine_step=0, disp_div=1000, decimals=1, unit="Mbps", serialize=PP_SER_INT`.
- `settings_fpvd.c` keymap: change `air/camera/bitrate` type from `FPVD_T_BITRATE_KBPS` to `FPVD_T_INT` (raw kbps both directions). The lossy `"%dM"` get / integer-only `"M"` set are no longer needed — the slider owns kbps↔Mbps. `video.bitrate` is still the backend path (and stays in `LOCKED_PATHS`; bitrate remains DLink-driven/locked — only TX Power is unlocked).
- `settings_dummy.c`: change the bitrate seed value from the old `"…M"` form to raw kbps (e.g. `"12000"`) so the sim slider initializes correctly.

## Testing

- **Unit (Catch2, host sim build):** new `tests/test_slider_scale.cpp` (or extend an existing slider test) covering the pure logic — raw↔display formatting (GOP `5→"0.5"`, bitrate `12500→"12.5 Mbps"`), variable-step selection across the `fine_threshold` boundary in both directions, init-string→raw parse for both serialize modes, and clamping. Factor the step-selection + format into testable helpers (alongside `pp_slider_accel`/`pp_slider_bounds`).
- **Visual (simulator):** GOP shows `0.0…1.0` then integers and edits in 0.1/1.0 steps; bitrate shows `0.5–26.0 Mbps`; both apply via the dummy backend. TX Power: with the dummy DLink lock simulated on, TX Power is no longer marked locked.
- **On-device:** deploy and confirm GOP/bitrate apply to the air unit and TX Power is editable with DLink on.

## Out of scope

- No change to other locked paths or DLink behavior beyond TX Power.
- Bitrate stays DLink-locked when DLink is on (it is dynamically driven); only its *control type* changes to a slider.
- No change to the `FPVD_T_BITRATE_KBPS` transform itself (it's just no longer used by the camera bitrate key; left in place for any future use).
