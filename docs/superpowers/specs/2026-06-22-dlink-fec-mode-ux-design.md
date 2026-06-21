# Dynamic-link FEC-mode-aware UX — design

## Summary

Make the dynamic-link lock UX aware of the Link page's FEC mode (`link.fec.mode`,
values `rs` / `swfec`) and drop the obsolete Failsafe section from the Dynamic
Link page. Specifically, when Dynamic Link is enabled:

- **FEC Mode** is always editable (never greyed by Dynamic Link).
- In **swfec** mode, **Deadline (ms)** and **Overhead (%)** become editable, and
  the Dynamic Link page's **Base Redundancy Ratio** and **Blocks / Frame** rows
  are greyed out (swfec ignores them).
- In **rs** mode, **FEC_K** / **FEC_N** stay greyed (Dynamic Link drives them),
  unchanged from today.

The whole **Failsafe** section is removed from the Dynamic Link page, including
its backing keymap entries, sim defaults, and obsolete tests.

This is a ground-station-menu-only change. No drone-side behavior changes.

## Context

The lock engine is centralized and data-driven:

- `prov_is_locked(d,p,k)` (`src/gsmenu/settings_fpvd.c`) reads the live
  `air_snapshot` to decide whether a row is locked by Dynamic Link.
- `pp_page_reapply_lock_state(page)` (`src/gsmenu/helper.c`) re-evaluates every
  row's lock state on each snapshot fanout, by calling `pp_settings_is_locked`
  per row. Rows grey / un-grey purely from the predicate — no per-page lock
  wiring is required.
- After a `set`, the worker re-fetches `/air/config`, refreshes `air_snapshot`,
  and fires the listener fanout. This already drives the Link page's `fec_mode`
  row visibility (`apply_fec_visibility` in `src/gsmenu/pages/link.c`). Reading
  `link.fec.mode` from that same snapshot in the lock predicate keeps lock state
  and visibility in sync.

Today `link.fec` is a coarse locked-path prefix (`LOCKED_PATHS` in
`settings_fpvd.c`), so **all** of `fec_mode` / `fec_k` / `fec_n` /
`fec_deadline_ms` / `fec_overhead_pct` are greyed whenever Dynamic Link is on.
The new UX needs per-field exceptions instead of the blanket prefix lock.

Two enforcement points share the same (currently duplicated) lock logic and must
stay consistent, or an unlocked row's push would still be rejected:

1. The widget-grey predicate `prov_is_locked` (~line 950).
2. The apply/push gate in `prov_set_async` (~line 881), which rejects pushes of
   locked AIR fields with `"Locked by Dynamic Link"`.

Relevant keymap entries (`settings_fpvd.c`):

```
{ "air", "wfbng", "fec_mode",         "link.fec.mode",        FPVD_T_ENUM, FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "wfbng", "fec_deadline_ms",  "link.fec.deadlineMs",  FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "wfbng", "fec_overhead_pct", "link.fec.overheadPct", FPVD_T_INT,  FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "wfbng", "fec_k",            "link.fec.k",           ... },
{ "air", "wfbng", "fec_n",            "link.fec.n",           ... },
{ "air", "dlink", "compute_base_redundancy",  "dynamicLink.compute.baseRedundancyRatio", FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
{ "air", "dlink", "compute_blocks_per_frame", "dynamicLink.compute.blocksPerFrame",      FPVD_T_FLOAT, FPVD_EP_AIR, FPVD_ROW_PLAIN },
```

Note `dynamicLink.compute.*` is **not** under `LOCKED_PATHS` — those rows are
never locked today.

## Approach

**Data-driven, mode-aware central predicate.** Teach the one central lock
predicate the FEC-mode exceptions, factor it into a shared helper used by both
the widget-grey path and the apply gate, and mirror it in the sim provider.
Pages need zero new lock code; the existing reapply-on-snapshot fanout re-greys
the compute rows automatically. The Dynamic Link page's only change is deleting
the Failsafe section.

Rejected alternatives:

- **Page-level lock toggling** (have `dynamiclink.c`'s snapshot listener
  imperatively lock/unlock the two compute rows): splits lock logic between the
  provider and the page, diverges from the centralized model, and still needs
  the apply-gate fix.
- **Hide compute rows on swfec instead of greying**: the requirement is to grey
  them out (keep the affordance visible); the lock states already render that.

## Changes

### `src/gsmenu/settings_fpvd.c` — shared mode-aware lock predicate

Add a helper that captures the full rule. It must be called WITHOUT holding
`G.mu` (it takes the lock internally to read the snapshot):

