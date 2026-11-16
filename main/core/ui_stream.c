// Widgets of the Streaming theme. See ui_stream.h.

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "ui_stream.h"
#include "os.h"

bool ui_stream(void)
{
    return ui_theme()->id == UI_THEME_STREAM;
}

char *ui_stream_caps(char *out, size_t n, const char *in)
{
    size_t i = 0;
    for (; in && in[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        // The display face has 0x20..0x5F and '|': anything else is a space.
        c = (unsigned char)toupper(c);
        out[i] = (c >= 0x20 && c <= 0x5F) || c == '|' ? (char)c : ' ';
    }
    if (n) {
        out[i] = 0;
    }
    return out;
}

lv_obj_t *ui_stream_caps_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &stream_caps_8, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xD6D6DC), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_stream_display(lv_obj_t *parent, int px, const char *text)
{
    const lv_font_t *f = px >= 64 ? &stream_disp_64 : px >= 30 ? &stream_disp_30 : px >= 24 ? &stream_disp_24 : &stream_disp_14;
    char caps[64];
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, ui_col(COL_TEXT), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, ui_stream_caps(caps, sizeof(caps), text));
    return l;
}

void ui_stream_ring(lv_obj_t *obj, bool on)
{
    lv_obj_set_style_outline_width(obj, on ? 2 : 0, 0);
    lv_obj_set_style_outline_pad(obj, 1, 0);
    lv_obj_set_style_outline_color(obj, lv_color_white(), 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
}

static lv_obj_t *chip(lv_obj_t *parent, int h, int radius)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 5, 0);
    lv_obj_set_scrollable(b, false);
    return b;
}

lv_obj_t *ui_stream_button(lv_obj_t *parent, const char *symbol, const char *text, bool primary)
{
    lv_obj_t *b = chip(parent, 20, 4);
    lv_color_t ink = primary ? lv_color_hex(0x0B0B0D) : lv_color_white();
    lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, primary ? lv_color_white() : lv_color_hex(0x787880), 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    if (symbol) {
        lv_obj_t *s = lv_label_create(b);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s, ink, 0);
        lv_label_set_text(s, symbol);
        lv_obj_set_style_pad_left(b, 8, 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &stream_bold_11, 0);
    lv_obj_set_style_text_color(l, ink, 0);
    lv_label_set_text(l, text);
    return b;
}

lv_obj_t *ui_stream_pill(lv_obj_t *parent, const char *text, bool on, uint32_t dot)
{
    lv_obj_t *b = chip(parent, 18, 9);
    lv_obj_set_style_pad_hor(b, 9, 0);
    if (on) {
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    } else {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_opa(b, LV_OPA_40, 0);
    }
    if (dot) {
        lv_obj_t *d = lv_obj_create(b);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 5, 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, ui_col(dot), 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &stream_caps_8, 0);
    lv_obj_set_style_text_color(l, on ? lv_color_hex(0x0B0B0D) : lv_color_white(), 0);
    char caps[32];
    lv_label_set_text(l, ui_stream_caps(caps, sizeof(caps), text));
    lv_obj_set_style_pad_hor(b, 6, 0);
    return b;
}

lv_obj_t *ui_stream_glow(lv_obj_t *parent, int cx, int cy, int w, int h, uint32_t color)
{
    // Three ellipses of falling opacity stand in for a radial gradient: no
    // image, and cheap for the software renderer.
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_pos(g, cx - w / 2, cy - h / 2);
    lv_obj_set_size(g, w, h);
    lv_obj_set_scrollable(g, false);
    static const struct {
        uint8_t pct;
        lv_opa_t opa;
    } rings[] = {{100, 14}, {78, 18}, {56, 22}};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *e = lv_obj_create(g);
        lv_obj_remove_style_all(e);
        lv_obj_set_size(e, w * rings[i].pct / 100, h * rings[i].pct / 100);
        lv_obj_center(e);
        lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(e, rings[i].opa, 0);
        lv_obj_set_style_bg_color(e, lv_color_hex(color), 0);
    }
    return g;
}

