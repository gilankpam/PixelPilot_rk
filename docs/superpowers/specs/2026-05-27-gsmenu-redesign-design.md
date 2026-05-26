# GSMenu Redesign — DJI Goggles-Inspired UI

**Status:** Design
**Date:** 2026-05-27

## Summary

Redesign the PixelPilot ground-station menu (GSMenu) for cleaner, simpler navigation modeled on the DJI Goggles OSD. Flatten the current Air/Ground hierarchy into five functional tabs, swap LVGL's `lv_menu` for a small set of custom widgets, and put a settings-provider abstraction between the UI and the persistence layer. The current `gsmenu.sh` shell-out backend is **dropped** as part of this work; only a stub provider exists initially, leaving a clean seam for a real backend to be plugged in later.

## Goals

- Cut the navigation tree's depth: 5 flat top-level categories, no Air/Ground split at the top.
- Adopt a DJI-style layout: vertical icon tab strip on the left, scrolling content pane on the right.
- Replace text "On"/"Off" with real toggle switches.
- Cleaner visual style: opaque dark panel, brightened OpenIPC-blue accent, subtle dividers, generous spacing.
- Make the settings persistence layer pluggable so the backend can be swapped without touching widgets.

## Non-goals

- **Settings persistence**: no real backend in this work. Stub provider only.
- **Cairo OSD telemetry overlay**: not touched. `config_osd.json` widgets keep rendering as today.
- **Input layer**: GPIO debouncing, virtual keyboard, `control_mode` FSM — unchanged.
- **Video pipeline**: MPP decoder, DRM compositing, color correction — unchanged.
- **Simulator harness**: `simulator.c`, `shell-sim.nix`, the LVGL patch — kept as-is.

## Decisions captured in brainstorming

| Decision | Choice |
|---|---|
| Scope | Visual + nav restructure (not a full overhaul of input model) |
| Layout | Left vertical tab strip + right content pane |
| Top-level categories | Flat, 5 functional tabs |
| Visual style | Opaque dark panel + brightened OpenIPC blue + subtle dividers |
| Camera sub-pages | Inline sections in one long scroll (no drill-down) |
| Booleans | Toggle switch widget |
| Implementation | Drop `lv_menu`, build custom tabview widgets |
| Settings backend | Drop `gsmenu.sh`; keep provider abstraction; ship with stub provider only |

## Architecture

Two layers compose into the new UI:

1. **`pp_settings` provider interface** — narrow, stateless C interface between widgets and persistence. Widgets call `pp_settings_set(...)` / `pp_settings_get(...)`; the provider is registered once at startup. Initial provider is a no-op stub.
2. **Custom LVGL widget set** under `src/gsmenu/widgets/` — left tab bar, scrollable pages, focusable rows of various control types, and an overlay drilldown for dynamic lists.

The existing input layer (`dispatch_input_char` → `control_mode` → virtual keyboard indev → LVGL groups) is unchanged. The widget set wires up focus groups; `control_mode` swaps W/A/S/D meaning between NAV/EDIT/SLIDER as today.

### Components and their boundaries

```
                          ┌─────────────────────┐
   stdin / SDL keys ─────▶│  input dispatch     │ (unchanged)
                          │  (control_mode FSM) │
                          └──────────┬──────────┘
                                     │ next_key
                                     ▼
                          ┌─────────────────────┐
                          │  virtual_keyboard   │ (unchanged)
                          │  LVGL indev         │
                          └──────────┬──────────┘
                                     │ LV_KEY_*
                                     ▼
   ┌────────────────────────────────────────────────────────┐
   │  pp_tabbar  │  pp_page (×5)                            │
   │             │   ├── pp_section_header                  │
   │             │   ├── pp_row / pp_toggle /               │
   │             │   │     pp_slider / pp_dropdown          │
   │             │   └── …                                  │
   │             │  pp_drilldown (overlay; opened on demand)│
   └─────────────┴──────────┬───────────────────────────────┘
                            │ pp_settings_set / get / set_async
                            ▼
                  ┌──────────────────────┐
                  │  pp_settings (api)   │
                  └──────────┬───────────┘
                             │
                             ▼
                  ┌──────────────────────┐
                  │  settings_stub       │  ← only initial provider
                  │  (LV_LOG_USER print) │
                  └──────────────────────┘
```

