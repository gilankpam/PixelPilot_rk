#include "lvgl/lvgl.h"
#include "lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>   /* getenv: PP_PANEL_FX opt-in for the costly panel effects */

/* Geist font instances loaded via lv_tiny_ttf at startup. NULL if the
 * TTF wasn't found at any known prefix — the styles fall back to the
 * built-in Montserrat in that case. */
static lv_font_t *g_font_geist_14 = NULL;
static lv_font_t *g_font_geist_16 = NULL;
static lv_font_t *g_font_geist_22 = NULL;

static lv_font_t *load_geist(int size) {
    const char *rel = "Geist-Regular.ttf";
    /* Prefer the search prefixes used by find_resource_file. Tiny TTF
     * needs a real path with "A:" prefix (LVGL POSIX FS driver). */
    const char *prefixes[] = {
        "A:/usr/local/share/pixelpilot/fonts",
        "A:/usr/share/pixelpilot/fonts",
        "A:./src/gsmenu/fonts",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", prefixes[i], rel);
        lv_font_t *f = lv_tiny_ttf_create_file(path, size);
        if (f) return f;
    }
    return NULL;
}

const lv_font_t *pp_font_geist_14(void) {
    return g_font_geist_14 ? g_font_geist_14 : &lv_font_montserrat_14;
}
const lv_font_t *pp_font_geist_16(void) {
    return g_font_geist_16 ? g_font_geist_16 : &lv_font_montserrat_16;
}
const lv_font_t *pp_font_geist_22(void) {
    return g_font_geist_22 ? g_font_geist_22 : &lv_font_montserrat_22;
}


lv_style_t style_rootmenu;
lv_style_t style_openipc;
lv_style_t style_openipc_dropdown;
lv_style_t style_openipc_outline;
lv_style_t style_openipc_textcolor;
lv_style_t style_openipc_disabled;
lv_style_t style_openipc_section;
lv_style_t style_openipc_dark_background;
lv_style_t style_openipc_lightdark_background;

lv_style_t pp_style_panel;
lv_style_t pp_style_panel_alt;
lv_style_t pp_style_tabbar;
lv_style_t pp_style_tab;
lv_style_t pp_style_tab_active;
lv_style_t pp_style_section_hdr;
lv_style_t pp_style_row;
lv_style_t pp_style_row_focus;
lv_style_t pp_style_value_focus;
lv_style_t pp_style_divider;
lv_style_t pp_style_switch_on;


