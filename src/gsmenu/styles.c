#include "lvgl/lvgl.h"
#include "lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>   /* getenv: PP_PANEL_FX opt-in for the costly panel effects */
#include "styles.h"   /* PP_SCALE + style/ font declarations */

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
    if (!g_med_sm) { g_med_sm = load_ttf(PP_FONT_MED, 22); if (g_med_sm) g_med_sm->fallback = &lv_font_montserrat_22; }
    if (!g_med_md) { g_med_md = load_ttf(PP_FONT_MED, 24); if (g_med_md) g_med_md->fallback = &lv_font_montserrat_24; }
    if (!g_med_lg) { g_med_lg = load_ttf(PP_FONT_MED, 32); if (g_med_lg) g_med_lg->fallback = &lv_font_montserrat_32; }
    if (!g_xb_md)  { g_xb_md  = load_ttf(PP_FONT_XB,  24); if (g_xb_md)  g_xb_md->fallback  = &lv_font_montserrat_24; }
    if (!g_xb_lg)  { g_xb_lg  = load_ttf(PP_FONT_XB,  32); if (g_xb_lg)  g_xb_lg->fallback  = &lv_font_montserrat_32; }

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

    /* Color tokens — OSD reskin (amber/opaque). */
    const lv_color_t c_panel   = lv_color_hex(PP_C_PANEL);
    const lv_color_t c_tabbar  = lv_color_hex(PP_C_RAIL);
    const lv_color_t c_text    = lv_color_hex(PP_C_INK);
    const lv_color_t c_accent  = lv_color_hex(PP_C_ACCENT);

    lv_style_init(&pp_style_panel);
    lv_style_set_bg_color(&pp_style_panel, c_panel);
    lv_style_set_bg_opa(&pp_style_panel, LV_OPA_COVER); /* opaque flat fill (design: no blur) */
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
    if (0 && getenv("PP_PANEL_FX")) {
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
    lv_style_set_bg_opa(&pp_style_tabbar, LV_OPA_COVER);
    lv_style_set_border_side(&pp_style_tabbar, LV_BORDER_SIDE_RIGHT);
    lv_style_set_border_color(&pp_style_tabbar, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_tabbar, 33);
    lv_style_set_border_width(&pp_style_tabbar, 1);
    lv_style_set_pad_ver(&pp_style_tabbar, PP_SCALE(16));
    lv_style_set_radius(&pp_style_tabbar, 0);

    lv_style_init(&pp_style_tab);
    lv_style_set_bg_opa(&pp_style_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&pp_style_tab, c_text);
    lv_style_set_text_opa(&pp_style_tab, 128);
    lv_style_set_text_font(&pp_style_tab, pp_font_med_sm());
    lv_style_set_pad_ver(&pp_style_tab, PP_SCALE(12));
    lv_style_set_radius(&pp_style_tab, 0);
    lv_style_set_border_width(&pp_style_tab, 0);

    lv_style_init(&pp_style_tab_active);
    lv_style_set_text_color(&pp_style_tab_active, c_accent);
    lv_style_set_text_opa(&pp_style_tab_active, LV_OPA_COVER);
    lv_style_set_bg_opa(&pp_style_tab_active, LV_OPA_TRANSP);
    lv_style_set_border_side(&pp_style_tab_active, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&pp_style_tab_active, c_accent);
    lv_style_set_border_opa(&pp_style_tab_active, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_tab_active, PP_SCALE(3));

    lv_style_init(&pp_style_section_hdr);
    lv_style_set_text_color(&pp_style_section_hdr, c_text);
    lv_style_set_text_opa(&pp_style_section_hdr, 102);
    lv_style_set_text_font(&pp_style_section_hdr, pp_font_med_sm());
    lv_style_set_text_letter_space(&pp_style_section_hdr, PP_SCALE(2));
    lv_style_set_pad_top(&pp_style_section_hdr, PP_SCALE(8));
    lv_style_set_pad_left(&pp_style_section_hdr, PP_SCALE(20));
    lv_style_set_pad_bottom(&pp_style_section_hdr, PP_SCALE(4));

    lv_style_init(&pp_style_row);
    lv_style_set_bg_opa(&pp_style_row, LV_OPA_TRANSP);
    lv_style_set_pad_hor(&pp_style_row, PP_SCALE(20));
    lv_style_set_pad_ver(&pp_style_row, PP_SCALE(8));
    lv_style_set_text_color(&pp_style_row, c_text);
    lv_style_set_text_font(&pp_style_row, pp_font_med_md());
    lv_style_set_border_side(&pp_style_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&pp_style_row, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&pp_style_row, 33);
    lv_style_set_border_width(&pp_style_row, 1);
    lv_style_set_radius(&pp_style_row, 0);

    lv_style_init(&pp_style_row_focus);
    lv_style_set_bg_color(&pp_style_row_focus, lv_color_hex(PP_C_INK));
    lv_style_set_bg_opa(&pp_style_row_focus, 18);   /* ~7% white */
    lv_style_set_border_side(&pp_style_row_focus, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&pp_style_row_focus, c_accent);
    lv_style_set_border_opa(&pp_style_row_focus, LV_OPA_COVER);
    lv_style_set_border_width(&pp_style_row_focus, PP_SCALE(2));
    lv_style_set_pad_left(&pp_style_row_focus, PP_SCALE(18));

    lv_style_init(&pp_style_value_focus);
    lv_style_set_text_color(&pp_style_value_focus, c_accent);

    lv_style_init(&pp_style_divider);
    lv_style_set_border_width(&pp_style_divider, 0);

    lv_style_init(&pp_style_switch_on);
    lv_style_set_bg_color(&pp_style_switch_on, c_accent);

    return 0;
}
