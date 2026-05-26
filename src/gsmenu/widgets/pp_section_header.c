#include "pp_section_header.h"
#include "../styles.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

lv_obj_t *pp_section_header(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &pp_style_section_hdr, 0);

    /* Uppercase the text. */
    size_t n = strlen(text);
    char *upper = malloc(n + 1);
    for (size_t i = 0; i < n; i++) upper[i] = (char)toupper((unsigned char)text[i]);
    upper[n] = '\0';
    lv_label_set_text(label, upper);
    free(upper);

    lv_obj_set_width(label, LV_PCT(100));
    return label;
}
