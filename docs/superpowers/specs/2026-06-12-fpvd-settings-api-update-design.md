# fpvd Settings Backend Update — Client-Orchestrated API

**Date:** 2026-06-12
**Status:** Approved design
**Supersedes:** parts of `2026-06-03-pixelpilot-settings-via-fpvd-gs-design.md` (endpoint shapes, `/link` coordinator, txpower model)
**API reference:** `fpvd/docs/api.md` (fpvd repo)

## Problem

The fpvd-GS HTTP API changed and PixelPilot's settings provider (`src/gsmenu/settings_fpvd.c`) no longer matches it:

- The `/link` coordinator (`PATCH /link`, `POST /link/apply {"applyTo": ...}`) was **removed**. `link` is now a normal mutable block on both ends; the **client** must orchestrate cross-device changes via `/air/*` then `/gs/*`.
- The GS config tree moved from root (`/config`, `/apply`) to `/gs/config`, `/gs/apply`.
- `link.txpower` (driver mBm) was renamed `link.txPowerDbm` (dBm, useful range −10..30) on both drone and GS. The percent↔mBm driver mapping in `settings_gs_rxpower.c` is obsolete.
- `GET /gs/status` now reports `link.droneReachable` and `beamforming.localMac`.

Operating model: the GS daemon runs on the same host as PixelPilot and is always reachable; the drone may or may not be reachable. Shared link config (channel, bandwidth, linkId) must match on both ends for the link to work.

## Scope

- Rewrite the endpoint layer of `settings_fpvd.c` to the new API.
- Client-side drone-first orchestration for shared link rows (channel, bandwidth).
- New Beamforming toggle in the GS link tab (full client-owned MAC handshake).
- TX power in dBm with a unit label.
- Split reachability into GS-connected vs drone-reachable.

**Out of scope (this round):**
- GS-side `dynamicLink` controller config block (`maxMcs`, `txpower.min/max`, `radioProfile`, `tuning`, …) — no menu UI for it. The drone-side `dynamicLink` rows (via `/air`) stay as they are.
- `idrForward` config UI.
- A linkId menu row (skipped for now; the orchestration mechanism is generic and can cover it later).

## Architecture

Approach: extend the existing single-provider job model. The worker thread, job queue with coalescing, keymap table, and snapshot design are retained. What changes is the endpoint groups and what a job executes.

### 1. Endpoint remap

`FPVD_EP_LINK` and `FPVD_EP_CONFIG` collapse into one `FPVD_EP_GS` group; `FPVD_EP_AIR` is unchanged:

| Group | Read | Write | Apply |
|---|---|---|---|
| `FPVD_EP_AIR` | `GET /air/config` | `PATCH /air/config` | `POST /air/apply` |
| `FPVD_EP_GS` | `GET /gs/config?pending=true` | `PATCH /gs/config` | `POST /gs/apply` |

The worker also polls `GET /gs/status` each poll cycle. It is the source for:
- `link.droneReachable` (reachability model, §2)
- `beamforming.localMac` (beamforming handshake, §4)
- `radio[].txpowerDbm` (live txpower fallback when GS config has `txPowerDbm: null`, §5)

Error parsing handles both shapes:
- GS errors: `{"error": "<human message>"}` with HTTP 400/404/500/502.
- Drone errors relayed via `/air`: `{error, message, details}` (codes `validation`, `dynamic_link_locked`, `bad_json`).
- `502` from `/air/*` means drone unreachable → sets `drone_reachable = false` in addition to failing the job.

### 2. Reachability model

The single `connected` bool splits into two flags in `fpvd_state_t`:

- `gs_connected` — last fpvd-GS HTTP round-trip succeeded (expected ~always true; covers daemon restarts).
- `drone_reachable` — from `/gs/status link.droneReachable`, confirmed/corrected by the result of the `/air/config` poll (a successful `/air` round-trip forces it true; a 502 forces it false).