## Settings provider abstraction

A small C interface that widgets call instead of constructing shell commands themselves.

```c
// src/gsmenu/settings.h
typedef struct {
    void  (*set)(const char *domain, const char *page,
                 const char *key, const char *value);
    char *(*get)(const char *domain, const char *page,
                 const char *key);                          // caller frees
    void  (*set_async)(const char *domain, const char *page,
                       const char *key, const char *value,
                       void (*on_done)(int rc, const char *stderr_out));
} pp_settings_provider_t;

void pp_settings_register(const pp_settings_provider_t *provider);

void  pp_settings_set(const char *domain, const char *page,
                      const char *key, const char *value);
char *pp_settings_get(const char *domain, const char *page,
                      const char *key);
void  pp_settings_set_async(const char *domain, const char *page,
                            const char *key, const char *value,
                            void (*on_done)(int rc, const char *err));
```

**Initial provider — stub (the only one shipped in this work):**

```c
// src/gsmenu/settings_stub.c
static void stub_set(const char *d, const char *p, const char *k, const char *v) {
    LV_LOG_USER("settings.set %s/%s/%s = %s", d, p, k, v);
}
static char *stub_get(const char *d, const char *p, const char *k) {
    return strdup("");          // empty; widgets render with their placeholder
}
static void stub_set_async(const char *d, const char *p, const char *k,
                           const char *v, void (*on_done)(int, const char*)) {
    LV_LOG_USER("settings.set_async %s/%s/%s = %s", d, p, k, v);
    if (on_done) on_done(0, NULL);   // succeed immediately
}
```

Registered once at startup in `src/main.cpp` (real hardware) and `src/simulator.c` (sim). Identical behavior in both — the redesign is a frontend-only effort.

**`set_async` is retained in the interface** even though the stub is synchronous. Future providers may be slow (round-trip to air unit, file write); widgets call `set_async` where blocking is unacceptable, and the provider decides whether to spawn a worker.

**Error and progress reporting** become provider responsibilities. The stub never errors. A future provider can raise an error event on a shared bus (subscribed to once by a single error-toast widget). The current scattered `result.exit_status > 0` → `show_error()` calls disappear.

## Widget set

All widgets live under `src/gsmenu/widgets/` — one `.c`/`.h` pair each, focused on one job.

### `pp_tabbar`

```c
typedef struct pp_tabbar pp_tabbar_t;

typedef struct {
    const char *label;        // "Camera"
    const void *icon_src;     // LVGL image or symbol
    lv_obj_t   *page;         // pp_page* to show when this tab is active
} pp_tabbar_item_t;

pp_tabbar_t *pp_tabbar_create(lv_obj_t *parent,
                              const pp_tabbar_item_t *items, size_t n);
void         pp_tabbar_set_active(pp_tabbar_t *t, size_t index);
lv_group_t  *pp_tabbar_group(pp_tabbar_t *t);
```

Owns 5 tab buttons in a vertical strip. Emits `LV_EVENT_VALUE_CHANGED` when active tab changes — payload is the new index. Page swapping (hide all, show selected) is the tabbar's responsibility, not the caller's.

### `pp_page`

```c
lv_obj_t   *pp_page_create(lv_obj_t *parent,
                           const char *domain,   // "air" / "gs"
                           const char *page);    // "camera", "link", ...
lv_group_t *pp_page_group(lv_obj_t *page);       // rows in this page
```

Scrollable container. The `(domain, page)` tuple is stored as user_data and inherited by every row added to this page so rows don't repeat it.

### `pp_section_header`

```c
lv_obj_t *pp_section_header(lv_obj_t *page, const char *text);
```

Non-focusable label introducing a group of rows (`Video`, `Image`, `ISP`, …). Small uppercase tracked text.

### `pp_row` (text/value display)

