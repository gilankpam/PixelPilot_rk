# PixelPilot GS Menu — OSD Reskin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-skin the existing PixelPilot ground-station config menu into the racing-HUD "OSD" look — amber accent, Barlow Condensed type, opaque flat panels over a dimmed full-frame video scrim, centered floating panel, and a static footer key-legend — with no change to fields, navigation, or control behavior.

**Architecture:** The menu (`src/gsmenu/`) is already fully functional. This plan changes only tokens, fonts, layout shell, and per-widget styling. Verification is by **building the SDL2 simulator and capturing PNG screenshots** of each state (there is no unit-testable logic here — the interaction code is unchanged). Task 1 builds a headless screenshot harness used by every later task.

**Tech Stack:** C, LVGL v9.5 (`lv_style_*`, flex, `lv_tiny_ttf`, `lv_snapshot`), SDL2 simulator, libpng, CMake, Nix (`shell-sim.nix`).

**Spec:** `docs/superpowers/specs/2026-06-07-pixelpilot-menu-osd-reskin-design.md`

---

## Conventions (read once, applies to every task)

**Build command** (run from repo root; `build-sim/` already exists):

```bash
nix-shell shell-sim.nix --run "cmake -DUSE_SIMULATOR=ON -S . -B build-sim && cmake --build build-sim -j" 2>&1 | tail -20
```
Expected on success: ends with `[100%] Built target pixelpilot` (or `Built target pixelpilot`) and no errors.

**Screenshot command** (headless; available after Task 1). `PP_SIM_KEYS` scripts key presses (`d`=enter tab/content, `w`/`s`=up/down, `\n` or `Enter` char `\n`, `a`=back); the harness opens the menu, replays the keys, then writes a PNG and exits:

```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/shot.png PP_SIM_KEYS='' ./build-sim/pixelpilot"
```
Then inspect `/tmp/shot.png` (Read tool) and/or send to the user (SendUserFile). Different states are reached with different `PP_SIM_KEYS` (each task says which).

**Scaling note (important):** Do **not** chase the handoff's exact 1920-canvas pixel values. The simulator runs at 1920×1080 but the device may differ, so size the **panel and rail proportionally** (percent + max-width cap) and keep the existing `PP_SCALE`-based row/padding metrics (they already render at a reasonable size). Pick font sizes that look right in the sim — pixel-identical to the handoff is explicitly not required.

**Commit cadence:** one commit per task. End every commit message body with:
```
Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

**Palette reference** (added in Task 2, used everywhere after):
`PP_C_ACCENT 0xFFB300` · `PP_C_CRIT 0xFF2E3E` · `PP_C_PANEL 0x0C0E12` · `PP_C_RAIL 0x090B0E` · `PP_C_MODAL 0x0E1014` · `PP_C_SCRIM 0x0A0B0E` · `PP_C_INK 0xFFFFFF`.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/simulator.c` | + headless screenshot harness | 1 |
| `lv_conf.h` | enable `LV_USE_SNAPSHOT` | 1 |
| `src/gsmenu/styles.h` | + `PP_C_*` palette macros, Barlow font accessor decls | 2,3 |
| `src/gsmenu/styles.c` | amber/opaque tokens; load Barlow Medium+ExtraBold | 2,3 |
| `src/menu.c` | scrim child, centered panel, content-col + pages-area, footer wire | 4,6 |
| `src/gsmenu/widgets/pp_tabbar.c` | opaque/wider rail, amber active-bar, amber/dim icon+label | 5 |
| `src/gsmenu/widgets/pp_footer.{c,h}` | **new** static legend widget | 6 |
| `src/gsmenu/widgets/pp_row.c` | drop leading icon; recolor focus | 7 |
| `src/gsmenu/widgets/pp_dropdown.c` | drop icon; centered modal (header+CURRENT) | 7,11 |
| `src/gsmenu/widgets/pp_toggle.c` | drop icon; amber/white switch colors | 7,8 |
| `src/gsmenu/widgets/pp_slider.c` | drop icon; amber edit + ExtraBold value | 7,9 |
| `src/gsmenu/widgets/pp_section_header.c` | left tick + token recolor | 10 |
| `src/gsmenu/widgets/pp_toast.c` | dark-panel restyle | 12 |
| `src/gsmenu/widgets/pp_drilldown.c` | token pickup | 13 |
| `src/gsmenu/fonts/` | + two Barlow Condensed TTFs (+ OFL) | 3 |
| `CMakeLists.txt` | install Barlow TTFs; add `pp_footer.c` to sim sources | 3,6 |

---

## Task 1: Headless screenshot harness (tooling)

**Files:**
- Modify: `lv_conf.h` (the `LV_USE_SNAPSHOT` line)
- Modify: `src/simulator.c` (add PNG writer + harness in `main`)

- [ ] **Step 1: Enable snapshot in lv_conf.h**

Find `#define LV_USE_SNAPSHOT 0` (≈ line 1108) and set it to `1`:
```c
#define LV_USE_SNAPSHOT 1
```

- [ ] **Step 2: Add a libpng writer + screenshot harness to simulator.c**

At the top of `src/simulator.c`, after the existing `#include` block (after `#include "gsmenu/settings.h"`), add:
```c
#include <png.h>
#include <stdlib.h>

/* from input.cpp */
extern void toggle_screen(void);

static int sim_write_png(const char *path, const uint8_t *argb,
                         uint32_t w, uint32_t h, uint32_t stride) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); fclose(fp); return -1; }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    uint8_t *row = (uint8_t *)malloc((size_t)w * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = argb + (size_t)y * stride;   /* ARGB8888 LE bytes: B,G,R,A */
        for (uint32_t x = 0; x < w; x++) {
            row[x*4+0] = src[x*4+2];  /* R */
            row[x*4+1] = src[x*4+1];  /* G */
            row[x*4+2] = src[x*4+0];  /* B */
            row[x*4+3] = src[x*4+3];  /* A */
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

static void sim_settle(int frames) {
    for (int i = 0; i < frames; i++) { lv_task_handler(); usleep(8000); }
}
```