New dispatcher API: `pp_settings_is_drone_reachable()` alongside the existing `pp_settings_is_connected()` (which keeps meaning "GS daemon reachable").

UI behavior:
- Drone unreachable → all `FPVD_EP_AIR` rows grey out (camera, image, recording, drone wfbng, all Dynamic Link rows) and the Beamforming toggle locks, with a "drone unreachable" hint.
- GS rows stay editable, **including** shared rows (channel, bandwidth) — this is the recovery path for retuning the GS onto the drone's channel.
- GS daemon unreachable (rare) → everything greys out, as today.

### 3. Shared-row orchestration (channel, bandwidth)

Keymap rows get a `bool shared` flag replacing the old `apply_to` string. Shared rows: `gs/wfbng/gs_channel` → `link.channel`, `gs/wfbng/bandwidth` → `link.width`.

Job runner sequence for a shared row when the drone is reachable (drone-first, per the API doc — the GS retunes onto the link the drone has already moved to):

1. `PATCH /air/config {"link": {...}}` — on failure: abort, report error. Nothing diverged.
2. `POST /air/apply` — on failure: abort, report error (check for `radio_bringup_failed` semantics: `validation` code with empty `details`). Drone config not committed, nothing diverged.
3. On drone 200: `PATCH /gs/config {"link": {...}}` → `POST /gs/apply`, retried up to 3 times with short backoff (500 ms) on failure. The GS apply is a live `iw` retune when possible, else a runner bounce.
4. If the GS step still fails after retries: report "Drone moved to <value> — GS apply failed, retry". The row stays dirty so re-committing the same value retries; since the drone side already holds the value, the re-run's drone PATCH+apply is a no-op commit and the flow effectively retries the GS side.

When `drone_reachable == false` at job execution time, the job degrades to GS-only (steps 3–4 without drone steps, no divergence warning blocking it) and the done-callback message notes "applied to GS only — drone unreachable".

No drone rollback on GS failure: the rollback would ride on a link that may already be broken (the drone has retuned away). Retry-then-report is the policy.

### 4. Beamforming toggle (new)

New row `gs/link/beamforming`, type BOOL, rendered in the GS **link** tab. Implemented as a special job type (not a plain keymap PATCH) executing the client-owned MAC handshake from the API doc:

**Enable:**
1. Read `beamforming.localMac` from the cached `/gs/status` snapshot (refresh if stale/missing; fail the job with a clear error if `localMac` is unavailable).
2. `PATCH /air/config {"link": {"beamforming": {"enabled": true, "remoteMac": "<localMac>"}, "stbc": false}}` — STBC and TX-beamforming are mutually exclusive on the drone.
3. `POST /air/apply`.
4. `PATCH /gs/config {"link": {"beamforming": {"enabled": true}}}` → `POST /gs/apply`, same 3×/500 ms retry policy as shared rows. (The GS beamformee self-reconciles; it reads the drone MAC read-only.)

**Disable:**
1. `PATCH /air/config {"link": {"beamforming": {"enabled": false}, "stbc": true}}` — restores STBC per the API doc's flow.
2. `POST /air/apply`.
3. `PATCH /gs/config {"link": {"beamforming": {"enabled": false}}}` → `POST /gs/apply` with retries.

Constraints:
- Rejected before any HTTP when `drone_reachable == false` ("drone unreachable" error, toggle is also visually locked in that state).
- A GS card without a `bf_monitor_conf` node causes a `/gs/config` validation error — surfaced as the row's error toast.
- Toggle state reads from the GS snapshot `link.beamforming.enabled`.

### 5. TX power → dBm

