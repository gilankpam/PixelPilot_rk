/* src/gsmenu/settings_gs_enum.h */
#ifndef PP_SETTINGS_GS_ENUM_H
#define PP_SETTINGS_GS_ENUM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Both return newline-joined strings (heap, caller free) or NULL on parse fail.
 * The parser functions are pure (no popen) so they can be tested with canned
 * inputs. */

char *pp_gs_parse_iw_list_channels(const char *iw_list_output);
char *pp_gs_parse_drm_info_modes (const char *drm_info_json);

/* Run the underlying binaries and parse. Binary paths overridable via
 * PP_GS_IW_BIN and PP_GS_DRM_INFO_BIN. Returns same as the parse functions. */
char *pp_gs_enum_channels(void);
char *pp_gs_enum_hdmi_modes(void);

#ifdef __cplusplus
}
#endif
#endif
