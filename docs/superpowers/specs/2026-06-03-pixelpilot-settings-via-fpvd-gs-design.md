# Route PixelPilot settings through the local GS fpvd

- **Date:** 2026-06-03
- **Status:** Approved (design)
- **Component:** `src/gsmenu` settings backend (PixelPilot_rk)
- **Depends on:** fpvd ground-station daemon API — see `fpvd/docs/api.md` (sections "Ground-station API (fpvd-GS)", "Link coordinator", "Drone proxy — /air/*")

## Problem

PixelPilot runs on the ground station. Its settings backend currently has two write
paths, both of which now collide with the new ground-station fpvd daemon (fpvd-GS):

- `settings_fpvd.c` talks **directly to the drone** at `http://10.5.0.10:8080`
  (`PATCH /config` + `POST /apply`) over the wfb tunnel.
- `settings_gs_local.c` writes **`/etc/wifibroadcast.cfg` directly** and restarts
  `S98wifibroadcast` via init.d for channel / bandwidth / rx-power.

fpvd-GS now **owns** `/etc/wifibroadcast.cfg`, supervises the wfb runner, and is the
intended single front door: GS radio via `/link` + `/config`, and the drone reached
opaquely through the `/air/*` proxy. Direct file writes fight fpvd-GS for ownership of
the cfg and the runner; direct-to-drone calls bypass the coordinator that keeps the GS
and drone radios in sync.

## Goal

PixelPilot's settings backend talks **only** to the local GS fpvd
(`http://127.0.0.1:8080`). No PixelPilot code opens the tunnel to the drone or writes
GS config files; fpvd-GS is the sole backend for both reads and writes.

## Non-goals

- Modeling GS-local settings that fpvd-GS does not expose (HDMI output mode, the
  PixelPilot decoder codec, init.d restart actions). Those rows are **dropped this
  round** until fpvd-GS grows support for them.
- Changing fpvd-GS itself (e.g. adding driver-aware or per-NIC txpower).
- Reworking the UI/menu model, the keymap's domain/page/key scheme, or the
  worker-thread/queue mechanics — those are reused as-is.

## Decisions (resolved during brainstorming)

1. **Topology:** single local front door at `http://127.0.0.1:8080`. The drone is
   reached only through fpvd-GS's `/air/*` proxy. The base URL stays overridable via
   the existing `PP_FPVD_URL` env var (default changes from `10.5.0.10:8080` to
   `127.0.0.1:8080`) so the test/sim harness can point at a stub.
2. **Non-radio GS settings dropped:** HDMI output mode, GS decoder codec, and init.d
   actions are removed/disabled this round (fpvd-GS does not model them).
3. **Structure — single unified provider (Approach A):** collapse to one provider.
   Delete the router and the GS-local file/init.d machinery.
4. **RX power — keep mapping (verified):** fpvd-GS treats `link.txpower` as a single
   mBm scalar applied via `iw set txpower fixed` and rendered as one global
   `wifi_txpower`. For the deployed `rtl88x2eu` (BL-M8812EU2) hardware the existing
   `pct→value` mapping already produces valid positive mBm, so behavior is unchanged.
   We keep the mapping and send the result as `link.txpower`.

## Architecture

One provider, one backend. `settings_fpvd.c` becomes the **sole** settings provider,
talking only to the local GS fpvd. It reuses what it already owns: the libcurl client,
the worker thread + job queue, the keymap, the type conversions, and the snapshot
reader. The router, the GS-local writers, and the init.d machinery are deleted.

### Endpoint routing (keyed off domain)

Each keymap entry is assigned an **endpoint group** (derivable from its domain). The
PATCH bodies are unchanged — `fpvd_build_patch_body("link.channel", …)` already emits
`{"link":{"channel":N}}`, which is exactly what both `/air/config` (opaque proxy) and
`/link` accept. Only the **URL** and the **apply call** differ per group.