- GS row (`gs/link/rx_power`): path `link.txpower` → `link.txPowerDbm`, type becomes plain `FPVD_T_INT`. Widget: slider **−10..30** with a "dBm" unit label. Always writes an explicit integer (no null/Auto position). If the GS config reports `null`, the displayed value falls back to the live `radio[0].txpowerDbm` from `/gs/status` (rounded to int).
- Drone-side: any keymap path still using `link.txpower` renames to `link.txPowerDbm`. `dynamicLink.safe.txPowerDbm` already matches. Drone TX power rows also get the dBm unit label.
- `LOCKED_PATHS` updates: `link.txpower` → `link.txPowerDbm` (drone-side Dynamic Link lock; per the API doc the locked set is `link.mcs`, `link.txPowerDbm`, `link.fec`, `link.width`, `video.bitrate`, `video.qpDelta`, `video.roi` — `link.channel` is not locked).
- **Deleted:** `settings_gs_rxpower.c`, `settings_gs_rxpower.h`, the `FPVD_T_RXPOWER` type and its handling in the job runner and `prov_get`, plus their tests.
- Dummy provider seeds update to dBm values so the simulator shows the new slider range.

### 6. Pixelpilot staged rows

Display/DVR rows (`pixelpilot.*` paths) move from root `/config` + `/apply` to the same `/gs/config` + `/gs/apply` tree. The stage-then-explicit-Apply behavior is retained: `pixelpilot.*` writes PATCH only and set `config_dirty`; the Apply button enqueues an apply-only job (`POST /gs/apply`).

**Accepted quirk:** the GS pending config is a single tree with a single commit point, so a shared-link or beamforming apply also commits any staged `pixelpilot.*` changes (restarting PixelPilot, since apply granularity is per-block). This matches the daemon's single-commit-point model; the interleaving is rare and not worth a flush/restore dance around link applies.

### 7. Error handling summary

| Scenario | Behavior |
|---|---|
| GS daemon HTTP failure | `gs_connected = false`, all rows grey, 2 s retry poll |
| `/air` returns 502 | `drone_reachable = false`, AIR rows + BF toggle grey, job fails with "drone unreachable" |
| Drone `validation` / `dynamic_link_locked` | First `details` message (or `details.locked` paths) shown as toast |
| Drone apply `radio_bringup_failed` (validation + empty details) | Generic "drone apply failed" toast; full reason is in drone `/status lastApply.error` (not fetched this round) |
| GS `{"error": msg}` | `msg` shown as toast |
| Shared row, drone ok + GS apply fails after retries | "Drone moved — GS apply failed, retry" toast; row stays dirty |
| Shared row while drone unreachable | GS-only apply + "applied to GS only" note |
| BF toggle while drone unreachable | Rejected pre-HTTP, row locked |

### 8. Testing

Host sim build (`USE_SIMULATOR=ON` build-test dir via nix-shell, Catch2):

- Keymap: renamed paths (`link.txPowerDbm`), new `gs/link/beamforming` row, removed `RXPOWER` type, `shared` flags.
- Patch-body builder for the new paths.
- Locked-path checks against the updated `LOCKED_PATHS`.
- **Orchestration step planner:** factor the drone-first / beamforming / GS-only sequencing into a pure function — input (row kind, reachability, value, localMac), output an ordered list of HTTP steps (method, URL suffix, body) plus retry annotations. Unit-test the plans: shared row online (drone-first order), shared row offline (GS-only), BF enable (remoteMac + stbc false), BF disable (stbc true), BF offline (rejected).
- Dummy provider: dBm seeds, BF toggle present, drone-unreachable simulation flag so the greyed states are exercisable in the sim.

## Files touched (expected)

- `src/gsmenu/settings_fpvd.c` — endpoint remap, reachability split, orchestration runner, step planner
- `src/gsmenu/settings_fpvd_internal.h` — `FPVD_EP_GS`, `shared` flag, step-planner types, drop `FPVD_T_RXPOWER`
- `src/gsmenu/settings.c` / `settings.h` — `pp_settings_is_drone_reachable()`
- `src/gsmenu/settings_gs_rxpower.c/h` — **deleted**
- `src/gsmenu/settings_dummy.c` — dBm seeds, BF row, drone-offline sim
- GS menu UI files — dBm slider range/unit label, BF toggle row, drone-unreachable hints
- Tests — updated/added per §8