```c
/* True when the Dynamic Link lock currently governs this field, accounting for
 * the FEC-mode-aware exceptions:
 *   - fec_mode is always editable (the user selects rs/swfec even with DL on).
 *   - In swfec mode, deadlineMs/overheadPct are editable, and the compute
 *     baseRedundancyRatio/blocksPerFrame become locked (swfec ignores them).
 * Caller passes the already-resolved keymap entry; must NOT hold G.mu. */
static bool dl_locks_field(const fpvd_keymap_entry_t *e,
                           const char *d, const char *p, const char *k) {
    pthread_mutex_lock(&G.mu);
    cJSON *dlink = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "dynamicLink") : NULL;
    cJSON *en    = dlink ? cJSON_GetObjectItemCaseSensitive(dlink, "enabled") : NULL;
    bool dl_on   = en && cJSON_IsTrue(en);
    cJSON *link  = G.air_snapshot ? cJSON_GetObjectItemCaseSensitive(G.air_snapshot, "link") : NULL;
    cJSON *fec   = link ? cJSON_GetObjectItemCaseSensitive(link, "fec") : NULL;
    cJSON *mode  = fec ? cJSON_GetObjectItemCaseSensitive(fec, "mode") : NULL;
    bool swfec   = mode && cJSON_IsString(mode) && strcmp(mode->valuestring, "swfec") == 0;
    pthread_mutex_unlock(&G.mu);

    if (!dl_on) return false;

    bool is_air_wfbng = (!strcmp(d, "air") && !strcmp(p, "wfbng"));
    bool is_air_dlink = (!strcmp(d, "air") && !strcmp(p, "dlink"));

    /* FEC Mode: always editable. */
    if (is_air_wfbng && !strcmp(k, "fec_mode")) return false;
    /* swfec: deadline/overhead editable; rs: they're hidden anyway. */
    if (swfec && is_air_wfbng &&
        (!strcmp(k, "fec_deadline_ms") || !strcmp(k, "fec_overhead_pct")))
        return false;
    /* swfec: the compute redundancy/blocks knobs are ignored, so grey them. */
    if (swfec && is_air_dlink &&
        (!strcmp(k, "compute_base_redundancy") || !strcmp(k, "compute_blocks_per_frame")))
        return true;

    /* Default: locked iff the path is under a locked prefix. */
    return fpvd_is_locked_path(e->path);
}
```

Rule table (Dynamic Link **on**):

| Field | rs mode | swfec mode | today |
| --- | --- | --- | --- |
| `fec_mode` | unlocked | unlocked | locked |
| `fec_deadline_ms` / `fec_overhead_pct` | locked (hidden) | **unlocked** | locked |
| `fec_k` / `fec_n` | locked | locked (hidden) | locked |
| `compute_base_redundancy` / `compute_blocks_per_frame` | unlocked | **locked** | unlocked |
| other `LOCKED_PATHS` (bandwidth, bitrate, roi, …) | locked | locked | locked |

When Dynamic Link is **off**, `dl_locks_field` returns `false` for everything
(unchanged). The compute rows are hidden when Dynamic Link is off, so their lock
state is moot in that case.

**Rewire the two enforcement points to the helper:**