```c
lv_obj_t *pp_row_text(lv_obj_t *page,
                      const void *icon,
                      const char *label,
                      const char *key);
```

Reads its value via `pp_settings_get(domain, page, key)` on page-load. Non-editable (e.g., `Version`, `Disk`).

### `pp_toggle`

```c
lv_obj_t *pp_toggle(lv_obj_t *page,
                    const void *icon,
                    const char *label,
                    const char *key);
```

Right side renders an `lv_switch` styled with the accent color. Focus → D → toggles state + `pp_settings_set_async(...)`. ESC reverts the visible state to the last-known.

### `pp_slider`, `pp_dropdown`

Same shape: `(page, icon, label, key, …type-specific params…)`. Each owns its LVGL control and calls `pp_settings_set_async` on commit. `control_mode` flips to SLIDER / EDIT while focused, exactly as today.

### `pp_drilldown`

```c
lv_obj_t *pp_drilldown_open(lv_obj_t *anchor_page,
                            const char *title,
                            void (*build_body)(lv_obj_t *body));
void      pp_drilldown_close(void);
```

Slide-in overlay on the right side, used only for cases where the row count is dynamic: TX Profiles, WiFi networks, DVR Playback. A goes back to the underlying page. While open, the page beneath is dimmed but visible.

### Why this granularity

Every widget takes `(page, icon, label, key, …)` — no caller ever:
- Repeats `domain`/`page` strings.
- Constructs settings commands.
- Touches LVGL groups directly.

Building a tab becomes a flat list of `pp_*(page, …)` calls.

## Page structure

Each tab is one scrolling page composed of section headers + rows. Exact settings come verbatim from today's per-page builders (`air_camera_*`, `gs_system_*`, etc.); only the *grouping* changes.

### Camera tab (domain: air)

```
[Video]      Resolution · Bitrate · Codec · GOP · FPS
[Image]      Mirror · Flip · Brightness · Contrast · Saturation
[ISP]        Profile · WDR · Anti-flicker · …
[FPV]        Mode · Noise · Roll · …
[Recording]  Enable · Resolution · Bitrate · FPS
```

Built from today's five `create_air_camera_*_menu` functions, merged in this order into a single page with section headers between groups.

### Link tab (mixed domain)

```
[WFB-NG]      Channel · MCS · Bandwidth · GI · TX Power     (shared air+gs)
[ALink]       Enable · Profile · …
[AALink]      Enable · Aggression · …
[AP-FPV]      Mode · SSID · Password
[TX Profiles] → drilldown to profile list (add / edit / remove)
```

### Display tab (domain: gs)

```
[Output]     HDMI Mode · Video Scale
[Color]      Color correction enable · Gain · Offset · Reset to default
```

The Cairo OSD telemetry overlay is **not** configured here — `config_osd.json` is unchanged.

### DVR tab (domain: gs)

```
[Recording]  Enable · Mode · FPS · Bitrate · Codec · Resolution · Max size
[Overlay]    Burn-in OSD into recording
[Playback]   → drilldown to file list (play / delete)
```

### System tab (mixed domain)

```
[Info]       Version · Disk · Receiver mode · RXMODE     (read-only)
[Network]    WiFi → drilldown to network list · AP mode · Restream
[Telemetry]  MAVLink enable · Baud · UART · Forward to GS
[Actions]    Reboot air · Reboot GS · Factory reset air · Factory reset GS
```

The current root-menu items (`Channel`, `HDMI-OUT`, `WFB_NICS`, `Version`, `Disk`) move into the **Info** section here — no separate "home" screen.

### Notes

- **Domain awareness is per-row, not per-tab.** Each row's `pp_settings_set(domain, page, key, value)` uses its own domain. The tab label never exposes Air vs Ground — DJI doesn't either.
- **Drilldowns are minimal**: TX Profiles, WiFi networks, DVR Playback. Everything else is inline.

## Navigation model

Three focus contexts, swapped by changing the indev's group:

| Context | Group | Active when |
|---|---|---|
| **Tabbar** | `pp_tabbar_group(tabbar)` | Default when menu opens; after A from a page |
| **Page rows** | `pp_page_group(active_page)` | After D on tabbar |
| **Drilldown** | drilldown's internal group | While overlay is open |

### Key mapping (`control_mode = NAV`)

| Key | In Tabbar | In Page | In Drilldown |
|---|---|---|---|
| **W** | move up in tab strip | move up in rows | move up |
| **S** | move down in tab strip | move down in rows | move down |
| **A** | close menu (return to OSD screen) | focus back to tabbar | close drilldown |
| **D** | enter page (focus first row) | activate row (toggle / enter edit mode / open drilldown) | activate item |

Tab activation is **lazy**: moving W/S on the tab strip updates the right pane immediately — page swap on focus change, not on D. (This is how DJI feels.)

Edit / slider mode is unchanged from today. When a row activates `pp_dropdown` or `pp_slider`, `control_mode` flips to EDIT or SLIDER, and W/A/S/D mean adjust/cancel/adjust/confirm per the existing FSM.

## Visual spec

### Colors

| Token | Hex | Use |
|---|---|---|
| `bg_panel` | `#0F1116` (0.97a) | Main panel background |
| `bg_tabbar` | `#000000` (0.30a) | Tab strip background |
| `divider` | `#FFFFFF` (0.05a) | Tabbar right border, row bottom borders |
| `text_primary` | `#FFFFFF` | Active labels, focused row label |
| `text_muted` | `#FFFFFF` (0.55a) | Values, inactive labels |
| `text_section` | `#FFFFFF` (0.40a) | Section headers (uppercase) |
| `accent` | `#6B7FFF` | Focus border, toggle on-state, active tab text/glyph, value-on-focus |
| `accent_bg` | `#4C60D8` (0.12a) | Focused row background, active tab background |
| `error` | `#FF5C5C` | Reserved for future error toasts (unused by stub) |

### Typography

- Family: LVGL default sans (Montserrat). No change.
- Sizes: row label/value 16px · tab label 12px · section header 11px tracked +1.2px uppercase · page title 14px.

### Dimensions

| Element | Size |
|---|---|
| Panel | 78% width × 100% height, anchored left |
| Tabbar | 72px wide |
| Tab item | 56px tall (icon 20px + label below) |
| Row | 36px tall, 20px horizontal padding |
| Row icon column | 14px wide + 12px gap |
| Section header | 22px tall, 8px top padding |
| Focus left border | 2px wide; row inner padding decreases by 2px to compensate |
| Section divider | 1px (`divider` token) |
| Page scroll bar | hidden by default; 2px wide accent indicator when scrolling |

### Behaviors

- **Tab change**: 120ms cross-fade between pages (LVGL anim).
- **Drilldown open**: 180ms slide-in from right.
- **Focus move**: no animation (instant) — keeps the menu responsive on small FPV monitors.

Nothing else animates. No glow, no blur (we picked the opaque style explicitly).

## File-level migration

### New

```
src/gsmenu/settings.h
src/gsmenu/settings.c                 — pp_settings_register/set/get/set_async dispatch
src/gsmenu/settings_stub.c            — only initial provider

src/gsmenu/widgets/pp_tabbar.{h,c}
src/gsmenu/widgets/pp_page.{h,c}
src/gsmenu/widgets/pp_section_header.{h,c}
src/gsmenu/widgets/pp_row.{h,c}
src/gsmenu/widgets/pp_toggle.{h,c}
src/gsmenu/widgets/pp_slider.{h,c}
src/gsmenu/widgets/pp_dropdown.{h,c}
src/gsmenu/widgets/pp_drilldown.{h,c}

src/gsmenu/pages/camera.c             — build_camera_tab()
src/gsmenu/pages/link.c               — build_link_tab()
src/gsmenu/pages/display.c            — build_display_tab()
src/gsmenu/pages/dvr.c                — build_dvr_tab()
src/gsmenu/pages/system.c             — build_system_tab()

src/gsmenu/icons/                     — new compact icon set for tab bar + rows
```

### Rewritten