In `main`, replace the final menu block:
```c
    pp_menu_main();
    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }
```
with:
```c
    pp_menu_main();

    const char *shot = getenv("PP_SIM_SHOT");
    if (shot) {
        extern lv_obj_t *pp_menu_screen;
        sim_settle(20);
        toggle_screen();                 /* loads pp_menu_screen + sets the group */
        sim_settle(20);
        const char *keys = getenv("PP_SIM_KEYS");
        if (keys) {
            for (const char *p = keys; *p; ++p) {
                if (*p == ' ') continue;
                dispatch_input_char(*p);
                sim_settle(8);
            }
        }
        /* Insert the placeholder video frame as the bottom child so the
         * scrim composites over it in the snapshot (device shows real video
         * through the scrim's transparency). Sim-only, for the screenshot. */
        lv_obj_t *bg = lv_image_create(pp_menu_screen);
        lv_image_set_src(bg, find_resource_file("osd-bg-2.png"));
        lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_move_to_index(bg, 0);
        sim_settle(20);

        lv_obj_t *scr = lv_screen_active();
        lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_ARGB8888);
        if (snap) {
            sim_write_png(shot, snap->data, snap->header.w, snap->header.h, snap->header.stride);
            lv_draw_buf_destroy(snap);
        }
        return 0;
    }

    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }
```

- [ ] **Step 3: Build**

Run the **Build command**. Expected: `Built target pixelpilot`, no errors.

- [ ] **Step 4: Capture a baseline screenshot (current blue/Geist look)**

```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/menu_before.png ./build-sim/pixelpilot"
```
Expected: command exits 0; `/tmp/menu_before.png` exists and is a 1920×1080 PNG showing the current menu over the forest background. Read it to confirm it renders (this is the "before" reference).

- [ ] **Step 5: Commit**

```bash
git add lv_conf.h src/simulator.c
git commit -m "test(sim): headless PNG screenshot harness (PP_SIM_SHOT/PP_SIM_KEYS)"
```

---

## Task 2: Centralized amber/opaque palette tokens

**Files:**
- Modify: `src/gsmenu/styles.h` (add macros)
- Modify: `src/gsmenu/styles.c` (recolor existing `pp_style_*`)

- [ ] **Step 1: Add palette macros to styles.h**

In `src/gsmenu/styles.h`, after the `PP_SCALE` macro block (after line 14), add:
```c
/* OSD reskin palette (see 2026-06-07 spec). lv_color_hex(PP_C_*). */
#define PP_C_ACCENT  0xFFB300   /* amber: focus bar, active tab, edit values, ON toggle */
#define PP_C_CRIT    0xFF2E3E   /* toast/error border */
#define PP_C_PANEL   0x0C0E12   /* opaque menu body */
#define PP_C_RAIL    0x090B0E   /* opaque tab rail */
#define PP_C_MODAL   0x0E1014   /* select modal / toast body */
#define PP_C_SCRIM   0x0A0B0E   /* full-frame dim behind panel */
#define PP_C_INK     0xFFFFFF   /* primary/focused text */
#define PP_OPA_SCRIM 194        /* ~76% */
```

- [ ] **Step 2: Recolor the token block in styles.c**

In `src/gsmenu/styles.c`, replace the `/* Color tokens */` block (currently lines 139–144):
```c
    /* Color tokens */
    const lv_color_t c_panel   = lv_color_hex(0x0F1116);
    const lv_color_t c_tabbar  = lv_color_hex(0x000000);
    const lv_color_t c_text    = lv_color_hex(0xFFFFFF);
    const lv_color_t c_accent  = lv_color_hex(0x6B7FFF);
    const lv_color_t c_accentd = lv_color_hex(0x4C60D8);
```
with:
```c
    /* Color tokens — OSD reskin (amber/opaque). */
    const lv_color_t c_panel   = lv_color_hex(PP_C_PANEL);
    const lv_color_t c_tabbar  = lv_color_hex(PP_C_RAIL);
    const lv_color_t c_text    = lv_color_hex(PP_C_INK);
    const lv_color_t c_accent  = lv_color_hex(PP_C_ACCENT);
    const lv_color_t c_accentd = lv_color_hex(PP_C_ACCENT);
```

- [ ] **Step 3: Make the panel opaque and drop the blur path**

In `src/gsmenu/styles.c`, in the `pp_style_panel` block, change `bg_opa` from `LV_OPA_70` to cover, and force the FX path off (flat fills only per design). Replace:
```c
    lv_style_set_bg_opa(&pp_style_panel, LV_OPA_70); /* lets more video bleed through; backdrop blur active */
```
with:
```c
    lv_style_set_bg_opa(&pp_style_panel, LV_OPA_COVER); /* opaque flat fill (design: no blur) */
```
And change the FX opt-in guard `if (getenv("PP_PANEL_FX")) {` to `if (0 && getenv("PP_PANEL_FX")) {` so the blur/shadow never apply (design is flat). Leave the body as-is.

- [ ] **Step 4: Make the tab rail opaque**

In the `pp_style_tabbar` block, change `lv_style_set_bg_opa(&pp_style_tabbar, 77);` to:
```c
    lv_style_set_bg_opa(&pp_style_tabbar, LV_OPA_COVER);
```

- [ ] **Step 5: Build + screenshot**

Run the **Build command** (expect `Built target pixelpilot`), then:
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t2.png ./build-sim/pixelpilot"
```
Read `/tmp/t2.png`. Expected: panel and rail are now opaque dark; the focused row's left bar and the active tab tint are **amber**, not blue.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/styles.h src/gsmenu/styles.c
git commit -m "feat(gsmenu): amber/opaque palette tokens"
```

---

## Task 3: Barlow Condensed font (Medium + ExtraBold)

**Files:**
- Create: `src/gsmenu/fonts/BarlowCondensed-Medium.ttf`, `BarlowCondensed-ExtraBold.ttf`, `OFL.txt`
- Modify: `src/gsmenu/styles.c` (loaders + accessors), `src/gsmenu/styles.h` (decls), `src/gsmenu/widgets/pp_dropdown.c` (accessor rename), `CMakeLists.txt` (install)

- [ ] **Step 1: Fetch the OFL TTFs**