| Group | Settings (keymap rows) | Write | Apply | Read source |
|-------|------------------------|-------|-------|-------------|
| **drone** (`air/*`) | video, image, recording, `link.mcs/fec/stbc/ldpc`, `dynamicLink.*`, camera codec | `PATCH /air/config` | `POST /air/apply` | `GET /air/config` |
| **gs-link, shared** (`gs/wfbng/gs_channel`, `gs/wfbng/bandwidth`) | `link.channel`, `link.width` | `PATCH /link` | `POST /link/apply {"applyTo":"both"}` | `GET /link` |
| **gs-link, local** (`gs/link/rx_power`) | `link.txpower` | `PATCH /link` | `POST /link/apply {"applyTo":"gs"}` | `GET /link` |

`applyTo:"both"` lets fpvd-GS push `channel`/`width`/`linkId` to the drone
**server-side** (it prefers a live `iw` retune, no runner bounce). This is what removes
the old client-side fan-out: the `FANOUT` table (channel, bandwidth, codec) is deleted.
`rx_power` uses `applyTo:"gs"` because fpvd-GS never pushes txpower to the drone, and
`"both"` would needlessly re-push channel/width and trigger a drone retune-defer.

Verified against fpvd-GS source: `schema.LINK_KEYS = {channel, width, txpower, region,
linkId, beamforming, wlans}` (so `PATCH /link` accepts all three of our writes);
`link.DRONE_PUSH_KEYS = (channel, width, linkId)` (so txpower stays GS-only even under
`applyTo:"both"`).

### Reads / snapshot

The provider keeps **two snapshot sub-views**, refreshed on the same poll cycle and
visibility throttling as today:

- `air` ← `GET /air/config` — the full drone config; `link.mcs`, `video.*`,
  `dynamicLink.*`, etc. resolve directly against existing keymap paths.
- `gs` ← `GET /link` — this endpoint returns the link fields **flat**
  (`{"channel":…, "width":…, "txpower":…, "droneReachable":…}`), so the provider
  wraps it as `{"link": {…}}` before storing, letting the existing dotted-path reader
  resolve `link.channel` / `link.width` / `link.txpower` unchanged.

`prov_get(d,p,k)` selects the sub-view by the keymap entry's domain (`air/*` → `air`,
`gs/*` → `gs`).

### RX power

Reuse `settings_gs_rxpower.c`'s `pct→driver-value` mapping and driver/NIC detection.
That value already *is* the wfb-ng `wifi_txpower` mBm that fpvd-GS wants.

