#include "pp_footer.h"
#include "../styles.h"

/* One [chip] LABEL group. `glyph` is an LV_SYMBOL string (Montserrat
 * fallback); `text` is the uppercase action label. */
static void add_item(lv_obj_t *bar, const char *glyph, const char *text) {
    lv_obj_t *chip = lv_label_create(bar);
    lv_label_set_text(chip, glyph);
    lv_obj_set_style_text_font(chip, pp_font_xb_md(), 0);
    lv_obj_set_style_text_color(chip, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, PP_SCALE(6), 0);
    lv_obj_set_style_pad_ver(chip, PP_SCALE(2), 0);
    lv_obj_set_style_radius(chip, PP_SCALE(5), 0);
    lv_obj_set_style_border_width(chip, 2, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(chip, 77, 0);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, pp_font_med_sm(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_text_opa(lbl, 153, 0);            /* ~60% */
    lv_obj_set_style_text_letter_space(lbl, PP_SCALE(2), 0);
    lv_obj_set_style_pad_right(lbl, PP_SCALE(22), 0);  /* gap before next group */
    lv_obj_set_style_pad_left(lbl, PP_SCALE(8), 0);
}

lv_obj_t *pp_footer_create(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, 46, 0);               /* ~18% */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(PP_C_INK), 0);
    lv_obj_set_style_border_opa(bar, 20, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_hor(bar, PP_SCALE(20), 0);
    lv_obj_set_style_pad_ver(bar, PP_SCALE(10), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    add_item(bar, LV_SYMBOL_UP LV_SYMBOL_DOWN, "NAVIGATE");
    add_item(bar, LV_SYMBOL_RIGHT, "ENTER");
    add_item(bar, LV_SYMBOL_LEFT, "BACK");
    return bar;
}