```bash
cd src/gsmenu/fonts
for f in BarlowCondensed-Medium.ttf BarlowCondensed-ExtraBold.ttf OFL.txt; do
  curl -fsSL "https://raw.githubusercontent.com/google/fonts/main/ofl/barlowcondensed/$f" -o "$f"
done
cd -
ls -l src/gsmenu/fonts/BarlowCondensed-*.ttf
```
Expected: two non-empty `.ttf` files (each ~100–200 KB) + `OFL.txt`. If the network is blocked, stop and ask the user to drop the two TTFs into `src/gsmenu/fonts/` (they are free/OFL from Google Fonts), then continue.

- [ ] **Step 2: Replace the font loaders + accessors in styles.c**

In `src/gsmenu/styles.c`, replace the static font globals + `load_geist` + the three `pp_font_geist_*` accessors (lines 11–41) with Barlow Medium + ExtraBold loaders:
```c
/* Barlow Condensed instances (lv_tiny_ttf). NULL if a TTF is missing —
 * accessors then fall back to Montserrat. Two weights: Medium for labels/
 * sections/rail/footer, ExtraBold for control values / emphasis / toast. */
static lv_font_t *g_med_sm  = NULL;  /* section/rail/footer label */
static lv_font_t *g_med_md  = NULL;  /* row label */
static lv_font_t *g_med_lg  = NULL;  /* reserved (large medium) */
static lv_font_t *g_xb_md   = NULL;  /* control value */
static lv_font_t *g_xb_lg   = NULL;  /* large value / toast */

static lv_font_t *load_ttf(const char *file, int size) {
    const char *prefixes[] = {
        "A:/usr/local/share/pixelpilot/fonts",
        "A:/usr/share/pixelpilot/fonts",
        "A:./src/gsmenu/fonts",
    };
    for (size_t i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", prefixes[i], file);
        lv_font_t *f = lv_tiny_ttf_create_file(path, size);
        if (f) return f;
    }
    return NULL;
}
#define PP_FONT_MED "BarlowCondensed-Medium.ttf"
#define PP_FONT_XB  "BarlowCondensed-ExtraBold.ttf"

const lv_font_t *pp_font_med_sm(void) { return g_med_sm ? g_med_sm : &lv_font_montserrat_22; }
const lv_font_t *pp_font_med_md(void) { return g_med_md ? g_med_md : &lv_font_montserrat_24; }
const lv_font_t *pp_font_med_lg(void) { return g_med_lg ? g_med_lg : &lv_font_montserrat_32; }
const lv_font_t *pp_font_xb_md(void)  { return g_xb_md  ? g_xb_md  : &lv_font_montserrat_24; }
const lv_font_t *pp_font_xb_lg(void)  { return g_xb_lg  ? g_xb_lg  : &lv_font_montserrat_32; }
```

- [ ] **Step 3: Load the instances in style_init()**

In `src/gsmenu/styles.c`, replace the existing Geist load block at the top of `style_init` (the `if (!g_font_geist_14) … fallback …` blocks, lines 78–89) with:
```c
    if (!g_med_sm) { g_med_sm = load_ttf(PP_FONT_MED, 22); if (g_med_sm) g_med_sm->fallback = &lv_font_montserrat_22; }
    if (!g_med_md) { g_med_md = load_ttf(PP_FONT_MED, 24); if (g_med_md) g_med_md->fallback = &lv_font_montserrat_24; }
    if (!g_med_lg) { g_med_lg = load_ttf(PP_FONT_MED, 32); if (g_med_lg) g_med_lg->fallback = &lv_font_montserrat_32; }
    if (!g_xb_md)  { g_xb_md  = load_ttf(PP_FONT_XB,  24); if (g_xb_md)  g_xb_md->fallback  = &lv_font_montserrat_24; }
    if (!g_xb_lg)  { g_xb_lg  = load_ttf(PP_FONT_XB,  32); if (g_xb_lg)  g_xb_lg->fallback  = &lv_font_montserrat_32; }
```

- [ ] **Step 4: Point the style fonts at the new accessors**

In `src/gsmenu/styles.c`, update the three `text_font` assignments:
- `pp_style_tab`: `pp_font_geist_14()` → `pp_font_med_sm()`
- `pp_style_section_hdr`: `pp_font_geist_14()` → `pp_font_med_sm()`
- `pp_style_row`: `pp_font_geist_16()` → `pp_font_med_md()`

- [ ] **Step 5: Update styles.h declarations**

In `src/gsmenu/styles.h`, replace the three `pp_font_geist_*` declarations with:
```c
const lv_font_t *pp_font_med_sm(void);
const lv_font_t *pp_font_med_md(void);
const lv_font_t *pp_font_med_lg(void);
const lv_font_t *pp_font_xb_md(void);
const lv_font_t *pp_font_xb_lg(void);
```
(If no declarations exist there, add these. Grep `pp_font_geist` to find every reference.)

- [ ] **Step 6: Fix the one other caller (pp_dropdown.c)**

In `src/gsmenu/widgets/pp_dropdown.c`, replace `extern const lv_font_t *pp_font_geist_16(void);` (line 13) with `extern const lv_font_t *pp_font_med_md(void);`, and the use `lv_obj_set_style_text_font(p, pp_font_geist_16(), 0);` (line 80) with `pp_font_med_md()`. Verify with `grep -rn pp_font_geist src/` → no matches remain.

- [ ] **Step 7: Install the TTFs via CMake**

In `CMakeLists.txt`, the existing font install (line ~416) installs `Geist-Regular.ttf`. Add the two Barlow files to the same `install(FILES …)` list:
```cmake
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/gsmenu/fonts/Geist-Regular.ttf
              ${CMAKE_CURRENT_SOURCE_DIR}/src/gsmenu/fonts/BarlowCondensed-Medium.ttf
              ${CMAKE_CURRENT_SOURCE_DIR}/src/gsmenu/fonts/BarlowCondensed-ExtraBold.ttf
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/fonts)
```

- [ ] **Step 8: Build + screenshot**

Run the **Build command**, then capture `/tmp/t3.png` (same command as Task 2 Step 5, new path). Expected: row labels, section headers, and tab labels now render in **Barlow Condensed** (narrow/condensed letterforms); `LV_SYMBOL` tab icons still render (Montserrat fallback intact).

