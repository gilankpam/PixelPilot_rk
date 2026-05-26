#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

extern lv_style_t style_rootmenu;
extern lv_style_t style_openipc;
extern lv_style_t style_openipc_dropdown;
extern lv_style_t style_openipc_outline;
extern lv_style_t style_openipc_textcolor;
extern lv_style_t style_openipc_disabled;
extern lv_style_t style_openipc_section;
extern lv_style_t style_openipc_dark_background;
extern lv_style_t style_openipc_lightdark_background;

int style_init(void);

/* New widget style tokens (Visual spec in 2026-05-27 design). */
extern lv_style_t pp_style_panel;        /* main panel bg */
extern lv_style_t pp_style_tabbar;       /* tab strip bg */
extern lv_style_t pp_style_tab;          /* tab item base */
extern lv_style_t pp_style_tab_active;   /* tab item active */
extern lv_style_t pp_style_section_hdr;  /* uppercase tracked label */
extern lv_style_t pp_style_row;          /* row base */
extern lv_style_t pp_style_row_focus;    /* row when focused */
extern lv_style_t pp_style_value_focus;  /* row value color when focused */
extern lv_style_t pp_style_divider;      /* row bottom border */
extern lv_style_t pp_style_switch_on;    /* lv_switch indicator on-color */

#ifdef __cplusplus
}
#endif