- `prov_is_locked`: keep the existing `pp_runtime_cfg_owns` short-circuit, the
  keymap lookup, and the endpoint / bandwidth gate. Replace the final
  `fpvd_is_locked_path(e->path)` + snapshot `dlink_on` block with
  `return dl_locks_field(e, d, p, k);`. (The bandwidth exception path —
  `gs/wfbng/bandwidth` → `link.width` — flows through `fpvd_is_locked_path` in
  the helper's default branch and remains locked when DL is on.)
- `prov_set_async` apply gate (~line 881): replace the inline
  `e->endpoint == FPVD_EP_AIR && fpvd_is_locked_path(...)` + `dlink_on` block
  with `if (e->endpoint == FPVD_EP_AIR && dl_locks_field(e, d, p, k)) {
  schedule_done(cb, ud, -1, "Locked by Dynamic Link"); return; }`.

This guarantees the widget-grey state and the push-rejection decision are
identical for every field.

### `src/gsmenu/settings_fpvd.c` — remove Failsafe keymap entries

Delete the four entries:

```
{ "air", "dlink", "safe_mcs",          "dynamicLink.safe.mcs",         ... },
{ "air", "dlink", "safe_k",            "dynamicLink.safe.k",           ... },
{ "air", "dlink", "safe_n",            "dynamicLink.safe.n",           ... },
{ "air", "dlink", "safe_bitrate_kbps", "dynamicLink.safe.bitrateKbps", ... },
```

`LOCKED_PATHS` is unchanged.

### `src/gsmenu/pages/dynamiclink.c` — remove Failsafe section

Delete the `pp_section_header(page, "Failsafe")` block and all four rows
(`safe_mcs`, `safe_k`, `safe_n`, `safe_bitrate_kbps`) plus the two
`pp_slider_set_relation` calls binding `safe_k`/`safe_n`.

No lock wiring is added to this page: the Base Redundancy / Blocks-per-Frame rows
grey and un-grey purely from `dl_locks_field` via the existing
`pp_page_reapply_lock_state` fanout (already invoked by `snapshot_listener_cb`).

### `src/gsmenu/settings_dummy.c` — sim parity

- Remove the four `safe_*` seed defaults
  (`safe_mcs`, `safe_k`, `safe_n`, `safe_bitrate_kbps`).
- Update `dummy_is_locked` to mirror `dl_locks_field`:
  - Drop `fec_mode` from `g_dummy_locked_keys` (always editable).
  - Keep the early `if (!dlink_on) return false;`.
  - Read the current `fec_mode` value via `find_value("fec_mode")`; let
    `swfec = mode && strcmp(mode, "swfec") == 0`.
  - For `fec_deadline_ms` / `fec_overhead_pct`: when `swfec`, return `false`
    (editable); otherwise fall through to the locked-keys list. Simplest: keep
    them in `g_dummy_locked_keys` but add an early
    `if (swfec && (key is deadline/overhead)) return false;`.
  - For `compute_base_redundancy` / `compute_blocks_per_frame`: add
    `if (swfec && (key is one of these)) return true;` (these are not in
    `g_dummy_locked_keys`, so without this they would stay unlocked).

The sim's overlay updates synchronously, so flipping the FEC Mode dropdown
re-greys / un-greys the affected rows immediately in `sim.sh`.

### Tests

- `tests/test_settings_fpvd.cpp`: remove the `safe_*` keymap-lookup assertions
  (the `safe_bitrate_kbps` lookup near the top and the `safe_mcs` lookup further
  down). No assertion should reference a `dynamicLink.safe.*` path after this.
- `tests/test_dynamiclink_page.cpp`: replace the two Failsafe cases. Assert the
  page no longer builds a `"Failsafe"` header nor any of the rows
  `"MCS"`, `"FEC K"`, `"FEC N"`, `"Bitrate (kbps)"`, and that the surviving
  Compute rows (e.g. `"Base Redundancy Ratio"`, `"Blocks / Frame"`,
  `"Min Bitrate"`, `"Max Bitrate"`, `"Max MCS"`) are still present.
- Lock behavior (mode-aware grey/unlock): exercise via the sim provider in the
  `test_settings_fpvd*` suite where `fec_mode` and `enabled` are injectable
  through the dummy overlay — assert `pp_settings_is_locked` returns the
  expected value for `fec_mode`, `fec_deadline_ms`, `fec_overhead_pct`,
  `compute_base_redundancy`, `compute_blocks_per_frame`, and `fec_k`/`fec_n`
  across `{dlink off, dlink on + rs, dlink on + swfec}`.

## Data flow

1. User opens the **FEC Mode** dropdown on the Link page and selects `rs` /
   `swfec`. The widget calls `pp_settings_set("air","wfbng","fec_mode", value)`.
2. The apply gate runs `dl_locks_field` for `fec_mode` → returns `false` → the
   push is allowed (no `"Locked by Dynamic Link"`).
3. The worker pushes to the drone, re-GETs `/air/config`, refreshes
   `air_snapshot` (now `link.fec.mode == <value>`), and fires the listener
   fanout.
4. Every registered page listener runs:
   - Link page: `apply_fec_visibility` shows the matching row group (k/n vs
     deadline/overhead) and `pp_page_reapply_lock_state` re-greys per
     `dl_locks_field` → on swfec, deadline/overhead unlock.
   - Dynamic Link page: `pp_page_reapply_lock_state` → on swfec, Base
     Redundancy + Blocks / Frame grey out; on rs, they un-grey.

## Out of scope

- Any drone-side FEC or dynamic-link behavior.
- Friendly dropdown labels for FEC mode (would need a label→value mapping layer).
- Changing which Compute rows exist beyond the lock behavior (Min/Max Bitrate
  and Max MCS remain editable in both modes).
- Relations/validation among `deadlineMs` / `overheadPct`.