- [ ] **Step 9: Commit**

```bash
git add src/gsmenu/fonts/ src/gsmenu/styles.c src/gsmenu/styles.h src/gsmenu/widgets/pp_dropdown.c CMakeLists.txt
git commit -m "feat(gsmenu): Barlow Condensed font (Medium + ExtraBold)"
```

---

## Task 4: Scrim + centered panel + content-col restructure

**Files:**
- Modify: `src/menu.c` (the `pp_menu_main` layout, lines 47–100)

- [ ] **Step 1: Build the scrim child + centered panel + content column**

In `src/menu.c`, replace the body from the `pp_menu_screen = lv_obj_create(NULL);` block through the end of the five `build_*_tab` + `lv_obj_set_*` calls (lines 47–77) with the structure below. Keep everything after it (the `pp_tabbar_item_t items[5]`, tabbar create, back-group wiring, `pp_osd_main`) — but note the parent changes called out in Step 2.

```c
    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(pp_menu_screen);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, 0);  /* video shows through */
    lv_obj_clear_flag(pp_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Full-frame scrim: a semi-opaque dark child the video shows through.
     * On device the video is the layer below this transparent screen; on the
     * sim a placeholder frame is inserted behind it for screenshots. */
    lv_obj_t *scrim = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(PP_C_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, PP_OPA_SCRIM, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered floating panel: [ rail | content-col ]. Width is proportional
     * with a max cap so it adapts to non-1920 panels. */
    lv_obj_t *panel = lv_obj_create(pp_menu_screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(72));
    lv_obj_set_style_max_width(panel, 1240, 0);
    lv_obj_set_height(panel, LV_PCT(86));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(PP_C_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(panel, 30, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Content column: pages-area (grows) + footer (fixed). Rail is added
     * first by pp_tabbar_create below (moved to index 0). */
    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(content, 0, 0);

    lv_obj_t *pages_area = lv_obj_create(content);
    lv_obj_remove_style_all(pages_area);
    lv_obj_set_width(pages_area, LV_PCT(100));
    lv_obj_set_flex_grow(pages_area, 1);
    lv_obj_clear_flag(pages_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(pages_area, 0, 0);

    /* Build the five tab pages into pages_area (was: into `root`). */
    lv_obj_t *cam = build_camera_tab(pages_area);
    lv_obj_set_size(cam, LV_PCT(100), LV_PCT(100));
    lv_obj_t *lnk = build_link_tab(pages_area);
    lv_obj_set_size(lnk, LV_PCT(100), LV_PCT(100));
    lv_obj_t *dl  = build_dynamiclink_tab(pages_area);
    lv_obj_set_size(dl, LV_PCT(100), LV_PCT(100));
    lv_obj_t *pp  = build_pixelpilot_tab(pages_area);
    lv_obj_set_size(pp, LV_PCT(100), LV_PCT(100));
    lv_obj_t *sys = build_system_tab(pages_area);
    lv_obj_set_size(sys, LV_PCT(100), LV_PCT(100));
```

- [ ] **Step 2: Re-parent the tabbar into the panel (not the old root)**

