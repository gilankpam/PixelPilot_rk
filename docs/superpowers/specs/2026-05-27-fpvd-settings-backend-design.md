# fpvd Settings Backend — Drone-Side Provider

**Status:** Design
**Date:** 2026-05-27

## Summary

Add a real settings backend that talks to the on-drone `fpvd` HTTP API over the wfb-ng tunnel (default `http://10.5.0.10:8080`). Plugs into the existing `pp_settings_provider_t` seam introduced in commit `8a16713`, alongside the in-memory dummy and the no-op stub. Maps to the Camera and Link tabs (with field cleanups and additions), and introduces a new **Dynamic Link** tab that controls the on-drone `dl-applier` process and grey-locks the link/video rows it owns.

Ground-station settings are out of scope. The fields conceptually shared between drone and GS (Wi-Fi channel, bandwidth) are written only to the drone backend in this work.

## Goals

- A real, production-quality settings provider talking to fpvd's stage-then-commit API (`PATCH /config` → `POST /apply`).
- Field-by-field reconciliation of the Camera and Link tabs against the fpvd schema: add what's missing, drop what fpvd doesn't model.
- New Dynamic Link tab covering the full `dynamicLink` schema in a flat layout consistent with existing tabs.
- Synchronous-feeling write UX: the row enters a busy state during the in-flight request and only transitions on the response. No optimistic UI.
- Offline resilience: if the drone is unreachable, every drone-backed row greys out; a background reconnect loop restores them when fpvd comes back.
- Dummy provider gains realistic latency simulation so the busy UX is exercisable in sim without a drone.

## Non-goals

- **GS-side persistence.** Writing wfb-ng GS config files, gs scripts, or any non-fpvd target. The provider routes only to fpvd; GS writers are future work.
- **Authentication.** fpvd has none — the wfb tunnel is the trust boundary.
- **Existing System / Display / DVR tabs.** Untouched in this work.
- **fpvd validation logic.** fpvd's own tests cover schema validation; we test the client's error-handling paths against canned responses.
- **Migration of `gsmenu.sh`.** Already removed in commit `92206b2`.

## Decisions captured in brainstorming

| Decision | Choice |
|---|---|
| Apply model | Auto-apply per row (PATCH + apply per change), worker-thread + debounced |
| Lock UX | Grey out + lock icon; focus passes over locked rows; tap shows toast |
| Dynamic Link tab layout | Flat sections (General / OSD / Timing / ROI QP / Safe Ceilings) matching existing tabs |
| Dynamic Link visibility | When `enabled=false`, only the Enabled toggle is shown; toggling reveals the rest |
| Write feedback | Synchronous-feeling: row enters busy state during HTTP; value only changes on success |
| Offline behavior | Grey out all drone-backed rows; background reconnect loop; rows re-enable on reconnect |
| Snapshot section | Not surfaced — `snapshot.*` stays at firmware defaults |
| Dummy provider | Gains `PP_SIM_LATENCY_MS` (default 200) to simulate wfb-tunnel round-trip |
| Optimistic rollback (from `8a16713`) | **Removed** — superseded by busy-state UI |

## Architecture

### New files

- `src/gsmenu/settings_fpvd.c` — provider implementation.
- `src/gsmenu/settings_fpvd_map.c` (or static table inside `settings_fpvd.c`) — the key→json-path mapping table.
- `src/gsmenu/pages/dynamiclink.c` + `dynamiclink.h` — new tab page builder.
- `tests/settings_fpvd_test.cpp` — Catch2 unit tests.
- `tests/settings_fpvd_integration_test.cpp` — Catch2 integration tests behind `[network]` tag.

### Modified files

- `src/gsmenu/settings.h` — add `pp_settings_register_fpvd()`, `pp_settings_is_locked(...)`, `pp_settings_set_snapshot_listener(...)`, `pp_settings_set_visibility(bool)`, `pp_settings_is_connected(void)`.
- `src/gsmenu/settings_dummy.c` — extend seed to cover new keys; add `PP_SIM_LATENCY_MS` deferred callbacks via `lv_timer_create`.
- `src/gsmenu/pages/camera.c` — drop ISP/FPV/Contrast/Hue/Saturation/Luminance/Max-usage, add Rotate/QP Delta/ROI section, fix Recording rows.
- `src/gsmenu/pages/link.c` — fix Bandwidth/MCS/TX-power/FEC ranges, drop "80" bandwidth option.
- `src/gsmenu/ui.h` and the tab-strip builder (wherever the existing tabs are registered) — add Dynamic Link tab between Link and System (exact insertion point follows the existing tab registration; pinned during implementation).
- `src/gsmenu/widgets/pp_row.h` + `pp_row.c` — add `pp_row_set_busy(row, bool)` and a small spinner widget on the row's trailing edge.
- `src/gsmenu/widgets/pp_toggle.c`, `pp_slider.c`, `pp_dropdown.c` — call `pp_row_set_busy(row, true)` before `pp_settings_set_async`; clear on callback; on success re-read the snapshot for the new display value; on failure leave value unchanged + show toast.
- `CMakeLists.txt` — link `libcurl` for the real build; ensure `cJSON` (or chosen JSON lib) is available; add new sources.
- Build glue / startup — replace `pp_settings_register_stub()` / `pp_settings_register_dummy()` with `pp_settings_register_fpvd()` in the on-device path; sim keeps dummy.