- **Set:** detect the primary NIC's driver → `pct→value` → `PATCH /link
  {link:{txpower:value}}` + `POST /link/apply {"applyTo":"gs"}`. If the driver is
  `UNKNOWN`, skip/disable the row rather than send a bogus value.
- **Read:** seed the percent slider by inverse-mapping the effective `link.txpower`
  from `GET /link`; cache the last-set pct (as `gs_local` did via `G.rxpower_pct`),
  defaulting to a sensible value when `txpower` is null (driver default).
- **Keep** in `settings_gs_rxpower.c`: `pp_nic_driver_from_name`,
  `pp_rxpower_pct_to_driver_value`, `pp_rxpower_list_wlx_nics`,
  `pp_rxpower_nic_driver_name`. Add an inverse `value→pct` helper for read-back.
- **Drop** `pp_rxpower_build_json` (fpvd-GS takes one scalar, not a per-NIC dict).

### Errors, locks, connection

- **Error shapes:** parse both the drone's `{error, message, details}` (relayed via
  `/air`, including `dynamic_link_locked` and `validation`) and the GS's `{error}`
  from `/link`/`/config`. Prefer `message`, fall back to `error`.
- **Adaptive-link lock** (`is_locked`): keep evaluating against the **air** snapshot —
  the lock only applies to `air/*` rows (`link.mcs`, `link.fec`, `video.bitrate`,
  `video.qpDelta`, `video.roi`, …). `gs/*` rows are never locked client-side.
- **Connection:** `is_connected` = GS fpvd reachable (the `/air/config` / `/link`
  fetch succeeded). `droneReachable` from the `/link` view is captured and available
  for a future "drone offline" indicator (not wired to UI this round).

## Files

### Delete

- `settings_router.c`, `settings_router_internal.h`
- `settings_gs_local.c`, `settings_gs_local_internal.h`
- `settings_gs_writers.c`, `settings_gs_writers.h`
- `pp_rxpower_build_json` (from `settings_gs_rxpower.c` / `.h`)

### Modify

- `settings_fpvd.c` / `settings_fpvd_internal.h` — add the endpoint-group concept;
  per-group write/apply/read URLs; two-snapshot storage and resolution; default base
  URL → `127.0.0.1:8080`; rx-power handling (set + read-back); dual error-shape
  parsing.
- `settings_gs_rxpower.c` / `.h` — drop the per-NIC JSON builder; add `value→pct`
  inverse for slider read-back.
- `settings_gs_enum.c` / `.h` — keep `pp_gs_enum_channels()` (read-only `iw`, no
  conflict with fpvd); drop `pp_gs_enum_hdmi_modes()` and its row.
- `src/main.cpp:1442` — `pp_settings_register_router()` → `pp_settings_register_fpvd()`.
- The menu/UI definitions — remove the HDMI output mode, GS decoder codec, and
  init.d restart-action rows (and any options wiring for them).
- `CMakeLists.txt` — drop deleted sources from device, sim, and test targets.

### Keep

- The rxpower pct↔value + driver/NIC helpers (see RX power above).
- `pp_gs_enum_channels()` for the channel dropdown.
- The keymap, type conversions, worker/queue, and snapshot reader in `settings_fpvd.c`.

## Tests

Follow TDD (red → green) per the writing-plans flow.

- **Remove:** `tests/test_settings_router.cpp`, `tests/test_settings_gs_writers.cpp`.
- **Trim:** `tests/test_settings_gs_enum.cpp` to channels only;
  `tests/test_settings_gs_rxpower.cpp` to the surviving pct/driver helpers (+ new
  inverse helper).
- **Add / extend** (`tests/test_settings_fpvd.cpp`,
  `tests/test_settings_fpvd_integration.cpp`):
  - endpoint-group selection: `air/*` → `/air/config` + `/air/apply`; `gs/*` →
    `/link` + `/link/apply`.
  - `applyTo` choice per row: `both` for channel/width, `gs` for rx_power.
  - `/link` PATCH body shape (`{"link":{…}}`).
  - two-snapshot read resolution (`GET /air/config` vs wrapped `GET /link`).
  - rx-power `pct→value` on set and `value→pct` on read-back; `UNKNOWN` driver
    disables the row.
  - dual error-shape parsing (`{error,message,details}` and `{error}`).

## Known limitations / out of scope

- **88XXau RX power:** fpvd-GS is mBm-based and driver-agnostic; the `rtl88xxau_wfb`
  negative-value convention is not honored by `iw set txpower fixed`. Closing this
  requires *server-side* driver-aware txpower in fpvd-GS. Out of scope; the deployed
  `rtl88x2eu` hardware is unaffected.
- **Mixed-driver GS** (two NICs, different drivers): fpvd-GS takes one txpower for all
  cards, so rx-power maps from the *primary* NIC's driver. Fine for single-driver rigs;
  a known simplification otherwise.
- **Width change while drone adaptive-link is active:** `link.width` is a drone-locked
  field when `dynamicLink.enabled`. An `applyTo:"both"` push of width could be rejected
  by the drone. Coordinating that is fpvd-GS's responsibility; PixelPilot surfaces
  whatever `/link/apply` returns.
- **Dropped non-radio rows** (HDMI mode, decoder codec, restart actions) return when
  fpvd-GS models them, or via a separate local-only follow-up.

## Open follow-ups (not this round)

- Wire `droneReachable` to a UI "drone offline" indicator.
- Reconsider the decoder codec: it is now drone-only via `/air`. If the PixelPilot
  decoder must be told h264 vs h265 (rather than auto-detecting the stream), a GS-side
  codec signal will be needed again.