Further down in `pp_menu_main`, the tabbar is created with `pp_tabbar_create(root, items, 5)`. Change its parent from `root` to `panel`:
```c
    pp_tabbar_t *tabbar = pp_tabbar_create(panel, items, 5);
    lv_obj_move_to_index(pp_tabbar_root(tabbar), 0);   /* rail leftmost */
```
The hidden pages overlay inside `pages_area`; only the active one is shown (handled by the tabbar's `apply_active`), so it fills the area. The `pages[5]` array and `pp_page_set_back_group` loop are unchanged.

- [ ] **Step 3: Build + screenshot**

Run the **Build command**, then capture `/tmp/t4.png`. Expected: the menu is now a **centered** rounded panel over the **dimmed** forest background (scrim), `[ rail | content ]`, instead of the left-anchored panel. Footer area is empty for now (added in Task 6).

- [ ] **Step 4: Verify navigation still works**

Capture a shot after entering the content and moving down a few rows:
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t4_nav.png PP_SIM_KEYS='dss' ./build-sim/pixelpilot"
```
Expected: focus has moved into the page and down two rows (amber focus bar on the 3rd focusable row). Confirms re-parenting preserved the tabbar↔page group wiring.

- [ ] **Step 5: Commit**

```bash
git add src/menu.c
git commit -m "feat(gsmenu): scrim + centered panel + content/pages-area layout"
```

---

## Task 5: Tab rail restyle (wider, opaque, amber active-bar)

**Files:**
- Modify: `src/gsmenu/widgets/pp_tabbar.c` (width + active-bar), `src/gsmenu/styles.c` (`pp_style_tab*`)

- [ ] **Step 1: Widen the rail + size the tabs**

In `src/gsmenu/widgets/pp_tabbar.c`, change the dimensions (lines 9–10):
```c
#define PP_TAB_W 150
#define PP_TAB_H 84
```

- [ ] **Step 2: Active-bar + amber icon on the active tab**

In `src/gsmenu/styles.c`, replace the `pp_style_tab_active` block:
```c
    lv_style_init(&pp_style_tab_active);
    lv_style_set_text_color(&pp_style_tab_active, c_accent);
    lv_style_set_bg_color(&pp_style_tab_active, c_accentd);
    lv_style_set_bg_opa(&pp_style_tab_active, 79);
```
with (4px amber left bar, transparent bg, amber text):
```c
    lv_style_init(&pp_style_tab_active);
    lv_style_set_text_color(&pp_style_tab_active, c_accent);
    lv_style_set_bg_opa(&pp_style_tab_active, LV_OPA_TRANSP);
    lv_style_set_border_side(&pp_style_tab_active, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&pp_style_tab_active, c_accent);
    lv_style_set_border_opa(&pp_style_tab_active, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_tab_active, PP_SCALE(3));
```

- [ ] **Step 3: Dim inactive tabs + brighten the active icon**

In `src/gsmenu/styles.c` `pp_style_tab` block, the inactive text opacity is `115`. Lower it for the design's ~50% inactive look:
```c
    lv_style_set_text_opa(&pp_style_tab, 128);
```
Bump the active tab icon: the active label should be `PP_C_INK` while the icon is amber. Since both share the tab text color, keep the amber active text (acceptable simplification) — the active-bar + amber already distinguish it. (No extra change needed.)

- [ ] **Step 4: Build + screenshot the five tabs**

Run the **Build command**, then capture each tab (rail is in `main_group`; `s`/`w` move tabs, `d` enters):
```bash
for i in 0 1 2 3 4; do
  K=$(printf 's%.0s' $(seq 1 $i))
  nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t5_tab$i.png PP_SIM_KEYS='$K' ./build-sim/pixelpilot"
done
```
Expected: the active tab shows a **4px amber left bar** + amber icon/label; inactive tabs are dimmed; rail is opaque and wider. Each tab's page content shows.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_tabbar.c src/gsmenu/styles.c
git commit -m "feat(gsmenu): tab rail — wider, opaque, amber active-bar"
```

---

## Task 6: Static footer legend (pp_footer)

**Files:**
- Create: `src/gsmenu/widgets/pp_footer.c`, `src/gsmenu/widgets/pp_footer.h`
- Modify: `src/menu.c` (build footer into `content`), `CMakeLists.txt` (sim sources)

- [ ] **Step 1: Create pp_footer.h**

```c
#pragma once
#include "lvgl/lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Static key-legend row: ▲▼ NAVIGATE · ▶ ENTER · ◀ BACK. */
lv_obj_t *pp_footer_create(lv_obj_t *parent);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create pp_footer.c**

```c
#include "pp_footer.h"
#include "../styles.h"

/* One [chip] LABEL group. `glyph` is an LV_SYMBOL string (Montserrat
 * fallback); `text` is the uppercase action label. */
static void add_item(lv_obj_t *bar, const char *glyph, const char *text) {
    lv_obj_t *chip = lv_label_create(bar);
    lv_label_set_text(chip, glyph);
    lv_obj_set_style_text_font(chip, pp_font_xb_md(), 0);
    lv_obj_set_style_text_color(chip, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, PP_SCALE(6), 0);
    lv_obj_set_style_pad_ver(chip, PP_SCALE(2), 0);
    lv_obj_set_style_radius(chip, PP_SCALE(5), 0);
    lv_obj_set_style_border_width(chip, 2, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(chip, 77, 0);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, pp_font_med_sm(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_opa(lbl, 153, 0);            /* ~60% */
    lv_obj_set_style_text_letter_space(lbl, PP_SCALE(2), 0);
    lv_obj_set_style_pad_right(lbl, PP_SCALE(22), 0);  /* gap before next group */
    lv_obj_set_style_pad_left(lbl, PP_SCALE(8), 0);
}

lv_obj_t *pp_footer_create(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, 46, 0);               /* ~18% */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(bar, 20, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_hor(bar, PP_SCALE(20), 0);
    lv_obj_set_style_pad_ver(bar, PP_SCALE(10), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    add_item(bar, LV_SYMBOL_UP LV_SYMBOL_DOWN, "NAVIGATE");
    add_item(bar, LV_SYMBOL_RIGHT, "ENTER");
    add_item(bar, LV_SYMBOL_LEFT, "BACK");
    return bar;
}
```

- [ ] **Step 3: Wire the footer into the content column**

In `src/menu.c`, add the include near the other widget includes:
```c
#include "gsmenu/widgets/pp_footer.h"
```
Then, immediately after the `pages_area` block (end of Task 4 Step 1), add the footer as the second child of `content`:
```c
    pp_footer_create(content);
```

- [ ] **Step 4: Add pp_footer.c to BOTH build source lists**

`CMakeLists.txt` has two explicit source lists that both enumerate the widgets:
`SIMULATOR_SOURCES` (the `pp_toast.c` line is ~79) and the device `SOURCE_FILES`
(the `pp_toast.c` line is ~135). Add the new widget after `pp_toast.c` in **each**:
```cmake
        src/gsmenu/widgets/pp_footer.h
        src/gsmenu/widgets/pp_footer.c
```
Confirm both: `grep -n "pp_footer.c" CMakeLists.txt` → two matches.

- [ ] **Step 5: Build + screenshot**

Run the **Build command**, then capture `/tmp/t6.png`. Expected: a footer row at the bottom of the content column with three chip+label groups — `▲▼ NAVIGATE`, `▶ ENTER`, `◀ BACK` — over a faint dark bar with a top hairline.

- [ ] **Step 6: Commit**

```bash
git add src/gsmenu/widgets/pp_footer.c src/gsmenu/widgets/pp_footer.h src/menu.c CMakeLists.txt
git commit -m "feat(gsmenu): static footer key-legend"
```

---

## Task 7: Remove per-row icons + amber focus

**Files:**
- Modify: `src/gsmenu/widgets/pp_row.c`, `pp_dropdown.c`, `pp_toggle.c`, `pp_slider.c` (drop icon render), `src/gsmenu/styles.c` (`pp_style_row_focus`, `pp_style_value_focus`)

- [ ] **Step 1: Stop rendering the leading icon in each row widget**

In each of `pp_row.c`, `pp_dropdown.c`, `pp_toggle.c`, `pp_slider.c`, delete the icon-creation block (keep the `icon_text` parameter in the signature — callers are unchanged). The block to delete looks like:
```c
    if (icon_text) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_pad_right(icon, /* 12 or PP_SCALE(12) */, 0);
    }
```
Replace it with:
```c
    (void)icon_text;   /* OSD reskin: rows are label + value only (no leading icon) */
```

- [ ] **Step 2: Recolor row focus to amber-on-light**

In `src/gsmenu/styles.c`, the `pp_style_row_focus` block already uses `c_accent` (now amber) for the left border — good. Adjust the focus background from the accent tint to the design's neutral light tint. Replace:
```c
    lv_style_set_bg_color(&pp_style_row_focus, c_accentd);
    lv_style_set_bg_opa(&pp_style_row_focus, 79);
```
with:
```c
    lv_style_set_bg_color(&pp_style_row_focus, lv_color_hex(PP_C_INK));
    lv_style_set_bg_opa(&pp_style_row_focus, 18);   /* ~7% white */
```
Leave the `pp_style_value_focus` text color as `c_accent` (now amber).

- [ ] **Step 3: Build + screenshot**

Run the **Build command**, then capture `/tmp/t7.png` with the content focused (`PP_SIM_KEYS='d'`). Expected: rows are **label + value only** (no leading glyph); the focused row has a light tint + amber left bar; the focused value is amber.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/widgets/pp_row.c src/gsmenu/widgets/pp_dropdown.c src/gsmenu/widgets/pp_toggle.c src/gsmenu/widgets/pp_slider.c src/gsmenu/styles.c
git commit -m "feat(gsmenu): remove per-row icons; amber focus"
```

---

## Task 8: Toggle restyle (amber track, dark knob)

**Files:**
- Modify: `src/gsmenu/styles.c` (`pp_style_switch_on`), `src/gsmenu/widgets/pp_toggle.c` (off-track + knob)

- [ ] **Step 1: ON-track amber (already a token, confirm)**

In `src/gsmenu/styles.c`, `pp_style_switch_on` already sets `bg_color` to `c_accent` (amber). No change needed beyond confirming it reads `c_accent`.

- [ ] **Step 2: Style the off-track and knob in pp_toggle.c**

In `src/gsmenu/widgets/pp_toggle.c`, after the `lv_obj_set_size(sw, PP_SCALE(40), PP_SCALE(22));` line, add:
```c
    /* OFF track: dim white. Knob: bright white (OFF) / dark (ON). */
    lv_obj_set_style_bg_color(sw, lv_color_hex(PP_C_INK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, 41, LV_PART_MAIN);                       /* ~16% */
    lv_obj_set_style_bg_color(sw, lv_color_hex(PP_C_INK), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, 235, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x06231A),
                              LV_PART_KNOB | LV_STATE_CHECKED);          /* dark knob when ON */
```

- [ ] **Step 3: Build + screenshot**

Run the **Build command**. Capture a toggle in both states — Camera ▸ ROI ▸ Enabled is ON by default in the dummy backend; navigate to it. Use `PP_SIM_KEYS='d'` then enough `s` to reach a toggle row:
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t8.png PP_SIM_KEYS='dssssssss' ./build-sim/pixelpilot"
```
Expected: ON toggles show an **amber track with a dark knob on the right**; OFF toggles show a dim track with a bright knob on the left.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/styles.c src/gsmenu/widgets/pp_toggle.c
git commit -m "feat(gsmenu): toggle — amber track, dark knob"
```

---

## Task 9: Stepper restyle (amber edit value, ExtraBold)

**Files:**
- Modify: `src/gsmenu/widgets/pp_slider.c` (`set_edit_state` colors + value font)

- [ ] **Step 1: Amber edit colors**

In `src/gsmenu/widgets/pp_slider.c`, in `set_edit_state`, replace the hardcoded `0x6B7FFF` (two places) with the amber token:
```c
static void set_edit_state(pp_slider_data_t *d, bool active) {
    lv_color_t c = active ? lv_color_hex(PP_C_ACCENT) : lv_color_hex(PP_C_INK);
    lv_opa_t   o = active ? LV_OPA_COVER : 90;
    lv_obj_set_style_text_color(d->up_chev,   c, 0);
    lv_obj_set_style_text_color(d->down_chev, c, 0);
    lv_obj_set_style_text_opa(d->up_chev,   o, 0);
    lv_obj_set_style_text_opa(d->down_chev, o, 0);
    lv_obj_set_style_text_color(d->num,
        active ? lv_color_hex(PP_C_ACCENT) : lv_color_hex(PP_C_INK), 0);
}
```

- [ ] **Step 2: ExtraBold value font**

In `pp_slider.c`, after the `lv_obj_t *num = lv_label_create(col);` line, add:
```c
    lv_obj_set_style_text_font(num, pp_font_xb_md(), 0);
```

- [ ] **Step 3: Build + screenshot (idle and editing)**

Run the **Build command**. Navigate to a stepper row (e.g. Camera ▸ GOP size) and enter edit mode with a trailing `\n`:
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t9_edit.png PP_SIM_KEYS='dsssss\n' ./build-sim/pixelpilot"
```
Expected: in edit mode the value + chevrons turn **amber** and the number is ExtraBold; idle steppers show a white ExtraBold value with dim chevrons.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/widgets/pp_slider.c
git commit -m "feat(gsmenu): stepper — amber edit value (ExtraBold)"
```

---

## Task 10: Section header tick + recolor

**Files:**
- Modify: `src/gsmenu/widgets/pp_section_header.c` (prepend a tick), `src/gsmenu/styles.c` (`pp_style_section_hdr` tracking)

- [ ] **Step 1: Add a small left tick to the section header**

`pp_section_header` currently creates a single label. Wrap it so a small dim tick precedes the title. Replace the body of `pp_section_header` in `src/gsmenu/widgets/pp_section_header.c` with:
```c
lv_obj_t *pp_section_header(lv_obj_t *parent, const char *text) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(row, &pp_style_section_hdr, 0);

    lv_obj_t *tick = lv_obj_create(row);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, PP_SCALE(16), PP_SCALE(3));
    lv_obj_set_style_bg_color(tick, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_bg_opa(tick, 102, 0);              /* ~40% */
    lv_obj_set_style_radius(tick, 1, 0);
    lv_obj_set_style_margin_right(tick, PP_SCALE(8), 0);

    lv_obj_t *label = lv_label_create(row);
    size_t n = strlen(text);
    char *upper = malloc(n + 1);
    for (size_t i = 0; i < n; i++) upper[i] = (char)toupper((unsigned char)text[i]);
    upper[n] = '\0';
    lv_label_set_text(label, upper);
    free(upper);
    return row;
}
```
> Note: `pp_style_section_hdr` sets `pad_left`/padding; the row now carries that style. The title still inherits the section font/color from the style applied to the row (label inherits text color/opacity/font). If the label doesn't inherit the dim color, add `lv_obj_add_style(label, &pp_style_section_hdr, 0);` too.

- [ ] **Step 2: Build + screenshot**

Run the **Build command**, capture `/tmp/t10.png`. Expected: each section header (`VIDEO`, `ROI`, `IMAGE`, …) shows a small dim tick before the uppercase tracked title.

- [ ] **Step 3: Commit**

```bash
git add src/gsmenu/widgets/pp_section_header.c src/gsmenu/styles.c
git commit -m "feat(gsmenu): section header tick"
```

---

## Task 11: Select picker → centered modal

**Files:**
- Modify: `src/gsmenu/widgets/pp_dropdown.c` (store label; rebuild `popup_open`/`popup_refresh`)

- [ ] **Step 1: Store the row label for the modal header**

In `src/gsmenu/widgets/pp_dropdown.c`, add a `char *label;` field to `struct pp_dd_data` (after `char *domain, *page, *key;`). In `pp_dropdown(...)`, after `d->key = strdup(key);`, add `d->label = strdup(label);`. In `on_delete`, add `free(d->label);`.

- [ ] **Step 2: Rebuild popup_open as a centered modal**

Replace the entire `popup_open` function in `pp_dropdown.c` with the version below (keeps the same `d->popup` handle and option semantics; adds a backdrop, header with amber marker, amber-highlighted option with left bar, and a "CURRENT" tag on the saved selection):
```c
static void popup_open(pp_dd_data_t *d) {
    if (d->popup) return;

    lv_obj_t *top = lv_layer_top();

    /* Dim backdrop behind the modal. */
    lv_obj_t *back = lv_obj_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x060709), 0);
    lv_obj_set_style_bg_opa(back, 140, 0);             /* ~55% */
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);

    /* Modal box. */
    lv_obj_t *p = lv_obj_create(back);
    lv_obj_remove_style_all(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(PP_C_MODAL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(PP_C_ACCENT), 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_pad_all(p, PP_SCALE(6), 0);
    lv_obj_set_style_text_font(p, pp_font_med_md(), 0);
    lv_obj_set_width(p, PP_SCALE(300));
    lv_obj_set_style_max_height(p, lv_display_get_vertical_resolution(NULL) - 160, 0);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_center(p);

    /* Header: amber marker + uppercase label. */
    lv_obj_t *hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, PP_SCALE(8), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mark = lv_obj_create(hdr);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, PP_SCALE(4), PP_SCALE(18));
    lv_obj_set_style_bg_color(mark, lv_color_hex(PP_C_ACCENT), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(mark, PP_SCALE(8), 0);
    lv_obj_t *htxt = lv_label_create(hdr);
    {
        char up[64]; size_t n = 0;
        for (const char *s = d->label ? d->label : ""; *s && n < sizeof(up)-1; ++s, ++n)
            up[n] = (char)toupper((unsigned char)*s);
        up[n] = '\0';
        lv_label_set_text(htxt, up);
    }
    lv_obj_set_style_text_font(htxt, pp_font_xb_md(), 0);
    lv_obj_set_style_text_color(htxt, lv_color_hex(PP_C_INK), 0);

    uint16_t cur   = lv_dropdown_get_selected(d->dd);
    uint16_t saved = d->saved_sel;
    uint16_t n     = lv_dropdown_get_option_count(d->dd);
    for (uint16_t i = 0; i < n; i++) {
        char buf[64];
        lv_dropdown_set_selected(d->dd, i);
        lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);

        lv_obj_t *item = lv_obj_create(p);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(item, PP_SCALE(12), 0);
        lv_obj_set_style_pad_ver(item, PP_SCALE(6), 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        if (i == cur) {
            lv_obj_set_style_bg_color(item, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_bg_opa(item, 26, 0);                  /* ~10% */
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_border_opa(item, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(item, PP_SCALE(3), 0);
        }
        lv_obj_t *l = lv_label_create(item);
        lv_label_set_text(l, buf);
        lv_obj_set_flex_grow(l, 1);
        if (i == cur) {
            lv_obj_set_style_text_color(l, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_text_font(l, pp_font_xb_md(), 0);
        } else {
            lv_obj_set_style_text_color(l, lv_color_hex(PP_C_INK), 0);
            lv_obj_set_style_text_opa(l, 200, 0);
        }
        if (i == saved) {
            lv_obj_t *tag = lv_label_create(item);
            lv_label_set_text(tag, "CURRENT");
            lv_obj_set_style_text_font(tag, pp_font_med_sm(), 0);
            lv_obj_set_style_text_color(tag, lv_color_hex(PP_C_INK), 0);
            lv_obj_set_style_text_opa(tag, 102, 0);                /* ~40% */
            lv_obj_set_style_text_letter_space(tag, PP_SCALE(2), 0);
        }
    }
    lv_dropdown_set_selected(d->dd, cur);

    if (cur < lv_obj_get_child_cnt(p)) {
        lv_obj_t *cur_item = lv_obj_get_child(p, cur + 1); /* +1 for header */
        if (cur_item) lv_obj_scroll_to_view(cur_item, LV_ANIM_OFF);
    }
    d->popup = back;   /* delete the backdrop closes the whole modal */
}
```

- [ ] **Step 3: Update popup_refresh for the header offset**

In `popup_refresh`, the option items are now children of the modal box (which is `lv_obj_get_child(d->popup, 0)`), and index 0 is the header. Replace the function body to walk the inner box and skip the header:
```c
static void popup_refresh(pp_dd_data_t *d) {
    if (!d->popup) return;
    lv_obj_t *box = lv_obj_get_child(d->popup, 0);     /* modal box inside backdrop */
    if (!box) return;
    uint16_t cur = lv_dropdown_get_selected(d->dd);
    uint32_t cnt = lv_obj_get_child_cnt(box);
    for (uint32_t i = 1; i < cnt; i++) {               /* i=0 is header */
        lv_obj_t *item = lv_obj_get_child(box, i);
        lv_obj_t *lbl  = lv_obj_get_child(item, 0);
        bool on = (i - 1) == cur;
        lv_obj_set_style_bg_opa(item, on ? 26 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, on ? PP_SCALE(3) : 0, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(on ? PP_C_ACCENT : PP_C_INK), 0);
            lv_obj_set_style_text_opa(lbl, on ? LV_OPA_COVER : 200, 0);
            lv_obj_set_style_text_font(lbl, on ? pp_font_xb_md() : pp_font_med_md(), 0);
        }
        if (on) lv_obj_scroll_to_view(item, LV_ANIM_ON);
    }
}
```
> `popup_close` already does `lv_obj_del(d->popup)`; since `d->popup` is now the backdrop and the box is its child, deleting it removes the whole modal — no change needed.

- [ ] **Step 4: Build + screenshot the modal**

Run the **Build command**. Open Camera ▸ Size and enter its picker (`d` into content, `\n` to open the modal on the first row, which is Size):
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/t11.png PP_SIM_KEYS='d\n' ./build-sim/pixelpilot"
```
Expected: a **centered modal** with a 1px amber border, an amber marker + `SIZE` header, options listed, the highlighted option with an amber left-bar + amber ExtraBold value, and a dim `CURRENT` tag on the saved value — matching the first reference screenshot.

- [ ] **Step 5: Commit**

```bash
git add src/gsmenu/widgets/pp_dropdown.c
git commit -m "feat(gsmenu): select picker as centered modal (header + CURRENT)"
```

---

## Task 12: Toast restyle

**Files:**
- Modify: `src/gsmenu/widgets/pp_toast.c`

- [ ] **Step 1: Restyle the toast body**

In `src/gsmenu/widgets/pp_toast.c`, change the background to the dark modal token with a crit border (replace the "Background: red, opaque, rounded" block):
```c
    /* Background: dark panel, crit border, rounded (design toast). */
    lv_obj_set_style_bg_color(toast, lv_color_hex(PP_C_MODAL), 0);
    lv_obj_set_style_bg_opa(toast, 240, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(PP_C_CRIT), 0);
```
Remove the later `lv_obj_set_style_border_width(toast, 0, 0);` line (it would undo the border). Add `#include "../styles.h"` at the top if not present.

- [ ] **Step 2: Uppercase ExtraBold message + dot**

Replace the label block so the text is uppercase ExtraBold with a leading crit dot:
```c
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = lv_obj_create(toast);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, PP_SCALE(8), PP_SCALE(8));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(PP_C_CRIT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(dot, PP_SCALE(8), 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_font(label, pp_font_xb_md(), 0);
    lv_obj_set_style_text_letter_space(label, PP_SCALE(1), 0);
```
(Delete the old `lv_label_create`/`set_width`/Montserrat-font label block this replaces.)

- [ ] **Step 3: Build + screenshot a toast**

Run the **Build command**. The dummy backend rejects locked rows with a toast; trigger one by trying to edit a Dynamic-Link-locked row, or temporarily call `pp_toast_error("APPLIED")` is not needed — instead reach a locked row. Simplest: confirm visually by reaching any error path, or just confirm the build and inspect a toast by adding a trailing `\n` on a locked row. Capture `/tmp/t12.png` and confirm the toast (if shown) is the dark panel + red border + dot + uppercase. If no toast path is reachable headlessly, verify the code compiles and defer the visual to the final sweep.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/widgets/pp_toast.c
git commit -m "feat(gsmenu): toast — dark panel, crit border, dot"
```

---

## Task 13: Drill-down pickup + full visual sweep

**Files:**
- Modify: `src/gsmenu/widgets/pp_drilldown.c` (token pickup, if it hardcodes colors/fonts)

- [ ] **Step 1: Align drill-down to the new tokens**

Open `src/gsmenu/widgets/pp_drilldown.c`. If it uses `pp_style_panel` it already inherits the new opaque panel — confirm. Replace any hardcoded blue hex (`0x4C60D8`/`0x6B7FFF`) with `lv_color_hex(PP_C_ACCENT)` and any `pp_font_geist_*` with the `pp_font_med_*` equivalents. If none are present, no change.

- [ ] **Step 2: Build + full sweep across all tabs**

Run the **Build command**, then capture every tab and the key states:
```bash
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_camera.png PP_SIM_KEYS='d'        ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_link.png   PP_SIM_KEYS='sd'       ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_dlink.png  PP_SIM_KEYS='ssd'      ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_pp.png     PP_SIM_KEYS='sssd'     ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_system.png PP_SIM_KEYS='ssssd'    ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_modal.png  PP_SIM_KEYS='d\n'      ./build-sim/pixelpilot"
nix-shell shell-sim.nix --run "SDL_VIDEODRIVER=dummy PP_SIM_SHOT=/tmp/sweep_edit.png   PP_SIM_KEYS='dsssss\n' ./build-sim/pixelpilot"
```
Read each PNG and compare against the reference screenshots + `design_handoff_pixelpilot_menu/`. Confirm: amber accent throughout, Barlow Condensed type, opaque centered panel over dimmed video, amber active tab-bar, label+value rows, footer legend, toggle/stepper/modal styling. Send a representative few to the user (SendUserFile).

- [ ] **Step 3: Confirm navigation end-to-end**

In a real (non-screenshot) run or by reasoning through the key scripts above, confirm tab switching, row nav, stepper edit/apply/cancel, the select modal, and back-to-rail all behave exactly as before the reskin. No behavior changed.

- [ ] **Step 4: Final commit**

```bash
git add -A src/gsmenu/widgets/pp_drilldown.c
git commit -m "feat(gsmenu): drill-down token pickup; OSD reskin visual sweep"
```

---

## Self-Review (completed during planning)

**Spec coverage:** palette (T2), font (T3), scrim+centered panel (T4), tab rail amber bar (T5), static footer (T6), remove row icons + amber focus (T7), toggle (T8), stepper (T9), section tick (T10), select modal + CURRENT (T11), toast (T12), drill-down + sweep (T13), font install (T3 Step 7). Out-of-scope items (header bar, counter, confirm dialogs, per-mode legend, behavior changes) are correctly absent. ✓

**Placeholder scan:** every code step shows concrete code or an exact find/replace; the only deferred detail is exact font px (intentional, sim-tuned per the Conventions note). ✓

**Type/name consistency:** font accessors `pp_font_med_sm/md/lg` + `pp_font_xb_md/lg` are defined in T3 and used consistently in T6/T9/T11/T12; `PP_C_*` macros defined in T2 used throughout; `pp_footer_create` defined in T6 and called in T6. ✓