### Provider internals (`settings_fpvd.c`)

**State**, protected by a single mutex:
- `cJSON *snapshot` — last known effective config from `GET /config`.
- `bool connected` — true if last HTTP call succeeded.
- `char fpvd_url[256]` — base URL; defaults to `http://10.5.0.10:8080`, overridable with `PP_FPVD_URL` env var.
- `bool menu_visible` — set by `pp_settings_set_visibility`; controls poll cadence.
- Listener function pointer + user data; called on UI thread via `lv_async_call` after every snapshot mutation.

**Worker thread.** Single pthread, owns a small ring queue of jobs (each: `json_path`, `value_string`, `on_done`, `user_data`). Producer (`set_async`) takes the mutex, coalesces with any pending job for the same `json_path` (replacing it), signals condvar. Worker loops:
1. Wait for either a queued job or the poll tick (cond_timedwait).
2. If a job is ready: PATCH+apply (see below), then sleep ~250 ms before the next dequeue so rapid commits coalesce.
3. Else (poll tick): `GET /config`, update snapshot, fire listener on UI thread if anything changed.
4. If `connected=false`: tighter poll cadence (~2 s) for reconnect attempts; ~3 s otherwise.

**Per-job HTTP** (libcurl easy interface, ~1.5 s connect timeout, ~5 s total):
1. `PATCH /config` with a sparse body built from the json_path + value.
   - On non-2xx: parse `error` code; format a user-readable message:
     - `validation` → first `details[i].message`
     - `dynamic_link_locked` → `"Locked by Dynamic Link"`; trigger a snapshot refresh.
     - `bad_json` → `"Internal error"` (shouldn't happen, builder bug)
     - Other → status code text.
   - On transport error: `connected=false`; queue listener; message `"Drone unreachable"`.
2. `POST /apply`.
   - On non-2xx: same mapping; if `validation` with empty `details`, follow up with `GET /status` and surface `lastApply.error`.
3. On success: parse `applied/version/restarted`; refresh snapshot via `GET /config` (simpler than merging locally); call listener; call `on_done(0, NULL, ud)` via `lv_async_call`.
4. On any failure path: call `on_done(-1, msg, ud)` via `lv_async_call`. Snapshot is **not** mutated, so widgets re-read it and remain at the previous value.

**JSON parsing.** Use `cJSON` (single-file C dep, MIT-licensed; vendor in `extern/` if not already present). Sparse PATCH bodies built by walking the json_path components and constructing nested objects.

### Key mapping table

A static array of:

```c
typedef enum { T_INT, T_FLOAT, T_BOOL, T_STRING, T_ENUM, T_BITRATE_KBPS } fpvd_type_t;

typedef struct {
    const char *domain;
    const char *page;
    const char *key;
    const char *path;       // dotted json path
    fpvd_type_t type;
} fpvd_keymap_entry_t;
```

`T_BITRATE_KBPS` formats `15000` ↔ `"15M"` (the dropdown still uses M-suffixed strings); other conversions are direct. Full table is enumerated in the implementation plan; sample entries:

| domain | page | key | path | type |
|---|---|---|---|---|
| air | camera | size | video.resolution | STRING |
| air | camera | fps | video.fps | INT |
| air | camera | bitrate | video.bitrate | BITRATE_KBPS |
| air | camera | codec | video.codec | ENUM |
| air | camera | gopsize | video.gopSize | FLOAT |
| air | camera | rc_mode | video.rcMode | ENUM |
| air | camera | qp_delta | video.qpDelta | INT |
| air | camera | roi_enabled | video.roi.enabled | BOOL |
| air | camera | roi_qp | video.roi.qp | INT |
| air | camera | roi_center | video.roi.center | FLOAT |
| air | camera | roi_steps | video.roi.steps | INT |
| air | camera | mirror | image.mirror | BOOL |
| air | camera | flip | image.flip | BOOL |
| air | camera | rotate | image.rotate | INT |
| air | camera | rec_enable | recording.enabled | BOOL |
| air | camera | rec_split | recording.maxSeconds | INT (min×60) |
| air | camera | rec_maxmb | recording.maxMB | INT |
| gs | wfbng | gs_channel | link.channel | INT |
| gs | wfbng | bandwidth | link.width | INT |
| gs | wfbng | txpower | link.txpower | INT |
| air | wfbng | mcs_index | link.mcs | INT |
| air | wfbng | stbc | link.stbc | BOOL |
| air | wfbng | ldpc | link.ldpc | BOOL |
| air | wfbng | fec_k | link.fec.k | INT |
| air | wfbng | fec_n | link.fec.n | INT |
| air | dlink | enabled | dynamicLink.enabled | BOOL |
| air | dlink | interleaving | dynamicLink.interleavingSupported | BOOL |
| air | dlink | mavlink_enable | dynamicLink.mavlinkEnable | BOOL |
| air | dlink | osd_enabled | dynamicLink.osd.enabled | BOOL |
| air | dlink | osd_debug_latency | dynamicLink.osd.debugLatency | BOOL |
| air | dlink | health_timeout_ms | dynamicLink.healthTimeoutMs | INT |
| air | dlink | min_idr_interval_ms | dynamicLink.minIdrIntervalMs | INT |
| air | dlink | apply_stagger_ms | dynamicLink.applyStaggerMs | INT |
| air | dlink | apply_subpace_ms | dynamicLink.applySubPaceMs | INT |
| air | dlink | roiqp_threshold_kbps | dynamicLink.roiQp.thresholdKbps | INT |
| air | dlink | roiqp_low_anchor_kbps | dynamicLink.roiQp.lowAnchorKbps | INT |
| air | dlink | roiqp_floor | dynamicLink.roiQp.floor | INT |
| air | dlink | roiqp_step | dynamicLink.roiQp.step | INT |
| air | dlink | safe_mcs | dynamicLink.safe.mcs | INT |
| air | dlink | safe_k | dynamicLink.safe.k | INT |
| air | dlink | safe_n | dynamicLink.safe.n | INT |
| air | dlink | safe_depth | dynamicLink.safe.depth | INT |
| air | dlink | safe_bandwidth | dynamicLink.safe.bandwidth | INT |
| air | dlink | safe_txpower_dbm | dynamicLink.safe.txPowerDbm | INT |
| air | dlink | safe_bitrate_kbps | dynamicLink.safe.bitrateKbps | INT |

### Locked paths

When `dynamicLink.enabled==true`, the following are read-only and `pp_settings_is_locked` returns true (prefix match against the locked path list):

```
link.mcs
link.txpower
link.fec        // covers link.fec.k and link.fec.n
link.width
video.bitrate
video.qpDelta
video.roi       // covers all video.roi.*
```

### Snapshot listener pattern

```c
typedef void (*pp_settings_snapshot_cb)(void *user_data);
void pp_settings_set_snapshot_listener(pp_settings_snapshot_cb cb, void *ud);
```

- Always invoked on the LVGL thread via `lv_async_call`.
- Page builders register one listener per tab. The listener walks its rows and calls `pp_row_set_locked(row, pp_settings_is_locked(...))` and (for the Dynamic Link tab) toggles row visibility based on `dynamicLink.enabled`.
- Listener is also called on `connected` transitions; rows query `pp_settings_is_connected()` and grey out entirely when false (existing locked styling, alternate icon).

## UI changes

### Camera tab

Final section list and rows:

- **Video** — Size, FPS, Bitrate, Codec, GOP size, RC Mode, **QP Delta** (new)
- **ROI** (new) — Enabled, QP, Center, Steps
- **Image** — Mirror, Flip, **Rotate** (new). (**Removed:** Contrast, Hue, Saturation, Luminance.)
- **ISP** — section removed entirely (no fpvd mapping).
- **FPV** — section removed entirely (no fpvd mapping).
- **Recording** — Enabled, Split (min), **Max size (MB)** (new). (**Removed:** Max usage %.)

### Link tab

Final WFB-NG section:

| Row | Range / options |
|---|---|
| Channel | dropdown — keep existing list (all within fpvd 1..200) |
| Bandwidth | **20, 40** (drop 80) |
| TX Power | slider 1..63 (driver units; relabel removes the "%") |
| MCS Index | slider 0..7 |
| STBC | toggle |
| LDPC | toggle |
| FEC_K | slider 1..31 |
| FEC_N | slider 2..32 |

### Dynamic Link tab (new)

Flat sections, all rows hidden when `dynamicLink.enabled=false` except the Enabled toggle.

- **General** — Enabled, Interleaving Supported, MAVLink Enable
- **OSD** — OSD Enabled, Debug Latency
- **Timing** — Health Timeout (ms) 1000..30000, Min IDR Interval (ms) 16..2000, Apply Stagger (ms) 0..500, Apply Sub-pace (ms) 0..50
- **ROI QP** — Threshold (kbps), Low Anchor (kbps), Floor (-48..0), Step (1..10)
- **Safe Ceilings** — Max MCS 0..7, Max FEC K 1..31, Max FEC N 2..32, Block Depth 1..8, Max Bandwidth (20/40), Max TX Power (dBm) -10..30, Max Bitrate (kbps)

## Write flow (UI ↔ provider)

1. User commits a change on a row (slider release, toggle tap, dropdown select).
2. Widget calls `pp_row_set_busy(row, true)` — spinner appears, inner control disabled, displayed value unchanged.
3. Widget calls `pp_settings_set_async(domain, page, key, new_value, on_done, row_ctx)`.
4. Provider enqueues; worker PATCHes + applies + refreshes snapshot.
5. Worker's `on_done` runs on the UI thread:
   - `rc=0` → `pp_row_set_busy(row, false)`; widget re-renders from snapshot (now reflects the new value).
   - `rc<0` → `pp_row_set_busy(row, false)`; toast surfaces `err`; widget re-renders from snapshot (unchanged from prior effective).

## Offline behavior

- On any transport failure during a write or a poll, `connected` flips to false.
- Listener fires; every drone-backed row greys out (lock-style affordance with an "offline" hint, e.g., a small "wifi-off" icon — exact glyph picked during implementation).
- Background loop keeps polling `GET /config` every ~2 s.
- On reconnect: snapshot populated, `connected=true`, listener fires, rows re-enable.
- Toast on transitions: `"Drone offline — retrying"` and `"Drone reconnected"`. No per-attempt noise.

## Dummy provider extensions

- New env var `PP_SIM_LATENCY_MS` (default **200**). Set to 0 to restore zero-latency; set high to stress-test the spinner.
- Implementation: `dummy_set_async` allocates a context, calls `lv_timer_create(deferred_done, latency_ms, ctx)`. The timer's handler invokes `on_done` (success or `PP_SIM_FAIL` failure) and deletes itself.
- Seed table updated to cover the new keys; orphaned keys removed; ranges aligned with fpvd (Bandwidth no 80, MCS 0..7, etc.).

## Testing

Catch2, host-side, following the pattern from commit `8a16713`.

**Unit tests (`tests/settings_fpvd_test.cpp`)**
- Key map coverage: every UI key resolves; every `LOCKED_PATHS` entry is reachable.
- Snapshot read: ~10 representative paths round-trip from a fixture `defaults.json`.
- Sparse PATCH builder: ~6 paths including nested (`link.fec.k`), float (`video.roi.center`), string, bool, enum.
- Value parsing: bitrate `"15M"` ↔ 15000, bool `"on"`/`"off"` ↔ `true`/`false`, rotate dropdown ↔ int, split (min) ↔ seconds.
- Lock evaluation: prefix-match logic (`video.roi.center` locked because `video.roi` is in the list).

**Integration tests (`tests/settings_fpvd_integration_test.cpp`, `[network]` tag)** — against an in-process embedded HTTP server (cpp-httplib if available, fallback to a small hand-rolled accept loop):
- Happy path: PATCH body shape, apply call, callback `rc=0`.
- Validation error → callback message contains the server's detail.
- Lock error → callback message `"Locked by Dynamic Link"` and a follow-up `GET /config` is observed.
- Transport failure → `connected=false`, callback `"Drone unreachable"`.
- Reconnect: server offline at start → comes up → listener fires.
- Debounce: 5 rapid writes to the same path → 1 PATCH at the server.

**Sim parity**
- Update `settings_dummy.c` seed; remove orphaned entries.
- `PP_SIM_LATENCY_MS=400 ./sim.sh` exercises the spinner UX; `PP_SIM_FAIL=1` exercises the error toast.

**Manual on-device smoke test** (documented, not automated)
- One row per section round-trips against a real fpvd; toast appears on injected validation errors.

## Open questions

- Exact tab insertion point for Dynamic Link in the tab strip — code is small, will be confirmed when reading the tab registration.
- JSON library choice: cJSON (recommended) vs. a tiny hand-rolled parser. Decided during implementation; keymap and PATCH builder are isolated from this choice.
- cpp-httplib for integration tests vs. hand-rolled — confirm against `extern/` deps and CMake at implementation time.
