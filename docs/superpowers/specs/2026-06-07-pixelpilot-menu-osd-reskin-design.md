# PixelPilot GS Menu — OSD Reskin (design)

**Date:** 2026-06-07
**Status:** approved-for-planning (pending user review)
**Handoff:** `design_handoff_pixelpilot_menu/` (README + JSX prototype + `assets/`)
**Supersedes the look of:** `docs/superpowers/specs/2026-05-27-gsmenu-redesign-design.md`

## Overview

Re-skin the existing PixelPilot ground-station config menu (`src/gsmenu/`) into the
racing-HUD "OSD" visual language from the handoff: amber accent, Barlow Condensed
type, opaque flat panels over a dimmed full-frame video scrim, centered floating
panel, and a static footer key-legend.

This is a **visual reskin only**. The menu is already fully functional — tab rail,
five pages, 4-button NAV/EDIT state machine, async settings backend, busy/lock
states, and toasts all work. We change **tokens, fonts, layout, and per-widget
styling**; we do **not** change the field inventory, the navigation model, or any
control behavior.

### In scope
- Amber palette replacing the blue/indigo tokens (centralized in `styles.h`).
- Barlow Condensed font (two weights) replacing Geist.
- Full-frame dim scrim behind the panel.
- Centered floating opaque panel (rail + content) replacing the left-anchored 45% panel.
- Tab rail restyle: opaque fill, 4px amber active-bar, amber-active / dim-inactive icon+label.
- Row restyle: remove per-row leading icons; amber focus bar + light focus tint.
- Control restyle: toggle (amber track), stepper (amber edit value), section header.
- Select picker: rebuild the option popup as a **centered modal** (amber border, header, CURRENT tag).
- **Static footer legend**: `▲▼ NAVIGATE · ▶ ENTER · ◀ BACK`.
- Toast restyle to the design's dark-panel look.
- Bundle + install the Barlow Condensed TTFs.

### Out of scope (explicit — per user decisions)
- No header bar / no `N / TOTAL` position counter.
- Footer legend is **static**, not context-sensitive (no per-mode swapping).
- No confirm dialogs, no action pills (RUN/OPEN), no danger-red action styling.
- No change to readout focusability or any other navigation behavior.
- No per-row sub-notes (e.g. "DRIVEN BY DLINK").
- No setting-field additions/removals/renames. Inventory and keys unchanged.
- No true italic (the chosen 2-weight font has no italic; emphasis = amber + ExtraBold).

## Approach

**Re-skin in place + centralize the palette.** Modify `styles.c`/`styles.h` tokens and
the handful of hardcoded hex literals embedded in the widgets, plus the `menu.c`
layout. Lift the scattered hex colors (`0x4C60D8`, `0x6B7FFF`, etc.) into named tokens
in `styles.h` so the palette lives in one place — a small, in-scope improvement that
makes the reskin and any future theme change a single-file edit.

Rejected alternatives: a new `pp_theme` module (unnecessary churn/risk for a reskin);
rebuilding the layout to the handoff's exact 1920×1080 px (the goggle resolution
differs — a centered max-width panel is more robust than absolute insets).

## Design tokens

Add named tokens to `styles.h` (values from the handoff "Design Tokens" table),
define in `styles.c`, and replace inline hex throughout the widgets.

| Token | Value | Use |
|---|---|---|
| `PP_C_ACCENT` | `0xFFB300` | focus bar, active tab, editing values, ON toggle, markers |
| `PP_C_CRIT` | `0xFF2E3E` | toast (error), border |
| `PP_C_PANEL` | `0x0C0E12` | menu body fill (opaque) |
| `PP_C_RAIL` | `0x090B0E` | tab rail fill (opaque) |
| `PP_C_MODAL` | `0x0E1014` | select-modal / toast body fill |
| `PP_C_SCRIM` | `0x0A0B0E` @ opa ~194 | full-frame dim behind panel |
| `PP_C_INK` | `0xFFFFFF` | primary / focused text |
| `PP_C_LINE` | `0xFFFFFF` @ ~8–12% | separators / borders |

Dim text levels are expressed with `text_opa` (≈ `0.84` ink for idle labels, `0.62`
idle values, `0.40` section headers/units, `0.26` idle chevrons), matching today's
opacity-based approach.

### Typography (Barlow Condensed, 2 weights)

| Role | Weight | Notes |
|---|---|---|
| Row label, section header, tab-rail label, footer label | **Medium** | uppercase + tracked where the design specifies |
| Control value (stepper/select), editing emphasis, toast, footer key-cap | **ExtraBold** | editing value also recolored to `PP_C_ACCENT` |

- Load `BarlowCondensed-Medium.ttf` and `BarlowCondensed-ExtraBold.ttf` via the existing
  `lv_tiny_ttf` path (one instance per weight×size). Keep the `&lv_font_montserrat_*`
  fallback chained for `LV_SYMBOL_*` glyphs (tab/footer icons), exactly as Geist does today.
