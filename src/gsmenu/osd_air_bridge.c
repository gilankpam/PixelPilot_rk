#include "osd_air_bridge.h"
#include "settings.h"   /* pp_settings_get, pp_settings_set_snapshot_listener */
#include "../osd.h"     /* osd_publish_str_fact, osd_publish_int_fact */

#include <stdlib.h>     /* free, atoi */

/* Read the configured air resolution + fps from the current settings snapshot
 * and (re)publish them as OSD facts. pp_settings_get returns a heap string the
 * caller must free(); NULL/empty means "not available yet" -> skip that fact. */
static void publish_air_video(void *ud) {
    (void)ud;
    char *res = pp_settings_get("air", "camera", "size"); /* e.g. "1920x1080" */
    char *fps = pp_settings_get("air", "camera", "fps");  /* e.g. "60" */
    if (res && res[0]) {
        osd_publish_str_fact("air.video.resolution", NULL, 0, res);
    }
    if (fps && fps[0]) {
        osd_publish_int_fact("air.video.fps", NULL, 0, (long)atoi(fps));
    }
    free(res);
    free(fps);
}

void pp_osd_air_bridge_init(void) {
    /* Fires on every snapshot refresh (boot, periodic poll, and post-apply). */
    pp_settings_set_snapshot_listener(publish_air_video, NULL);
    /* Publish once now in case a snapshot already arrived before registration. */
    publish_air_video(NULL);
}
