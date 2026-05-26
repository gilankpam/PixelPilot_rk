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
#include "gsmenu/air_actions.h"
#include "gsmenu/gs_actions.h"

void dispatch_input_char(char c);

// SDL event watch: convert keypresses in the LVGL window into the same
// W/A/S/D/Enter/Q/T chars the stdin handler uses. Returning 1 keeps the
// event in the queue so LVGL/SDL still process window close, resize, etc.
static int sdl_key_watch(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (event->type != SDL_KEYDOWN) return 1;
    if (event->key.repeat) return 1;
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

MenuAction airactions[MAX_ACTIONS];
size_t airactions_count = 0;
MenuAction gsactions[MAX_ACTIONS];
size_t gsactions_count = 0;

bool restream_get_enabled(void)                       { return false; }
void restream_scan_clients(char *buf, size_t buf_len) { if (buf && buf_len) buf[0] = '\0'; }
const char *restream_get_manual_ip(void)              { return ""; }

int dvr_get_mode(void)          { return 0; }
int dvr_reenc_get_osd(void)     { return 0; }
int dvr_reenc_get_fps(void)     { return 30; }
int dvr_reenc_get_bitrate(void) { return 8000; }
int dvr_reenc_get_codec(void)   { return 0; }
int dvr_reenc_get_resolution(void) { return 1; }
int dvr_get_max_size(void)      { return 4000; }
void my_log_cb(lv_log_level_t level, const char * buf)
{
  printf("%s",buf);
}

int main(int argc, char **argv)
{
    lv_log_register_print_cb(my_log_cb);
    lv_init();
    lv_disp_t * disp = lv_sdl_window_create(1920,1080);
    SDL_AddEventWatch(sdl_key_watch, NULL);

    lv_obj_t * bottom = lv_display_get_layer_bottom(disp);
    lv_obj_t *obj = lv_img_create(bottom);
    lv_image_set_src(obj, find_resource_file("osd-bg-2.png"));
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);

    pp_menu_main();
    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }

}