- Reuse the current three-size scheme (`pp_font_*_14/16/22` roles ≈ 22/24/32 px at the
  150 % `PP_SCALE`). Add per-weight accessors (e.g. `pp_font_barlow_med(role)` /
  `pp_font_barlow_xbold(role)`); exact px tuned in the simulator. Geist remains the
  fallback if a Barlow TTF is missing.

## Layout shell (`src/menu.c`, `pp_tabbar`, `pp_page`)

Restructure the menu screen so a centered panel floats over a dimmed video, and the
content side carries a fixed footer:

```
pp_menu_screen                       ← bg = PP_C_SCRIM @ ~76% (today: transparent)
└── panel                  [flex ROW, centered]   ← PP_C_PANEL opaque, radius 10, 1px PP_C_LINE border
    ├── rail (pp_tabbar root)        ← PP_C_RAIL opaque, fixed width
    └── content-col        [flex COLUMN]
        ├── pages-area      [flex-grow:1]          ← holds the 5 pages (only active visible)
        │   └── page (pp_page, scrolls)
        └── footer (pp_footer)       [fixed]       ← static legend
```

- **Scrim:** set `pp_menu_screen` bg color `PP_C_SCRIM`, `bg_opa ≈ 194`. The decoded
  video composites underneath (LVGL draws on top), so the frame shows through dimmed.
- **Panel:** centered (e.g. `lv_obj_center`), width capped to a max (~1240 px-equivalent,
  clamped to ≤ ~92 % screen), height = screen minus top/bottom insets (~90/70 px-equiv).
  Radius ~10, 1px `PP_C_LINE` border, opaque `PP_C_PANEL`. Replaces the current
  left-anchored `LV_PCT(45)` root.
- **pages-area:** the five pages move from being `root` siblings to children of
  `pages-area`. The tabbar's `apply_active` still toggles `LV_OBJ_FLAG_HIDDEN`; hidden
  flex items take no space, so the active page fills the area. Per-page vertical scroll
  is unchanged. The page-to-tabbar back-group wiring (`pp_page_set_back_group`) is unchanged.
- Disable the existing `PP_PANEL_FX` blur/shadow path for the panel (design is flat
  fills only; also removes the known RK3566 nav-slowness cost).

### Tab rail (`pp_tabbar.c` + `pp_style_tab*`)

- Root: opaque `PP_C_RAIL`; width bumped to the design's rail (≈188 px scaled, today 72).
- Active tab: **4px amber left bar** (radius `0 2 2 0`, inset top/bottom), icon → `PP_C_ACCENT`,
  label → `PP_C_INK`. Implement the bar as a left border on the tab in `LV_STATE_CHECKED`.
- Inactive tab: icon + label at `PP_C_INK` ~50 %.
- Rail-focused row: bg `PP_C_INK` ~7 %.
- Icon size bumped (~29 px design); label Medium ~15 px. Keep existing `LV_SYMBOL` tab glyphs.

### Footer legend (`pp_footer` — new widget)

A single static instance built once and placed in `content-col` (shared across tabs).

- Container: top border 1px `PP_C_LINE`, bg black ~18 %, horizontal padding ~30 / vertical ~14 (scaled), flex ROW.
- Three groups: `[▲▼] NAVIGATE` · `[▶] ENTER` · `[◀] BACK`.
- Key-cap chip: ~26 px scaled, 1.5px `PP_C_INK` ~30 % border, radius 5, ExtraBold; glyphs via
  `LV_SYMBOL_UP`/`DOWN`/`RIGHT`/`LEFT`.
- Labels: Medium, uppercase, tracked, `PP_C_INK` ~60 %.

## Rows & controls

### Rows (`pp_row.c`, `pp_dropdown.c`, `pp_toggle.c`, `pp_slider.c`, `pp_style_row*`)

- **Remove the per-row leading icon.** Drop the `lv_label_create(... icon_text ...)`
  block in each row-building widget (keep the `icon_text` parameter for API stability;
  simply stop rendering it). Pages are unchanged. Tab-rail icons are unaffected.
- Focused row: bg `PP_C_INK` ~7 % + **4px amber left bar** (already structured this way
  in `pp_style_row_focus`; recolor to `PP_C_ACCENT` and adjust the focus bg).
- Label font → Medium; value font → ExtraBold; idle/focus text opacities per tokens.

### Toggle (`pp_toggle.c`, `pp_style_switch_on`)

- ON: track `PP_C_ACCENT`, knob dark. OFF: track `PP_C_INK` ~16 %, knob `PP_C_INK` ~92 %.
- Style via `lv_switch` parts: `LV_PART_INDICATOR|CHECKED` (on-track), `LV_PART_MAIN`
  (off-track), `LV_PART_KNOB` (knob color). Sizing unchanged.

### Stepper (`pp_slider.c`)

- Recolor `set_edit_state` from `0x6B7FFF`→`PP_C_ACCENT` (edit) and white (idle).
- Editing value rendered ExtraBold + amber; chevrons dim (`~26 %`) idle → bright/amber on edit.
- Behavior (NAV/EDIT, hold-to-accelerate, async apply) unchanged.

### Select picker → centered modal (`pp_dropdown.c`)

