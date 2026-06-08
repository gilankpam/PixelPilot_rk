# GS Menu: Add DFS Channels to the Channel Dropdown

**Date:** 2026-06-08
**Status:** Approved
**Scope:** Single-file change to the GS menu Link tab.

## Goal

Add the full 5GHz DFS channel set to the GS menu channel dropdown so they are
selectable for the link. The motivation is **more spectrum options** to escape
congestion on the non-DFS channels. The radar/regulatory implications of using
DFS channels are knowingly accepted; the menu only *offers* the option.

## Current State

The channel dropdown is hardcoded in `src/gsmenu/pages/link.c:14-16`:

```c
pp_dropdown(page, LV_SYMBOL_WIFI, "Channel",
            "gs", "wfbng", "gs_channel",
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n149\n153\n157\n161\n165");
```

Offered today: `1–11` (2.4GHz), `36/40/44/48` (UNII-1), `149/153/157/161/165`
(UNII-3). No DFS channels (`52–144`).

Relevant mechanics that constrain the change:

- **Value round-trip:** `pp_dropdown` sends the selected option **string
  verbatim** to the backend (`pp_dropdown.c:228,238`). `gs_channel` maps to
  `link.channel` as an integer (`settings_fpvd.c`).
- **Load-time matching:** on build, the stored value is matched against options
  by **exact string compare** (`pp_dropdown.c:324-327`). The option text and the
  stored value must be byte-identical for the dropdown to show the current
  selection.
- Bare numeric options (e.g. `"36"`) satisfy both: they parse cleanly as ints
  and round-trip exactly.

A separate dynamic enumerator (`settings_gs_enum.c`, `pp_gs_enum_channels`)
parses `iw list` and **filters out** channels marked `"radar detection"`
(lines 34-35). It is **not wired into `link.c`** and is out of scope here.

## Change

Single edit to the options string in `src/gsmenu/pages/link.c:16`. Insert the
full DFS set as **bare numbers**, in numeric order, between `48` and `149`:

DFS channels added:
- UNII-2A: `52, 56, 60, 64`
- UNII-2C: `100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144`

Resulting options string:

```
1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n52\n56\n60\n64\n100\n104\n108\n112\n116\n120\n124\n128\n132\n136\n140\n144\n149\n153\n157\n161\n165
```

## Explicitly Out of Scope

- `settings_gs_enum.c` radar-detection filter — left unchanged (not wired into
  `link.c`).
- `pp_dropdown.c` — unchanged. Bare numbers preserve the verbatim-string
  round-trip and `link.channel` int parsing.
- Backend / fpvd mapping — unchanged.
- Any visual DFS marker (e.g. `"52 (DFS)"`) — rejected; it would require
  splitting display text from the sent value and break the round-trip.

## Behavior

Selecting a DFS channel sends e.g. `"52"` exactly as `"36"` is sent today. On
reload, stored `"52"` matches its option by exact string compare. No new code
paths.

## Risk Note

DFS channels are radar-restricted. Making them selectable is a deliberate
regulatory trade-off accepted for this build. Whether the link actually keys up
on a DFS channel still depends on the adapter/driver regdomain — the menu only
presents the option.

## Verification

No new unit-test surface: the dropdown options are a static string, and the
existing parser test (`tests/test_settings_gs_enum.cpp`) does not cover
`link.c`. Verification is:

1. Host-sim build compiles cleanly.
2. Manual check on the GS that the dropdown lists `52–144` in order and that
   selecting a DFS channel applies and reloads correctly (value persists and
   re-selects on menu reopen).