```
src/menu.c                            — pp_menu_main() spins up pp_tabbar + 5 pp_page; no lv_menu
src/gsmenu/ui.c                       — slimmed: globals + screen-toggle glue only
src/gsmenu/helper.c                   — keep reload helpers and find_resource_file
src/gsmenu/styles.{c,h}               — replaced using tokens from the Visual spec
src/input.cpp                         — no logic change; verify group switching points still apply
src/main.cpp / src/simulator.c        — register stub settings provider at startup
CMakeLists.txt                        — adjust SIMULATOR_SOURCES and LIB_SOURCE_FILES file lists
```

### Deleted

```
src/gsmenu/executor.{c,h}             — entire shell-out + spinner + msgbox layer
src/gsmenu/air_aalink.{c,h}
src/gsmenu/air_actions.{c,h}
src/gsmenu/air_alink.{c,h}
src/gsmenu/air_camera.{c,h}
src/gsmenu/air_telemetry.{c,h}
src/gsmenu/air_txprofiles.{c,h}       — replaced by drilldown in pages/link.c
src/gsmenu/air_wfbng.{c,h}
src/gsmenu/gs_actions.{c,h}
src/gsmenu/gs_apfpv.{c,h}
src/gsmenu/gs_connection_checker.{c,h} — only used by removed pages
src/gsmenu/gs_dvr.{c,h}
src/gsmenu/gs_dvrplayer.{c,h}         — replaced by drilldown in pages/dvr.c
src/gsmenu/gs_main.{c,h}              — absorbed into pages/system.c
src/gsmenu/gs_system.{c,h}            — split across pages/display.c, pages/dvr.c, pages/system.c
src/gsmenu/gs_wfbng.{c,h}
src/gsmenu/gs_wifi.{c,h}              — replaced by drilldown in pages/system.c
```

## Suggested order of work

The implementation plan (separate document) will expand this. Sketch:

1. **Settings provider scaffolding** (`settings.{h,c}` + `settings_stub.c`) and delete `executor.c`. Build still passes because no widgets call the provider yet — the existing menu continues working against the old `lv_menu` builders for the duration of steps 2–4.
2. **Widget primitives in isolation** — build each widget; drive it from a throwaway test page in the simulator before any real tab uses it.
3. **One real tab end-to-end** — Display (smallest). Proves the tabbar + page + row + toggle path.
4. **Remaining four tabs** — repeat the pattern; delete the corresponding `air_*`/`gs_*` files as each is replaced.
5. **Remove `lv_menu` usage** — once nothing references it; rewrite `pp_menu_main`.
6. **Polish pass** — animations, scrollbar, focus visuals, icon polish.

Each step leaves the simulator in a runnable state.

## Testing

- **Simulator** is the primary validation surface. Every tab must be navigable end-to-end with W/A/S/D + arrows in the SDL window.
- **Smoke checklist** maintained alongside the design: open menu → traverse all 5 tabs → enter each section → toggle a switch → change a slider → open + close drilldown → close menu. Manual; ~2 minutes.
- **No widget unit tests** in this work. LVGL widgets are hard to test without rendering and the project has no LVGL test infra today.
- The settings provider interface stays unit-testable later by registering a recording provider in a test main.

## Risks

- **`lv_menu` removal touches every page builder.** The migration plan isolates this by porting one tab at a time; the old menu coexists until the last tab moves.
- **Focus group bugs are easy to introduce.** The new tabbar / page / drilldown each own their own group; switching them at the right moments matters. Manual smoke check on every step.
- **Stub provider means visible values are empty.** Rows render with their static label and placeholder value (e.g., `—`). Acceptable for this work since persistence is out of scope; obvious to anyone running it that no backend is wired.

## Open questions

None blocking implementation. Things to revisit after this lands:

- Real settings provider design (next effort). The interface here is intentionally minimal; the real provider will likely need batched reads and change subscriptions.
- Whether to bring the OSD telemetry overlay's configuration into this menu eventually.
- Icon set: this work uses LVGL symbols + simple bordered glyphs; a polished icon font/SVG set is a possible follow-up.