int style_init(void) {
    /* Load Geist TTF at the sizes we use. If unavailable, the
     * pp_font_geist_* accessors return Montserrat as a fallback.
     *
     * Geist is a normal text typeface and does not include the
     * LV_SYMBOL_* private-use glyphs. Chain Montserrat at the same
     * size as the fallback font so icons keep rendering inline. */
    if (!g_font_geist_14) {
        g_font_geist_14 = load_geist(14);
        if (g_font_geist_14) g_font_geist_14->fallback = &lv_font_montserrat_14;
    }
    if (!g_font_geist_16) {
        g_font_geist_16 = load_geist(16);
        if (g_font_geist_16) g_font_geist_16->fallback = &lv_font_montserrat_16;
    }
    if (!g_font_geist_22) {
        g_font_geist_22 = load_geist(22);
        if (g_font_geist_22) g_font_geist_22->fallback = &lv_font_montserrat_22;
    }

    lv_style_reset(&style_rootmenu);
    lv_style_init(&style_rootmenu);
    lv_style_set_bg_color(&style_rootmenu, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 50));
    lv_style_set_pad_top(&style_rootmenu, 0);
    lv_style_set_pad_bottom(&style_rootmenu, 0);
    lv_style_set_pad_left(&style_rootmenu, 0);
    lv_style_set_pad_right(&style_rootmenu, 0);
    lv_style_set_radius(&style_rootmenu, 0);
    lv_style_set_border_width(&style_rootmenu, 0);
    lv_style_set_border_color(&style_rootmenu, lv_color_hex(0xff4c60d8));

    lv_style_reset(&style_openipc_section);
    lv_style_init(&style_openipc_section);
    lv_style_set_bg_color(&style_openipc_section, lv_color_lighten( lv_color_make(0xcd, 0xcd, 0xcd), 50));

    lv_style_reset(&style_openipc_dark_background);
    lv_style_init(&style_openipc_dark_background);
    lv_style_set_bg_color(&style_openipc_dark_background, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 90));    

    lv_style_reset(&style_openipc_lightdark_background);
    lv_style_init(&style_openipc_lightdark_background);
    lv_style_set_bg_color(&style_openipc_lightdark_background, lv_color_darken( lv_color_make(0xcd, 0xcd, 0xcd), 30));    

    lv_style_reset(&style_openipc);
    lv_style_init(&style_openipc);
    lv_style_set_bg_color(&style_openipc, lv_color_hex(0xff4c60d8));
    lv_style_set_outline_color(&style_openipc, lv_color_hex(0xff4c60d8));
    lv_style_set_arc_color(&style_openipc, lv_color_hex(0xff4c60d8));

    lv_style_init(&style_openipc_dropdown);
    lv_style_set_bg_color(&style_openipc_dropdown, lv_color_hex(0xff4c60d8));

    lv_style_reset(&style_openipc_outline);
    lv_style_init(&style_openipc_outline);
    lv_style_set_outline_color(&style_openipc_outline, lv_color_hex(0xff4c60d8));
    lv_style_set_outline_width(&style_openipc_outline,7);

    lv_style_reset(&style_openipc_textcolor);
    lv_style_init(&style_openipc_textcolor);
    lv_style_set_text_color(&style_openipc_textcolor, lv_color_hex(0xff4c60d8));    

    lv_style_reset(&style_openipc_disabled);
    lv_style_init(&style_openipc_disabled);
    lv_style_set_bg_color(&style_openipc_disabled, lv_color_hex(0xff4c60d8));
    lv_style_set_text_color(&style_openipc_disabled, lv_color_darken( lv_color_make(0xff, 0xff, 0xff), 50));
    //lv_style_set_line_color(&style_openipc_disabled, lv_color_hex(0xffd8ce36));
    //lv_style_set_border_color(&style_openipc_disabled, lv_color_hex(0xffe61212));

    /* Color tokens */
    const lv_color_t c_panel   = lv_color_hex(0x0F1116);
    const lv_color_t c_tabbar  = lv_color_hex(0x000000);
    const lv_color_t c_text    = lv_color_hex(0xFFFFFF);
    const lv_color_t c_accent  = lv_color_hex(0x6B7FFF);
    const lv_color_t c_accentd = lv_color_hex(0x4C60D8);

    lv_style_init(&pp_style_panel);
    lv_style_set_bg_color(&pp_style_panel, c_panel);
    lv_style_set_bg_opa(&pp_style_panel, LV_OPA_70); /* lets more video bleed through; backdrop blur active */
    lv_style_set_border_width(&pp_style_panel, 0);
    lv_style_set_radius(&pp_style_panel, 0);
    lv_style_set_pad_all(&pp_style_panel, 0);

    /* The two new v9.5 software effects below (native backdrop blur + Gaussian
     * drop shadow) are the dominant gsmenu nav-slowness factor on the RK3566
     * ground station: ~250 ms of the per-render cost, on top of which they
     * neither vectorize (NEON) nor parallelize across draw units (the IIR blur
     * is one sequential whole-panel task). Disabled by default for responsive
     * navigation; set PP_PANEL_FX=1 to restore the look on capable hardware.
     * See docs/superpowers/notes/2026-06-03-gsmenu-nav-slowness-handoff.md. */
    if (getenv("PP_PANEL_FX")) {
    /* Native backdrop blur — blurs the live-video content behind each page panel
     * (LVGL v9.5+). Radius 8 is a conservative starting value; tune after
     * interactive verification. blur_backdrop=true requires bg_opa < LV_OPA_COVER
     * so the blurred content bleeds through the semi-transparent background. */
    lv_style_set_blur_radius(&pp_style_panel, 8);
    lv_style_set_blur_backdrop(&pp_style_panel, true);

    /* Drop shadow — rendered by the new v9.5 drop_shadow API (Gaussian blur,
     * CPU-only; distinct from the legacy lv_style_set_shadow_* box shadow). */
    lv_style_set_drop_shadow_radius(&pp_style_panel, 24);
    lv_style_set_drop_shadow_opa(&pp_style_panel, LV_OPA_50);
    lv_style_set_drop_shadow_color(&pp_style_panel, lv_color_black());
    lv_style_set_drop_shadow_offset_y(&pp_style_panel, 4);
    }

    /* ALT state variant — distinguishing bg color for future day/night theme.
     * Applied when LV_STATE_ALT is set on the panel object; no user-facing
     * toggle yet (this is the seam only). */
    lv_style_init(&pp_style_panel_alt);
    lv_style_set_bg_color(&pp_style_panel_alt, lv_color_hex(0x1A1F2E));
    lv_style_set_bg_opa(&pp_style_panel_alt, LV_OPA_90);

    lv_style_init(&pp_style_tabbar);
    lv_style_set_bg_color(&pp_style_tabbar, c_tabbar);
    lv_style_set_bg_opa(&pp_style_tabbar, 77);
    lv_style_set_border_side(&pp_style_tabbar, LV_BORDER_SIDE_RIGHT);
    lv_style_set_border_color(&pp_style_tabbar, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_tabbar, 33);
    lv_style_set_border_width(&pp_style_tabbar, 1);
    lv_style_set_pad_ver(&pp_style_tabbar, 16);
    lv_style_set_radius(&pp_style_tabbar, 0);

    lv_style_init(&pp_style_tab);
    lv_style_set_bg_opa(&pp_style_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&pp_style_tab, c_text);
    lv_style_set_text_opa(&pp_style_tab, 115);
    lv_style_set_text_font(&pp_style_tab, pp_font_geist_14());
    lv_style_set_pad_ver(&pp_style_tab, 12);
    lv_style_set_radius(&pp_style_tab, 0);
    lv_style_set_border_width(&pp_style_tab, 0);

    lv_style_init(&pp_style_tab_active);
    lv_style_set_text_color(&pp_style_tab_active, c_accent);
    lv_style_set_bg_color(&pp_style_tab_active, c_accentd);
    lv_style_set_bg_opa(&pp_style_tab_active, 79);

    lv_style_init(&pp_style_section_hdr);
    lv_style_set_text_color(&pp_style_section_hdr, c_text);
    lv_style_set_text_opa(&pp_style_section_hdr, 102);
    lv_style_set_text_font(&pp_style_section_hdr, pp_font_geist_14());
    lv_style_set_text_letter_space(&pp_style_section_hdr, 2);
    lv_style_set_pad_top(&pp_style_section_hdr, 8);
    lv_style_set_pad_left(&pp_style_section_hdr, 20);
    lv_style_set_pad_bottom(&pp_style_section_hdr, 4);

    lv_style_init(&pp_style_row);
    lv_style_set_bg_opa(&pp_style_row, LV_OPA_TRANSP);
    lv_style_set_pad_hor(&pp_style_row, 20);
    lv_style_set_pad_ver(&pp_style_row, 8);
    lv_style_set_text_color(&pp_style_row, c_text);
    lv_style_set_text_font(&pp_style_row, pp_font_geist_16());
    lv_style_set_border_side(&pp_style_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&pp_style_row, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_row, 33);
    lv_style_set_border_width(&pp_style_row, 1);
    lv_style_set_radius(&pp_style_row, 0);

    lv_style_init(&pp_style_row_focus);
    lv_style_set_bg_color(&pp_style_row_focus, c_accentd);
    lv_style_set_bg_opa(&pp_style_row_focus, 79);
    lv_style_set_border_side(&pp_style_row_focus, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&pp_style_row_focus, c_accent);
    lv_style_set_border_opa(&pp_style_row_focus, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_row_focus, 2);
    lv_style_set_pad_left(&pp_style_row_focus, 18);

    lv_style_init(&pp_style_value_focus);
    lv_style_set_text_color(&pp_style_value_focus, c_accent);

    lv_style_init(&pp_style_divider);
    lv_style_set_border_width(&pp_style_divider, 0);

    lv_style_init(&pp_style_switch_on);
    lv_style_set_bg_color(&pp_style_switch_on, c_accent);

    return 0;
}
