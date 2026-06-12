# Drone-reachable detection: wire up visibility + refresh on menu open

**Date:** 2026-06-12
**Scope:** PixelPilot only (`src/gsmenu/settings_fpvd.c`, `src/input.cpp`, `src/menu.c`)

## Problem

Drone-config rows in the settings menu stay locked (`PP_ROW_LOCKED_OFFLINE`)
for 30s or more after the drone comes online, even though the intended
menu-open poll cadence is 3s.

Root cause: the fpvd settings worker polls `/air/config` every
`G.visible ? 3000 : 60000` ms (`settings_fpvd.c:786`), but
`pp_settings_set_visibility()` is **never called from production code** —
only from `tests/test_settings.cpp`. `G.visible` is initialized `false`
(`settings_fpvd.c:1020`) and stays false forever, so the effective cadence
with the menu open is 60s (~30s average wait), and a transient probe failure
sticks for a further full minute, making blips look like real outages.

A second, smaller gap: the worker only refreshes when its
`pthread_cond_timedwait` returns `ETIMEDOUT` (`settings_fpvd.c:793`). A
visibility change signals the CV, which merely restarts the wait — so even
with visibility wired, opening the menu would inherit up-to-60s-stale state
and still wait up to one more 3s tick before probing.

## Design

Two changes, no new mechanisms:

### 1. Wire visibility to menu open/close

- `toggle_screen()` (`src/input.cpp:443`) — the only place that sets
  `menu_active = true` — additionally calls
  `pp_settings_set_visibility(true)`.
- `on_tabbar_cancel()` (`src/menu.c:30`) — the only place that sets
  `menu_active = false` — additionally calls
  `pp_settings_set_visibility(false)`.

`pp_settings_set_visibility()` (`settings.c:113`) already null-checks the
provider, so calling it before a provider registers (or under the dummy/stub
providers, which lack `set_visibility`) is safe.

### 2. Refresh immediately on the hidden→visible transition

In the fpvd provider (`settings_fpvd.c`):

- Add `bool refresh_now;` to the worker state `G`.
- `prov_set_visibility(v)`: when `v` is true and `G.visible` was false, set
  `G.refresh_now = true` before signaling the CV (it already signals).
- Worker loop (`worker_main`): inside the idle-wait loop, refresh when
  `G.refresh_now` is set — checked both **before** waiting (so a flag set
  while the worker was busy running a job isn't missed once the signal is
  consumed) and after `pthread_cond_timedwait` returns — or when the wait
  returns `ETIMEDOUT`, as today. Clear `G.refresh_now` before refreshing.
  The existing notify-after-unlock dance is unchanged.

Result: opening the menu triggers a probe within milliseconds; rows unlock
as soon as the `/air/config` round-trip succeeds (sub-second when the drone
is up), instead of 0–60s later.

## Explicitly out of scope

- Debounce/hysteresis on `drone_reachable` (single-sample flip stays; the
  3s cadence makes blips self-correct quickly enough).
- Any fpvd-GS daemon change.
- The 60s hidden cadence and the stale-snapshot-retention behavior.

## Error handling

No new failure modes: `refresh_now` is read/written under `G.mu` like the
rest of the worker state. A spurious CV wakeup without `refresh_now` simply
re-enters the wait, as today.

## Testing

- **Unit (host sim build, `USE_SIMULATOR=ON`, Catch2):** extend
  `tests/test_settings.cpp` — after `pp_settings_set_visibility(true)`, the
  provider performs a refresh promptly (observable via the snapshot
  listener firing) without waiting for a poll tick.
- **On-target verification (GS `root@10.18.0.1`):** with the drone off,
  open the menu (rows locked), power the drone
  (`root@192.168.10.152`), and confirm rows unlock within ~3–8s; close and
  reopen the menu and confirm an immediate probe (log timing).
