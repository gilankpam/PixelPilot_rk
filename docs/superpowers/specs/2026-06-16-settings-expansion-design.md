# gsmenu settings expansion — design

Three independent settings changes, delivered as one combined spec/plan:

- **A. Camera — Resilience dropdown** (`video.resilience`), greys out GOP size when active.
- **B. Camera — OSD Enabled toggle** (`osd.enabled`).
- **C. Dynamic Link page refactor** — strip to Enabled + Failsafe, add a Compute section
  (drone `dynamicLink.compute.*` + GS `dynamicLink.maxMcs`).

## Shared context

The gsmenu settings layer maps a `(domain, page, key)` UI triple to a drone/GS JSON
path plus a serialization type, in the `KEYMAP` table in
`src/gsmenu/settings_fpvd.c`. `fpvd_keymap_lookup(d, p, k)` resolves the triple;
`value_to_cjson` / `fpvd_snapshot_read_string` convert by `fpvd_type_t`.

Relevant types: `FPVD_T_BOOL` (`on`/`off` ↔ JSON bool), `FPVD_T_INT`,
`FPVD_T_FLOAT`, `FPVD_T_ENUM` (dropdown option text stored verbatim ↔ JSON string).
Endpoints: `FPVD_EP_AIR` (drone `/air/*`), `FPVD_EP_GS` (`/gs/*`). Row classes:
`FPVD_ROW_PLAIN` (PATCH + apply on the row's endpoint), `FPVD_ROW_DLINK`
(adaptive-link arm/disarm), etc.

`LOCKED_PATHS` (settings_fpvd.c) lists JSON-path prefixes that are read-only while
the drone's `dynamicLink` is armed; `fpvd_is_locked_path` does prefix/subtree
matching. None of the paths added in this spec belong there.

The simulator's stand-in provider `src/gsmenu/settings_dummy.c` holds default values
(keyed by bare UI key) and `g_dummy_locked_keys[]` (UI-key mirror of `LOCKED_PATHS`).

Host tests build under `USE_SIMULATOR=ON` in `build-test/`; all cmake/test commands
run inside `nix-shell --run '...'` (lvgl needs SDL2/libpng from `shell.nix`). The
`pixelpilot` GUI target cannot run headless — a clean compile is the gate for
UI/page changes. `fpvd_tests` covers the keymap.

---

## Feature A — Camera: Resilience dropdown

### Behavior

`video.resilience` selects a waybeam error-resilience preset. Allowed values
(drone-validated, exact strings): `off`, `rescue`, `quality`, `sprint`, `racing`,
`endurance`, `patrol`, `rally`, `range`, `fpv`. Default `off`. When `resilience !=
"off"`, the drone ignores `gopSize`, so the GOP size row is greyed out.

`video.resilience` is operator-owned and stays editable while `dynamicLink.enabled`
— it is **not** added to `LOCKED_PATHS`.

### Changes

**`settings_fpvd.c` — keymap (Camera block, near `gopsize`):**
```c
{ "air", "camera", "resilience", "video.resilience", FPVD_T_ENUM, FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

**`camera.c`:**
- Add the dropdown immediately after the GOP size slider:
  ```c
  pp_dropdown(page, LV_SYMBOL_SETTINGS, "Resilience",
              "air", "camera", "resilience",
              "off\nrescue\nquality\nsprint\nracing\nendurance\npatrol\nrally\nrange\nfpv");
  ```
- Capture the GOP size slider's `lv_obj_t *` (currently the return value of
  `pp_slider_ex(... "gopsize" ...)` is unused) and tag it with
  `LV_OBJ_FLAG_USER_1` so the gate can find it.
- Add `apply_resilience_gate(lv_obj_t *page)`: read
  `pp_settings_get("air","camera","resilience")`, free it. Iterate the tagged GOP
  row (`LV_OBJ_FLAG_USER_1`). If the value is non-NULL and not `"off"`, disable it
  via `lv_obj_add_state(row, LV_STATE_DISABLED)` + `lv_obj_set_style_opa(row,
  LV_OPA_60, 0)` (no lock icon — distinguishes "inactive due to preset" from a
  Dynamic-Link lock). Otherwise **restore** it
  (`lv_obj_remove_state(row, LV_STATE_DISABLED)` + `LV_OPA_COVER`) **only when its
  lock state is `PP_ROW_UNLOCKED`** — the gate must be reversible, and the
  preceding lock pass does NOT restore it for us (`pp_row_set_locked` early-returns
  on an unchanged `UNLOCKED` state and never clears the gate's raw
  `LV_STATE_DISABLED`/opacity). Guarding on `pp_row_get_locked(row) ==
  PP_ROW_UNLOCKED` avoids clobbering a row the lock pass legitimately disabled
  (drone offline/unavailable/dynamic). Finish with `pp_page_rescue_focus(page)`.
- Replace the current snapshot-listener registration (which only calls
  `pp_page_reapply_lock_state`) with a combined `snapshot_listener_cb(void *ud)`
  that runs `pp_page_reapply_lock_state(page)` **then** `apply_resilience_gate(page)`
  — gate last so the lock pass cannot overwrite it. Call both once at build time.

**`settings_dummy.c`:** add default `{ "resilience", "off" }`. Not locked.

### Edge cases
- Live update: changing the dropdown fires the fanout listener, so GOP greys/ungreys
  immediately.
- `resilience == "off"`: GOP follows its normal lock state untouched.
- GOP both offline-locked *and* resilience-gated: the lock pass shows its lock icon
  and the gate's extra DISABLED/opacity are harmless no-ops on top.
- Unknown/empty resilience (pre-snapshot): treated as off-equivalent (gate only
  disables on a concrete non-`"off"` value), so GOP stays enabled until a real
  snapshot arrives — consistent with `off` being the default.

---

## Feature B — Camera: OSD Enabled toggle

### Behavior
Drone top-level `osd.enabled` (boolean, default `false`). Plain operator toggle, not
locked.

### Changes

**`settings_fpvd.c` — keymap:**
```c
{ "air", "camera", "osd_enabled", "osd.enabled", FPVD_T_BOOL, FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

**`camera.c`:** add a new section after the Video section (and before ROI):
```c
pp_section_header(page, "OSD");
pp_toggle(page, LV_SYMBOL_EYE_OPEN, "OSD Enabled", "air", "camera", "osd_enabled");
```

**`settings_dummy.c`:** add default `{ "osd_enabled", "off" }`. Not locked.

### Naming note
The Dynamic Link refactor (Feature C) removes its old `air/dlink/osd_enabled` entry
and the dummy default keyed `osd_enabled`, so after this spec the bare key
`osd_enabled` is unique in the dummy table — no collision. (Keymap lookups are by
full triple and are unaffected regardless.)

---

## Feature C — Dynamic Link page refactor

### Background: two distinct `dynamicLink` blocks
- **Drone-side** (`/air`, the *applier*): `enabled`, `compute.*`, `safe.*`, plus the
  OSD/Timing/ROI-QP knobs the current page exposes.
- **GS-side** (`/gs`, the *controller* `fpvdgs`): `enabled`, `maxMcs` (0..7), etc.,
  applied live via `PATCH /gs/config` + `POST /gs/apply` with no wfb restart.

Only the existing **drone-side** `enabled` toggle is kept. The GS-side controller
`enabled` is out of scope.

### Target page (`dynamiclink.c`)
```
General:   Enabled (toggle)              KEPT  air/dlink/enabled  (FPVD_ROW_DLINK)
Compute:   Base Redundancy Ratio  float 0.1–2.0 step 0.1  def 0.5   -> dynamicLink.compute.baseRedundancyRatio [air FLOAT]
           Blocks / Frame         float 0.5–8.0 step 0.5  def 2.0   -> dynamicLink.compute.blocksPerFrame       [air FLOAT]
           Min Bitrate (Mbps)     Mbps 0.5–26.0 step 0.5  def 1.0   -> dynamicLink.compute.minBitrateKbps       [air INT]
           Max Bitrate (Mbps)     Mbps 0.5–26.0 step 0.5  def 24.0  -> dynamicLink.compute.maxBitrateKbps       [air INT]
           Max MCS                int 0–7                  def 5     -> dynamicLink.maxMcs                        [GS  INT]
Failsafe:  MCS, FEC K, FEC N, Block Depth, Bandwidth, TX Power, Bitrate   KEPT verbatim
```

Removed sections/rows: General's *Interleaving* and *MAVLink Enable*; the entire
*OSD* section; the entire *Timing* section; the entire *ROI QP* section.

### Slider details
- Base Redundancy Ratio & Blocks/Frame are floats — use `pp_slider_ex` with a
  `pp_slider_cfg_t` using `PP_SER_FLOAT_DIV`:
  - Base Redundancy Ratio: `raw_min=1, raw_max=20, step=1, disp_div=10, decimals=1`
    (displays 0.1–2.0, serializes 0.1–2.0).
  - Blocks/Frame: `raw_min=1, raw_max=16, step=1, disp_div=2, decimals=1`
    (displays 0.5–8.0 in 0.5 steps, serializes the float).
- Min/Max Bitrate mirror the **camera Bitrate** slider UI exactly — `pp_slider_ex`
  with a `pp_slider_cfg_t` matching `bitrate_cfg` in `camera.c`:
  `{ .raw_min=500, .raw_max=26000, .step=500, .fine_step=0, .fine_threshold=0,
     .disp_div=1000, .decimals=1, .unit="Mbps", .serialize=PP_SER_INT }`
  (displays Mbps, stores raw kbps as INT). A single shared `static const`
  config can serve both rows.
- Enforce min ≤ max−500 (raw kbps units) with `pp_slider_set_relation`, mirroring
  the FEC K/N relation:
  `pp_slider_set_relation(min_row, "air","dlink","compute_max_bitrate_kbps", -500, /*is_max*/ true)`
  and
  `pp_slider_set_relation(max_row, "air","dlink","compute_min_bitrate_kbps",  500, /*is_max*/ false)`.
- Max MCS is a plain `pp_slider` int, 0–7.

### Visibility behavior (kept)
The page's existing `apply_visibility` hides every row past the Enabled-toggle
anchor (`LV_OBJ_FLAG_USER_3`) when `dynamicLink.enabled` is off. The new Compute
rows are added after the anchor, so they hide/show with the rest. The
`snapshot_listener_cb` (visibility + `reapply_lock_state`) is unchanged in shape.

### Keymap (`settings_fpvd.c`)
**Remove** the 12 dead `air/dlink` entries: `interleaving`, `mavlink_enable`,
`osd_enabled`, `osd_debug_latency`, `health_timeout_ms`, `min_idr_interval_ms`,
`apply_stagger_ms`, `apply_subpace_ms`, `roiqp_threshold_kbps`,
`roiqp_low_anchor_kbps`, `roiqp_floor`, `roiqp_step`.

**Add** 5 entries:
```c
{ "air", "dlink", "compute_base_redundancy",  "dynamicLink.compute.baseRedundancyRatio", FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "dlink", "compute_blocks_per_frame", "dynamicLink.compute.blocksPerFrame",      FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "dlink", "compute_min_bitrate_kbps", "dynamicLink.compute.minBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "dlink", "compute_max_bitrate_kbps", "dynamicLink.compute.maxBitrateKbps",      FPVD_T_INT,   FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "gs",  "dlink", "max_mcs",                  "dynamicLink.maxMcs",                      FPVD_T_INT,   FPVD_EP_GS,  FPVD_ROW_PLAIN },
```
`dynamicLink.compute` and `dynamicLink.maxMcs` are **not** added to `LOCKED_PATHS`.

### Sim provider (`settings_dummy.c`)
- Remove the 12 dead Dynamic Link defaults (`interleaving`, `mavlink_enable`,
  `osd_enabled`, `osd_debug_latency`, `health_timeout_ms`, `min_idr_interval_ms`,
  `apply_stagger_ms`, `apply_subpace_ms`, `roiqp_threshold_kbps`,
  `roiqp_low_anchor_kbps`, `roiqp_floor`, `roiqp_step`).
- Add 5 defaults: `{ "compute_base_redundancy", "0.5" }`,
  `{ "compute_blocks_per_frame", "2.0" }`, `{ "compute_min_bitrate_kbps", "1000" }`,
  `{ "compute_max_bitrate_kbps", "24000" }`, `{ "max_mcs", "5" }`.
- None added to `g_dummy_locked_keys[]`.

---

## Testing (all features)

`tests/test_settings_fpvd.cpp`:
- New keymap lookups (path + type) for: `air/camera/resilience`
  (`video.resilience`, ENUM); `air/camera/osd_enabled` (`osd.enabled`, BOOL);
  `air/dlink/compute_base_redundancy` (`dynamicLink.compute.baseRedundancyRatio`,
  FLOAT); `air/dlink/compute_blocks_per_frame`
  (`dynamicLink.compute.blocksPerFrame`, FLOAT); `air/dlink/compute_min_bitrate_kbps`
  (`dynamicLink.compute.minBitrateKbps`, INT); `air/dlink/compute_max_bitrate_kbps`
  (`dynamicLink.compute.maxBitrateKbps`, INT); `gs/dlink/max_mcs`
  (`dynamicLink.maxMcs`, INT).
- Assert removed keys now return `nullptr`: `air/dlink/interleaving`,
  `air/dlink/roiqp_floor`, `air/dlink/health_timeout_ms` (representative sample).
- Assert not locked: `fpvd_is_locked_path("video.resilience") == false`,
  `("osd.enabled") == false`, `("dynamicLink.compute") == false`,
  `("dynamicLink.maxMcs") == false`.

Build/run:
- `nix-shell --run 'cmake --build build-test --target fpvd_tests -j && ./build-test/fpvd_tests'`
- `nix-shell --run 'cmake --build build-test --target pixelpilot -j'` (clean compile gate)
- Manual sim (`./sim.sh`): Resilience greys/ungreys GOP; OSD toggle present; Dynamic
  Link page shows only Enabled + Compute + Failsafe and hides Compute/Failsafe when
  disabled.

## Out of scope
- GS-side `dynamicLink.enabled` (controller arm) and the selector/smoothing knobs.
- Friendly dropdown labels for resilience (verbatim ENUM only).
- Any drone/GS-side behavior changes.
