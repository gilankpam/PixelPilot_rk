# Link FEC Mode row — design

## Summary

Add a **FEC Mode** selector to the Link settings page (`src/gsmenu/pages/link.c`).
Mode is pushed to the drone at `link.fec.mode` and accepts `"rs"` or `"swfec"`.
The mode gates which FEC parameter rows are visible:

- `mode == "rs"` → show **FEC_K** and **FEC_N**; hide Deadline and Overhead.
- `mode == "swfec"` → show **Deadline (ms)** and **Overhead (%)**; hide FEC_K and FEC_N.

The FEC Mode dropdown itself is always visible and acts as the anchor for the
two conditional row groups.

## Context

The drone config endpoint (`http://192.168.10.152:8080/config`) exposes:

```json
"link": {
  "fec": { "deadlineMs": 30, "k": 8, "mode": "swfec", "n": 12, "overheadPct": 50 }
}
```

The Link page already renders `FEC_K` (`link.fec.k`) and `FEC_N` (`link.fec.n`)
as sliders. This change adds the remaining three `link.fec.*` fields: `mode`,
`deadlineMs`, `overheadPct`.

`link.fec` is already a locked-path prefix (`LOCKED_PATHS` in
`settings_fpvd.c`), so all three new fields automatically participate in the
dynamic-link lock — they disable when `dynamicLink.enabled == true`, exactly
like `fec_k`/`fec_n` do today. No lock changes are required.

## Approach

Mirror the existing conditional-row pattern in `src/gsmenu/pages/dynamiclink.c`:
a per-page snapshot listener recomputes row visibility on every settings
mutation, toggling `LV_OBJ_FLAG_HIDDEN`. This was chosen over building a generic
"row depends on dropdown value" mechanism in the widget layer (YAGNI — only this
page needs it) and over splitting FEC into sub-pages (overkill).

The mode value uses `FPVD_T_ENUM`, which stores the dropdown's selected option
text verbatim. The dropdown options are therefore literally `rs` and `swfec`
(matching the existing `codec` h264/h265 and `rc_mode` cbr/vbr dropdowns). No
label→value mapping layer is introduced.

## Changes

### `src/gsmenu/settings_fpvd.c` — keymap

Add three entries adjacent to the existing `fec_k`/`fec_n` rows:

```c
{ "air", "wfbng", "fec_mode",         "link.fec.mode",        FPVD_T_ENUM, FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "wfbng", "fec_deadline_ms",  "link.fec.deadlineMs",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "wfbng", "fec_overhead_pct", "link.fec.overheadPct", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

`LOCKED_PATHS` is unchanged — the existing `"link.fec"` prefix already covers
all three subpaths.

### `src/gsmenu/pages/link.c` — page layout + conditional visibility

- Add a **FEC Mode** dropdown immediately before the existing `FEC_K` slider:
  `pp_dropdown(page, ..., "FEC Mode", "air", "wfbng", "fec_mode", "rs\nswfec")`.
- Keep the existing `FEC_K` (range 1..31) and `FEC_N` (range 2..32) sliders and
  their k ≤ n−2 relation untouched.
- Add two sliders after `FEC_N`:
  - **Deadline (ms)** → `air/wfbng/fec_deadline_ms`, range `10..50`.
  - **Overhead (%)** → `air/wfbng/fec_overhead_pct`, range `0..100`.
- Tag the conditional rows so the listener can find them:
  - rs-group (`FEC_K`, `FEC_N`) → `LV_OBJ_FLAG_USER_1`.
  - swfec-group (`Deadline`, `Overhead`) → `LV_OBJ_FLAG_USER_2`.
- Add `apply_fec_visibility(page)`: read `air/wfbng/fec_mode`; show the matching
  group and hide the other via `LV_OBJ_FLAG_HIDDEN`. An unknown/empty mode hides
  both groups (consistent with `dynamiclink.c` pre-snapshot behavior).
- Replace the current snapshot-listener registration (which only calls
  `pp_page_reapply_lock_state`) with a combined callback that runs
  `apply_fec_visibility()` then `pp_page_reapply_lock_state()`, matching
  `dynamiclink.c`'s `snapshot_listener_cb`. Call both once at build time after
  the group is populated.

### `src/gsmenu/settings_dummy.c` — sim provider parity

- Add default values: `{ "fec_mode", "swfec" }`, `{ "fec_deadline_ms", "30" }`,
  `{ "fec_overhead_pct", "50" }`.
- Add `"fec_mode"`, `"fec_deadline_ms"`, `"fec_overhead_pct"` to
  `g_dummy_locked_keys[]` so the simulator mirrors the `link.fec` lock.

## Data flow

1. User opens the FEC Mode dropdown and selects `rs` or `swfec`.
2. The dropdown widget calls `pp_settings_set("air","wfbng","fec_mode", value)`.
3. The provider stores the value (verbatim, ENUM) and fires the snapshot fanout.
4. The page's combined listener runs `apply_fec_visibility` → the relevant row
   group flips visibility immediately, and `pp_page_reapply_lock_state` refreshes
   lock state.
5. On apply/push, `fec_mode` serializes via `value_to_cjson` (ENUM →
   `cJSON_CreateString`) to `link.fec.mode`; the int rows serialize to
   `link.fec.deadlineMs` / `link.fec.overheadPct`.

## Testing

- `tests/test_settings_fpvd.cpp`:
  - keymap lookups for `air/wfbng/fec_mode` (path `link.fec.mode`, type
    `FPVD_T_ENUM`), `fec_deadline_ms` (`link.fec.deadlineMs`, `FPVD_T_INT`),
    `fec_overhead_pct` (`link.fec.overheadPct`, `FPVD_T_INT`).
  - `fpvd_is_locked_path("link.fec.mode")`,
    `fpvd_is_locked_path("link.fec.deadlineMs")`,
    `fpvd_is_locked_path("link.fec.overheadPct")` all return `true`.
- Host simulator (`sim.sh`) and on-GS manual check: toggling the mode flips the
  visible row group; values round-trip to the drone config.

## Out of scope

- Friendly dropdown labels (would require a new label→value mapping layer).
- Validation/relations among deadlineMs / overheadPct.
- Any changes to drone-side FEC behavior.