Rebuild `popup_open` (and `popup_refresh`/positioning) as a centered modal; keep the
existing key handling (UP/DOWN move highlight, ENTER apply async, ESC cancel) intact.

- Optional full-screen dim behind the modal (`rgba(6,7,9,0.55)`); modal box centered.
- Box: ~440 px scaled wide, max-height clamped, bg `PP_C_MODAL`, **1px `PP_C_ACCENT` border**, radius 12.
- Header: amber marker + uppercase label (store the row's `label` in `pp_dd_data` for the title).
- Option row: highlighted = `PP_C_ACCENT` ~10 % bg + 4px amber left bar + amber ExtraBold value;
  others ink-dim. The currently-saved option shows a dim **"CURRENT"** tag on the right.

### Section header (`pp_section_header.c` / `pp_style_section_hdr`)

Already uppercase, tracked, dim. Add the small left tick (≈16×3 px dim bar) before the
title to match the design. Recolor/retrack per tokens.

### Toast (`pp_toast.c`)

Restyle the existing error toast: bg `PP_C_MODAL` ~94 %, 1px `PP_C_CRIT` border, radius 8,
a small dot + uppercase ExtraBold message. Remains error-only (no new success toasts).

### Drill-down (`pp_drilldown.c`)

Minor: ensure it picks up the new panel tokens (bg/border/font) so sub-pages match.
No structural change.

## Font sourcing & deploy

- Add `BarlowCondensed-Medium.ttf` and `BarlowCondensed-ExtraBold.ttf` (OFL, Google Fonts)
  to `src/gsmenu/fonts/`. Record the OFL license alongside.
- Extend the CMake font `install(FILES …)` rule (currently installs `Geist-Regular.ttf`
  to `${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/fonts`) to include both Barlow TTFs,
  so the GS deploy ships them. The `load_*` search prefixes in `styles.c` already cover
  `/usr/share/pixelpilot/fonts` and `./src/gsmenu/fonts`.

## Verification

- Build the SDL2 simulator: `cmake -DUSE_SIMULATOR=ON -S . -B build_sim && cmake --build build_sim`
  (`src/simulator.c` boots straight into `pp_menu_main()`).
- Capture screenshots: each of the five tabs, the select modal (e.g. Camera ▸ Size),
  a stepper mid-edit, a toggle, the footer legend, and the scrim/centered-panel framing.
  Compare against the reference screenshots and `design_handoff_pixelpilot_menu/`.
- Confirm 4-button navigation still works end-to-end (tab switch, row nav, edit/apply/cancel,
  select modal, back-to-rail) — behavior must be unchanged.
- Optional: deploy to the GS hardware after sim sign-off (per the GS deploy workflow).

## File-by-file change summary

| File | Change |
|---|---|
| `src/gsmenu/styles.h` | Add `PP_C_*` palette tokens + Barlow font accessor decls. |
| `src/gsmenu/styles.c` | Amber/opaque tokens; load Barlow Medium+ExtraBold; disable panel FX; tick/section/row/switch styles. |
| `src/menu.c` | Scrim bg; centered panel (radius/border); `content-col` with `pages-area` + footer; reparent pages. |
| `src/gsmenu/widgets/pp_tabbar.c` | Opaque rail, wider rail, amber active-bar, amber/dim icon+label. |
| `src/gsmenu/widgets/pp_footer.{c,h}` | **New** static legend widget. |
| `src/gsmenu/widgets/pp_row.c` | Drop leading icon render; recolor focus; fonts. |
| `src/gsmenu/widgets/pp_dropdown.c` | Drop leading icon; rebuild popup as centered modal (header + CURRENT tag); store label. |
| `src/gsmenu/widgets/pp_toggle.c` | Drop leading icon; amber/white switch part colors. |
| `src/gsmenu/widgets/pp_slider.c` | Drop leading icon; amber edit colors; ExtraBold value. |
| `src/gsmenu/widgets/pp_section_header.c` | Add left tick; token recolor. |
| `src/gsmenu/widgets/pp_toast.c` | Dark-panel restyle (crit border + dot + uppercase). |
| `src/gsmenu/widgets/pp_drilldown.c` | Token pickup (bg/border/font). |
| `src/gsmenu/fonts/` | Add two Barlow Condensed TTFs (+ OFL license). |
| `CMakeLists.txt` | Install both Barlow TTFs with the existing font rule. |

## Risks / notes

- **Font memory/load on RK3566:** two weights × a few sizes = a handful of `lv_tiny_ttf`
  instances. Expected modest; confirm in the sim and, if heavy, trim sizes. (Removing the
  panel blur/shadow FX more than offsets this in render cost.)
- **Reparenting pages** into `pages-area` must preserve the tabbar↔page references and the
  back-group wiring; verify tab switching and HOME-to-rail still work.
- **Centered-panel sizing** is resolution-dependent; use caps/percentages and tune insets
  in the sim rather than hardcoding 1920×1080 px.
- Glyphs for tab/footer icons rely on the Montserrat fallback chain — verify it survives
  the font swap (same mechanism Geist uses today).
