#include "pp_dropdown.h"
#include "pp_toast.h"
#include "pp_row.h"
#include "../styles.h"
#include "../settings.h"
#include "../../input.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Scaled menu-row text font (same accessor pp_style_row uses). Declared
 * extern here per the pp_drilldown.c pattern so the popup option labels
 * render at the 1.5x-scaled size instead of the 14px LVGL default. */
extern const lv_font_t *pp_font_med_md(void);

typedef struct pp_dd_data pp_dd_data_t;

struct pp_dd_data {
    char *domain, *page, *key;
    char *label;
    lv_obj_t *dd, *value_label, *row;
    lv_obj_t *popup;            /* floating options list while in EDIT */
    uint16_t saved_sel;
    bool      in_flight;
};

struct dropdown_ctx {
    pp_dd_data_t *d;
    uint16_t      target_sel;
};

static void dropdown_done_cb(int rc, const char *err, void *user_data) {
    struct dropdown_ctx *ctx = (struct dropdown_ctx *)user_data;
    pp_dd_data_t *d = ctx->d;
    pp_row_set_busy(d->row, false);
    d->in_flight = false;
    if (rc == 0) {
        lv_dropdown_set_selected(d->dd, ctx->target_sel);
        /* refresh the value label using the existing helper. */
        char buf[64];
        lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
        lv_label_set_text(d->value_label, buf);
    } else {
        pp_toast_error(err ? err : "Failed to apply dropdown");
        /* Selection already at saved_sel — we never moved it. */
    }
    lv_free(ctx);
}

static void popup_close(pp_dd_data_t *d);

static void on_delete(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    if (d) {
        popup_close(d);
        free(d->domain); free(d->page); free(d->key); free(d->label); free(d);
    }
}

static void refresh_label(pp_dd_data_t *d) {
    char buf[64];
    lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
    lv_label_set_text(d->value_label, buf);
}

