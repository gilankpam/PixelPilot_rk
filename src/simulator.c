#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <SDL.h>
#include "lvgl/lvgl.h"
#include "menu.h"
#include "input.h"
#include "gsmenu/helper.h"
#include "gsmenu/settings.h"
#include <png.h>
#include <stdlib.h>

/* defined in menu.cpp — used by the screenshot harness below */
extern lv_obj_t *pp_menu_screen;

static int sim_write_png(const char *path, const uint8_t *argb,
                         uint32_t w, uint32_t h, uint32_t stride) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); unlink(path); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); unlink(path); return -1; }
    uint8_t *row = NULL;
    if (setjmp(png_jmpbuf(png))) {
        free(row);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        unlink(path);
        return -1;
    }
    row = (uint8_t *)malloc((size_t)w * 4);
    if (!row) { png_destroy_write_struct(&png, &info); fclose(fp); unlink(path); return -1; }
    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
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

void dispatch_input_char(char c);

// SDL event watch: convert keypresses in the LVGL window into the same
// W/A/S/D/Enter/Q/T chars the stdin handler uses. Returning 1 keeps the
// event in the queue so LVGL/SDL still process window close, resize, etc.
static int sdl_key_watch(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (event->type != SDL_KEYDOWN) return 1;
    /* Let OS key-autorepeat through so holding W/S accelerates sliders
     * (and rapid-scrolls focus in NAV mode). */
    char c = 0;
    switch (event->key.keysym.sym) {
        case SDLK_w: case SDLK_UP:     c = 'w'; break;
        case SDLK_s: case SDLK_DOWN:   c = 's'; break;
        case SDLK_a: case SDLK_LEFT:   c = 'a'; break;
        case SDLK_d: case SDLK_RIGHT:  c = 'd'; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: c = '\n'; break;
        case SDLK_t: c = 't'; break;
        case SDLK_q: case SDLK_ESCAPE: c = 'q'; break;
        default: return 1;
    }
    dispatch_input_char(c);
    return 1;
}


int dvr_enabled = 0;
uint64_t gtotal_tunnel_data = 0;
bool disable_vsync = false;
const char *dvr_template = "/tmp/record_%Y-%m-%d_%H-%M-%S.mp4";

// Stubs for symbols defined in main.cpp / dvr.cpp
bool enable_live_colortrans = false;
float live_colortrans_gain = 2.5f;
float live_colortrans_offset = -0.15f;

bool restream_get_enabled(void)                       { return false; }
void restream_scan_clients(char *buf, size_t buf_len) { if (buf && buf_len) buf[0] = '\0'; }
const char *restream_get_manual_ip(void)              { return ""; }

int dvr_get_mode(void)          { return 0; }
int dvr_reenc_get_bitrate(void) { return 8000; }
int dvr_get_max_size(void)      { return 4000; }
void my_log_cb(lv_log_level_t level, const char * buf)
{
  printf("%s",buf);
}

int main(int argc, char **argv)
{
    pp_settings_register_dummy();
    lv_log_register_print_cb(my_log_cb);
    lv_init();
    /* Default to the hardware OSD resolution. Dev machines with a smaller
     * logical desktop (e.g. macOS Retina at 2x) can override via env vars;
     * LVGL renders directly at the requested size, so output stays crisp
     * (no SDL downscaler). Layouts won't be pixel-identical to hardware at
     * non-default sizes, so use 1920x1080 for final visual sign-off. */
    int sim_w = 1920, sim_h = 1080;
    const char * w_env = getenv("PP_SIM_WIDTH");
    const char * h_env = getenv("PP_SIM_HEIGHT");
    if (w_env && *w_env) sim_w = atoi(w_env);
    if (h_env && *h_env) sim_h = atoi(h_env);
    lv_disp_t * disp = lv_sdl_window_create(sim_w, sim_h);
    SDL_AddEventWatch(sdl_key_watch, NULL);

    lv_obj_t * bottom = lv_display_get_layer_bottom(disp);
    lv_obj_t *obj = lv_img_create(bottom);
    lv_image_set_src(obj, find_resource_file("osd-bg-2.png"));
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);

    extern void pp_widget_demo_main(void);
    if (getenv("PP_WIDGET_DEMO")) {
        pp_widget_demo_main();
        while (1) { handle_keyboard_input(); lv_task_handler(); usleep(5000); }
    }

    pp_menu_main();

    const char *shot = getenv("PP_SIM_SHOT");
    if (shot) {
        /* settle counts: ~20 x 8ms frames, enough for layout + the 120ms page fade */
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
        int rc = -1;
        if (snap) {
            rc = sim_write_png(shot, snap->data, snap->header.w, snap->header.h, snap->header.stride);
            lv_draw_buf_destroy(snap);
        }
        return rc;   /* non-zero => screenshot failed (visible to CI) */
    }

    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }

}