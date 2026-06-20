# gsmenu: disable Bandwidth row when Dynamic Link is enabled

**Date:** 2026-06-04
**Status:** Approved (design)

## Problem

The Link tab's **Bandwidth** dropdown (`gs`/`wfbng`/`bandwidth`) maps to `link.width`,
a path the drone's Dynamic Link controller locks (`kLockedPaths` in the drone fpvd;
mirrored in the GS keymap `LOCKED_PATHS`). When Dynamic Link is enabled, the drone
rejects any `PATCH /config` that writes `link.width` — even unchanged — with
`dynamic_link_locked`. So a user can still select a Bandwidth value in the menu while
DL is on, but the change cannot take effect on the drone. The row should be disabled
(locked) while DL is enabled, exactly like the MCS/FEC rows already are.

This is the UI complement to the GS-daemon delta-push fix (fpvd
`fix/link-channel-delta-push`): that stops an *unchanged* width from poisoning a
channel change; this stops the user from attempting a *real* width change under DL.

## Scope

- **In scope:** the Bandwidth row only.
- **Out of scope:** `rx_power` (`gs`/`link`/`rx_power` → `link.txpower`). It also maps to a
  DL-locked path, but it is the **GS card's own** power (`apply_to:"gs"`), not
  drone-controlled, so it stays editable. AIR rows are unchanged.

## Design

Single change in `prov_is_locked` (`src/gsmenu/settings_fpvd.c`). Today it bails for any
non-AIR row:

```c
if (e->endpoint != FPVD_EP_AIR) return false;
```

Add a narrow special-case so the Bandwidth row is also eligible, then fall through to the
existing `fpvd_is_locked_path(e->path)` + `dynamicLink.enabled` gates:

```c
bool is_bandwidth = (!strcmp(d, "gs") && !strcmp(p, "wfbng") && !strcmp(k, "bandwidth"));
if (e->endpoint != FPVD_EP_AIR && !is_bandwidth) return false;
```

No other code changes. The existing machinery reuses this decision:

- `pp_dropdown()` at build time and `pp_page_reapply_lock_state()` (`helper.c`) on every
  snapshot change call `pp_settings_is_locked` and apply `PP_ROW_LOCKED_DYNAMIC` —
  greyed row + lock icon, and the dropdown's key handler shows a "Locked by Dynamic Link"
  toast on edit attempts.
- Because the air snapshot listener drives `pp_page_reapply_lock_state`, the Bandwidth row
  locks/unlocks **live** when Dynamic Link is toggled.

### Why match `d/p/k` and not the path

Matching the explicit `gs`/`wfbng`/`bandwidth` keys is the most direct expression of the
chosen scope ("Bandwidth only"). Matching by `link.width` would be equivalent today but
implicitly assumes only one row ever maps to `link.width`; a keymap flag would add infra
for a single row. Narrowest, most readable wins.

## Testing

TDD via Catch2 in `tests/test_settings_fpvd_integration.cpp`, mirroring the existing
`integration: dynamic_link_locked rejected client-side (air snapshot)` case:

1. Mock the air snapshot with `dynamicLink.enabled:true`; register the fpvd provider.
2. New assertions (written failing first, then fixed):
   - `pp_settings_is_locked("gs", "wfbng", "bandwidth") == true`
   - `pp_settings_is_locked("gs", "link", "rx_power") == false`  (regression guard: GS-only stays editable)
3. Optionally assert that with `dynamicLink.enabled:false`, Bandwidth is not locked.

Verify on the host `build-test` suite (`USE_SIMULATOR=ON`), then spot-check on device.

## Risks

Minimal. One added condition in an existing predicate; no new state, no interface change.
The `rx_power` regression assertion guards the one nearby row that must stay editable.