static void popup_open(pp_dd_data_t *d) {
    if (d->popup) return;

    /* Parent the modal box directly to the active screen — NO full-screen
     * translucent backdrop. A translucent (NOT_COVER) full-screen dim forces
     * the single-threaded software renderer on the RK3566 GS to recomposite
     * the entire frame on open (scrim + every panel row's tiny_ttf glyphs +
     * the blend) — that was the ~1-2 s dropdown-open latency. The page scrim
     * already dims the menu, and this box is opaque, so only the box's own
     * area redraws. Active screen (not lv_layer_top) so lv_snapshot_take()
     * still captures it for the screenshot harness; the error toast stays on
     * lv_layer_top so it renders above this box. move_foreground puts the box
     * above the panel. */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(PP_C_MODAL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(PP_C_ACCENT), 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_pad_all(p, PP_SCALE(6), 0);
    lv_obj_set_style_text_font(p, pp_font_med_md(), 0);
    lv_obj_set_width(p, PP_SCALE(300));
    lv_obj_set_style_max_height(p, lv_display_get_vertical_resolution(NULL) - 160, 0);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_center(p);
    lv_obj_move_foreground(p);   /* render above the panel */

    /* Header: amber marker + uppercase label. */
    lv_obj_t *hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, PP_SCALE(8), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mark = lv_obj_create(hdr);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, PP_SCALE(4), PP_SCALE(18));
    lv_obj_set_style_bg_color(mark, lv_color_hex(PP_C_ACCENT), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_right(mark, PP_SCALE(8), 0);
    lv_obj_t *htxt = lv_label_create(hdr);
    {
        char up[64]; size_t n = 0;
        for (const char *s = d->label ? d->label : ""; *s && n < sizeof(up)-1; ++s, ++n)
            up[n] = (char)toupper((unsigned char)*s);
        up[n] = '\0';
        lv_label_set_text(htxt, up);
    }
    lv_obj_set_style_text_font(htxt, pp_font_xb_md(), 0);
    lv_obj_set_style_text_color(htxt, lv_color_hex(PP_C_INK), 0);

    uint16_t cur   = lv_dropdown_get_selected(d->dd);
    uint16_t saved = d->saved_sel;
    uint16_t n     = lv_dropdown_get_option_count(d->dd);
    for (uint16_t i = 0; i < n; i++) {
        char buf[64];
        lv_dropdown_set_selected(d->dd, i);
        lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);

        lv_obj_t *item = lv_obj_create(p);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(item, PP_SCALE(12), 0);
        lv_obj_set_style_pad_ver(item, PP_SCALE(6), 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        if (i == cur) {
            lv_obj_set_style_bg_color(item, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_bg_opa(item, 26, 0);                  /* ~10% */
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_border_opa(item, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(item, PP_SCALE(3), 0);
        }
        lv_obj_t *l = lv_label_create(item);
        lv_label_set_text(l, buf);
        lv_obj_set_flex_grow(l, 1);
        if (i == cur) {
            lv_obj_set_style_text_color(l, lv_color_hex(PP_C_ACCENT), 0);
            lv_obj_set_style_text_font(l, pp_font_xb_md(), 0);
        } else {
            lv_obj_set_style_text_color(l, lv_color_hex(PP_C_INK), 0);
            lv_obj_set_style_text_opa(l, 200, 0);
        }
        if (i == saved) {
            lv_obj_t *tag = lv_label_create(item);
            lv_label_set_text(tag, "CURRENT");
            lv_obj_set_style_text_font(tag, pp_font_med_sm(), 0);
            lv_obj_set_style_text_color(tag, lv_color_hex(PP_C_INK), 0);
            lv_obj_set_style_text_opa(tag, 102, 0);                /* ~40% */
            lv_obj_set_style_text_letter_space(tag, PP_SCALE(2), 0);
        }
    }
    lv_dropdown_set_selected(d->dd, cur);

    if (cur + 1 < lv_obj_get_child_cnt(p)) {
        lv_obj_t *cur_item = lv_obj_get_child(p, cur + 1); /* +1 for header */
        if (cur_item) lv_obj_scroll_to_view(cur_item, LV_ANIM_OFF);
    }
    d->popup = p;   /* the box itself; popup_close deletes it */
}

static void popup_refresh(pp_dd_data_t *d) {
    if (!d->popup) return;
    lv_obj_t *box = d->popup;                          /* d->popup is the box itself now */
    uint16_t cur = lv_dropdown_get_selected(d->dd);
    uint32_t cnt = lv_obj_get_child_cnt(box);
    for (uint32_t i = 1; i < cnt; i++) {               /* i=0 is header */
        lv_obj_t *item = lv_obj_get_child(box, i);
        lv_obj_t *lbl  = lv_obj_get_child(item, 0);
        bool on = (i - 1) == cur;
        lv_obj_set_style_bg_opa(item, on ? 26 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, on ? PP_SCALE(3) : 0, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(on ? PP_C_ACCENT : PP_C_INK), 0);
            lv_obj_set_style_text_opa(lbl, on ? LV_OPA_COVER : 200, 0);
            lv_obj_set_style_text_font(lbl, on ? pp_font_xb_md() : pp_font_med_md(), 0);
        }
        if (on) lv_obj_scroll_to_view(item, LV_ANIM_ON);
    }
}

static void popup_close(pp_dd_data_t *d) {
    if (!d->popup) return;
    lv_obj_del(d->popup);
    d->popup = NULL;
}

/* Re-read the value from the provider — sent (LV_EVENT_REFRESH) when the
 * row transitions out of an offline/unavailable lock, i.e. when fresh data
 * first becomes readable for a row that was built without one. Same
 * option-index lookup + label refresh as the constructor's initial read. */
static void on_refresh(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    if (d->in_flight) return;          /* a write is pending — don't clobber */
    char *v = pp_settings_get(d->domain, d->page, d->key);
    if (v && *v) {
        uint16_t prev = lv_dropdown_get_selected(d->dd);
        uint16_t n = lv_dropdown_get_option_count(d->dd);
        char buf[64];
        bool matched = false;
        for (uint16_t i = 0; i < n; i++) {
            lv_dropdown_set_selected(d->dd, i);
            lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
            if (strcmp(buf, v) == 0) { matched = true; break; }
        }
        if (matched) refresh_label(d);
        else         lv_dropdown_set_selected(d->dd, prev);  /* keep what we had */
    }
    free(v);
}

static void on_key(lv_event_t *e) {
    pp_dd_data_t *d = lv_event_get_user_data(e);
    lv_key_t k = lv_event_get_key(e);
    extern gsmenu_control_mode_t control_mode;
    bool consumed = false;
    if (k == LV_KEY_ENTER) {
        if (d->in_flight) {
            consumed = true;
        } else if (control_mode == GSMENU_CONTROL_MODE_NAV) {
            if (pp_row_get_locked(d->row) != PP_ROW_UNLOCKED) {
                pp_toast_error("Locked by Dynamic Link");
                consumed = true;
            } else {
                d->saved_sel = lv_dropdown_get_selected(d->dd);
                control_mode = GSMENU_CONTROL_MODE_EDIT;
                popup_open(d);
                consumed = true;
            }
        } else {
            control_mode = GSMENU_CONTROL_MODE_NAV;
            uint16_t attempted = lv_dropdown_get_selected(d->dd);
            popup_close(d);
            if (attempted != d->saved_sel) {
                char buf[64];
                lv_dropdown_get_selected_str(d->dd, buf, sizeof buf);
                /* Revert visible selection to saved_sel; callback flips
                 * to attempted on success. */
                lv_dropdown_set_selected(d->dd, d->saved_sel);
                refresh_label(d);
                d->in_flight = true;
                pp_row_set_busy(d->row, true);
                struct dropdown_ctx *ctx = lv_malloc(sizeof(*ctx));
                ctx->d          = d;
                ctx->target_sel = attempted;
                pp_settings_set_async(d->domain, d->page, d->key, buf,
                                      dropdown_done_cb, ctx);
            }
            consumed = true;
        }
    } else if (k == LV_KEY_UP) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint16_t s = lv_dropdown_get_selected(d->dd);
            if (s > 0) lv_dropdown_set_selected(d->dd, s - 1);
            refresh_label(d);
            popup_refresh(d);
            consumed = true;
        }
    } else if (k == LV_KEY_DOWN) {
        if (control_mode == GSMENU_CONTROL_MODE_EDIT) {
            uint16_t s = lv_dropdown_get_selected(d->dd);
            if (s + 1 < lv_dropdown_get_option_count(d->dd))
                lv_dropdown_set_selected(d->dd, s + 1);
            refresh_label(d);
            popup_refresh(d);
            consumed = true;
        }
    } else if (k == LV_KEY_ESC) {
        lv_dropdown_set_selected(d->dd, d->saved_sel);
        refresh_label(d);
        popup_close(d);
        control_mode = GSMENU_CONTROL_MODE_NAV;
        consumed = true;
    }
    /* Same scroll-bubbling guard as pp_slider — only stop bubbling for
     * keys we consumed in EDIT mode. In NAV mode, UP/DOWN go to the
     * page group's focus traversal (which is what we want for row nav). */
    if (consumed) lv_event_stop_bubbling(e);
}

