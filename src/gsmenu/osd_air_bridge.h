#ifndef OSD_AIR_BRIDGE_H
#define OSD_AIR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers a settings snapshot listener that republishes the configured air
 * video resolution + fps (sourced from fpvd GET /air/config via the settings
 * snapshot) as OSD facts "air.video.resolution" (string) and "air.video.fps"
 * (int). Call once at startup, AFTER pp_settings_register_fpvd(). */
void pp_osd_air_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif /* OSD_AIR_BRIDGE_H */