lv_obj_t *ui_stream_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *p = chip(parent, 20, 10);
    lv_obj_set_style_bg_opa(p, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x787880), 0);
    lv_obj_set_style_pad_hor(p, 10, 0);
    lv_obj_t *i = lv_label_create(p);
    lv_obj_set_style_text_font(i, &stream_bold_11, 0);
    lv_obj_set_style_text_color(i, lv_color_white(), 0);
    lv_label_set_text(i, "i");
    lv_obj_set_style_border_width(i, 1, 0);
    lv_obj_set_style_border_color(i, lv_color_white(), 0);
    lv_obj_set_style_radius(i, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(i, 4, 0);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &pearl_font_11, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    return p;
}

lv_obj_t *ui_stream_empty_hint(lv_obj_t *parent)
{
    const os_app_t *app = os_current();
    const char *text = !app ? NULL
                     : !strcmp(app->id, "quests") ? "Talk to a character to start a quest"
                     : !strcmp(app->id, "xdex") ? "Hold two badges close and swap cards"
                     : !strcmp(app->id, "trade") ? "Lock, then sign once they lock too" : NULL;
    return text ? ui_stream_hint(parent, text) : NULL;
}

lv_obj_t *ui_stream_empty(lv_obj_t *parent, int y, const char *headline, const char *line, const char *hint)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 10, y);
    lv_obj_set_size(col, OS_ROOT_W - 20, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_scrollable(col, false);
    if (headline) {
        ui_stream_display(col, 24, headline);
    }
    if (line) {
        lv_obj_t *l = ui_label(col, &pearl_font_11, COL_DIM, line);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_max_width(l, OS_ROOT_W - 24, 0);
    }
    if (hint) {
        lv_obj_set_style_margin_top(ui_stream_hint(col, hint), 6, 0);
    }
    return col;
}

lv_obj_t *ui_stream_row(lv_obj_t *parent, int y, const char *label, const char *value, uint32_t value_color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, 14, y);
    lv_obj_set_size(r, OS_ROOT_W - 28, 27);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x2A2A2F), 0);
    lv_obj_set_scrollable(r, false);
    lv_obj_t *l = ui_label(r, &pearl_font_11, COL_DIM, label);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = ui_label(r, &stream_bold_11, value_color ? value_color : COL_TEXT, value);
    lv_obj_set_style_max_width(v, 200, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(v, lv_font_get_line_height(&stream_bold_11));
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

lv_obj_t *ui_stream_empty_art(lv_obj_t *parent)
{
    const os_app_t *app = os_current();
    const lv_image_dsc_t *art = !app ? NULL
                              : !strcmp(app->id, "quests") ? &img_stream_radar
                              : !strcmp(app->id, "xdex") ? &img_stream_cards
                              : !strcmp(app->id, "trade") ? &img_stream_orb : NULL;
    if (!art) {
        return NULL;
    }
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, art);
    return img;
}

lv_obj_t *ui_stream_poster(lv_obj_t *parent, const char *name, const char *symbol, const void *icon, uint32_t accent, bool art)
{
    // No app has artwork of its own yet: `art` is where a store app's would go.
    (void)art;
    const lv_image_dsc_t *img = NULL;
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, UI_POSTER_W, UI_POSTER_H);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_scrollable(p, false);
    if (img) {
        lv_obj_t *i = lv_image_create(p);
        lv_image_set_src(i, img);
        return p;
    }
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, lv_color_mix(lv_color_hex(accent), lv_color_hex(0x0B0B0D), 110), 0);
    lv_obj_set_style_bg_grad_color(p, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_t *ic;
    if (symbol || !icon) {
        ic = lv_label_create(p);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
        lv_label_set_text(ic, symbol ? symbol : LV_SYMBOL_PLAY);
    } else {
        ic = lv_image_create(p);
        lv_image_set_src(ic, icon);
    }
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 9);
    char caps[24];
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &stream_disp_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, -2, 0);
    lv_obj_set_width(l, UI_POSTER_W - 4);
    lv_obj_set_style_max_height(l, 28, 0);
    lv_label_set_text(l, ui_stream_caps(caps, sizeof(caps), name));
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -3);
    return p;
}