lv_obj_t *pp_dropdown(lv_obj_t *parent_page,
                     const char *icon_text, const char *label,
                     const char *domain, const char *page, const char *key,
                     const char *options) {
    lv_obj_t *row = lv_obj_create(parent_page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &pp_style_row, 0);
    lv_obj_add_style(row, &pp_style_row_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, PP_SCALE(36));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    (void)icon_text;   /* OSD reskin: rows are label + value only (no leading icon) */

    lv_obj_t *label_obj = lv_label_create(row);
    lv_label_set_text(label_obj, label);
    lv_obj_set_style_min_width(label_obj, 200, 0);
    lv_obj_set_style_pad_right(label_obj, PP_SCALE(16), 0);
    lv_obj_set_flex_grow(label_obj, 1);

    lv_obj_t *value_label = lv_label_create(row);
    lv_obj_set_style_text_font(value_label, pp_font_xb_md(), 0);
    lv_label_set_text(value_label, "—");
    lv_obj_set_style_pad_right(value_label, PP_SCALE(8), 0);

    /* Hidden dropdown — we drive it with keys, not its own popup.
     * Our own popup (popup_open) renders all options visibly. */
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);
    /* lv_dropdown is group_def=TRUE: if a default group is set, LVGL
     * auto-added it there, making it key-focusable. A stray ENTER would
     * then open its unstyled native list over whatever screen is shown. */
    lv_group_remove_obj(dd);

    pp_dd_data_t *d = calloc(1, sizeof(*d));
    d->domain = strdup(domain);
    d->page   = strdup(page);
    d->key    = strdup(key);
    d->label  = strdup(label);
    d->dd     = dd;
    d->value_label = value_label;
    d->row    = row;
    lv_obj_set_user_data(row, d);
    lv_obj_add_event_cb(row, on_delete, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(row, on_key,    LV_EVENT_KEY,    d);
    lv_obj_add_event_cb(row, on_refresh, LV_EVENT_REFRESH, d);

    char *v = pp_settings_get(domain, page, key);
    if (v && *v) {
        uint16_t n = lv_dropdown_get_option_count(dd);
        char buf[64];
        bool matched = false;
        for (uint16_t i = 0; i < n; i++) {
            lv_dropdown_set_selected(dd, i);
            lv_dropdown_get_selected_str(dd, buf, sizeof buf);
            if (strcmp(buf, v) == 0) { matched = true; break; }
        }
        if (matched) {
            refresh_label(d);
        } else {
            lv_dropdown_set_selected(dd, 0);
            /* keep placeholder em-dash in the label */
        }
    }
    free(v);

    if (!pp_settings_is_available(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_UNAVAILABLE);
    } else if (pp_settings_is_locked(domain, page, key)) {
        pp_row_set_locked(row, PP_ROW_LOCKED_DYNAMIC);
    }

    return row;
}
