#include "pp_section_header.h"
#include "../styles.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

lv_obj_t *pp_section_header(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &pp_style_section_hdr, 0);

    /* Increase left padding to make room for the tick bar. */
    lv_obj_set_style_pad_left(label, PP_SCALE(28), 0);

    /* Uppercase the text. */
    size_t n = strlen(text);
    char *upper = malloc(n + 1);
    for (size_t i = 0; i < n; i++) upper[i] = (char)toupper((unsigned char)text[i]);
    upper[n] = '\0';
    lv_label_set_text(label, upper);
    free(upper);

    lv_obj_set_width(label, LV_PCT(100));

    /* Leading tick — a child of the LABEL (not the page), so the page's
     * focus-group loop never sees it and the header stays non-focusable. */
    lv_obj_t *tick = lv_obj_create(label);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, PP_SCALE(16), PP_SCALE(3));
    lv_obj_set_style_bg_color(tick, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_bg_opa(tick, 102, 0);            /* ~40% */
    lv_obj_set_style_radius(tick, 1, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    /* Sit in the left padding, vertically centered. Negative x pulls it into
     * the pad area ahead of the text; offset positions it just left of the
     * first glyph without overlapping it. */
    lv_obj_align(tick, LV_ALIGN_LEFT_MID, -PP_SCALE(24), 0);
    return label;
}
