#ifndef PP_DROPDOWN_H
#define PP_DROPDOWN_H
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
/* options: newline-separated list, e.g. "1080p60\n720p120\n540p60" */
lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                      const char *icon_text, const char *label,
                      const char *domain, const char *page, const char *key,
                      const char *options);

/* Like pp_dropdown, but options display with a unit suffix scaled by disp_div
 * (display = atoi(option)/disp_div + " " + unit), while the stored/matched
 * value stays the raw option. e.g. options "4000\n8000", unit "Mbps",
 * disp_div 1000 -> shows "4 Mbps" / "8 Mbps", stores "8000". */
lv_obj_t *pp_dropdown_units(lv_obj_t *parent_page,
                      const char *icon_text, const char *label,
                      const char *domain, const char *page, const char *key,
                      const char *options, const char *unit, int disp_div);
#ifdef __cplusplus
}
#endif
#endif
