// Home layout: a retro handheld RPG's menu, over a sky and a strip of grass
// with a gem on it. A badge card on the left (the owner, the !LAMPs figure, a
// Wi-Fi signal bar), the command menu with its cursor on the right, and a
// dialogue box underneath that asks what to do and carries the key hint. No
// status bar: the layout has the whole screen. Positions are the design's, in
// screen pixels.

#include <stdio.h>
#include <string.h>
#include "home.h"

#define ROWS        7
#define ROW_Y       19
#define ROW_PITCH   21.5f
#define INK         0x23233A
#define GREY        0x5A5A70
#define CREAM       0xFFFDF2
#define GOLD        0xD8B848
#define RED         0xD8432F

#define F_TEXT      (&rpg_font_9)
#define F_BOLD      (&rpg_bold_10)
#define F_NUM       (&rpg_num_26)

static lv_obj_t *s_cursor, *s_names[ROWS], *s_ask, *s_keys;
static lv_obj_t *s_owner, *s_id, *s_lamps, *s_wifi, *s_signal;
static int s_top;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t hex)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, int x, int y, uint32_t hex, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

// A text whose right edge sits at x (in the parent's coordinates).
static lv_obj_t *text_right(lv_obj_t *parent, const lv_font_t *font, int x, int y, uint32_t hex)
{
    lv_obj_t *l = text(parent, font, 0, 0, hex, "");
    lv_obj_update_layout(parent);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, x - lv_obj_get_content_width(parent), y);
    return l;
}

// A panel: a dark frame with a soft drop under it, a cream face with a gold
// inner line. Returns the face; children are in the frame's coordinates
// minus (2, 2).
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *drop = rect(parent, x, y + 3, w, h, INK);
    lv_obj_set_style_bg_opa(drop, 90, 0);
    lv_obj_set_style_radius(drop, 8, 0);
    lv_obj_t *frame = rect(parent, x, y, w, h, INK);
    lv_obj_set_style_radius(frame, 8, 0);
    lv_obj_t *face = rect(frame, 2, 2, w - 4, h - 4, CREAM);
    lv_obj_set_style_radius(face, 6, 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(GOLD), 0);
    return face;
}

static void info(void)
{
    home_info_t in;
    char buf[64];
    int i = home_find("badge");
    bool owned = false;
    if (i >= 0) {
        home_info(home_app(i), &in);
        owned = in.value[0];
        set_text(s_owner, in.line);
        set_text(s_lamps, in.value);
        snprintf(buf, sizeof(buf), "No.%s", in.extra[0] ? in.extra : "------");
        set_text(s_id, buf);
        snprintf(buf, sizeof(buf), "What will %s do?", owned ? in.line : "you");
        set_text(s_ask, buf);
    }
    i = home_find("wifi");
    if (i >= 0) {
        home_info(home_app(i), &in);
        bool ok = in.value_color == COL_OK;
        set_text(s_wifi, ok ? "OK" : in.value[0] ? in.value : "NONE");
        lv_obj_set_style_text_color(s_wifi, lv_color_hex(ok ? 0x2E7D4F : 0xB83A16), 0);
        // The signal bar: full when joined, a stub otherwise.
        lv_obj_set_width(s_signal, ok ? 71 : 9);
        lv_obj_set_style_bg_color(s_signal, lv_color_hex(ok ? 0x4FB06D : 0xE8552D), 0);
    }
}

static void render(bool list)
{
    int sel = home_sel(), count = home_count();
    if (sel < s_top) {
        s_top = sel;
    } else if (sel >= s_top + ROWS) {
        s_top = sel - ROWS + 1;
    }
    if (s_top > count - ROWS) {
        s_top = count - ROWS;
    }
    if (s_top < 0) {
        s_top = 0;
    }
    for (int i = 0; i < ROWS; i++) {
        const os_app_t *app = home_app(s_top + i);
        // The menu of the design says STORE.
        lv_label_set_text(s_names[i], !app ? "" : !strcmp(app->id, "store") ? "Store" : app->name);
    }
    lv_obj_set_y(s_cursor, ROW_Y - 10 + (int)((sel - s_top) * ROW_PITCH));
    char keys[64];
    snprintf(keys, sizeof(keys), "D-PAD move   SELECT open   %d/%d", sel / ROWS + 1, (count + ROWS - 1) / ROWS);
    lv_label_set_text(s_keys, keys);
    info();
}

static void enter(lv_obj_t *root)
{
    lv_obj_t *owner = lv_obj_create(root);
    lv_obj_remove_style_all(owner);
    lv_obj_t *scr = home_fullscreen(owner);

    // The sky, the grass and the checkered edge between them.
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xBFE8F5), 0);
    rect(scr, 0, 110, OS_ROOT_W, 130, 0x9ED8A8);
    for (int x = 0; x < OS_ROOT_W; x += 12) {
        rect(scr, x, 104, 6, 6, 0x86C98E);
    }
    lv_obj_t *gem = lv_image_create(scr);
    lv_image_set_src(gem, &img_rpg_gem);
    lv_obj_set_pos(gem, 76, 132);

    // The badge card, under a strip in red.
    lv_obj_t *card = panel(scr, 8, 8, 186, 116);
    lv_obj_t *strip = rect(card, 2, 2, 178, 20, RED);
    text(strip, F_TEXT, 8, 4, 0xFFFFFF, "BADGE CARD");
    s_id = text_right(strip, F_TEXT, 170, 4, 0xFFFFFF);
    text(card, F_TEXT, 10, 28, GREY, "NAME");
    s_owner = text_right(card, F_BOLD, 172, 27, INK);
    text(card, F_TEXT, 10, 67, GREY, "!LAMPs");
    s_lamps = text_right(card, F_NUM, 172, 50, INK);
    text(card, F_TEXT, 10, 92, GREY, "WI-FI");
    lv_obj_t *bar = rect(card, 54, 94, 79, 8, CREAM);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(INK), 0);
    s_signal = rect(bar, 0, 0, 9, 4, 0xE8552D);
    s_wifi = text_right(card, F_BOLD, 172, 91, 0xB83A16);

    // The command menu.
    lv_obj_t *menu = panel(scr, 200, 8, 112, 164);
    s_cursor = text(menu, &lv_font_montserrat_12, 9, 0, INK, LV_SYMBOL_PLAY);
    for (int i = 0; i < ROWS; i++) {
        s_names[i] = text(menu, F_BOLD, 23, ROW_Y - 10 + (int)(i * ROW_PITCH), INK, "");
        lv_obj_set_width(s_names[i], 80);
        lv_label_set_long_mode(s_names[i], LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_height(s_names[i], lv_font_get_line_height(F_BOLD));
    }

    // The dialogue box.
    lv_obj_t *box = panel(scr, 8, 178, 304, 54);
    s_ask = text(box, F_BOLD, 12, 8, INK, "What will you do?");
    lv_obj_set_width(s_ask, 256);
    lv_label_set_long_mode(s_ask, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_ask, lv_font_get_line_height(F_BOLD));
    text(box, &lv_font_montserrat_12, 277, 8, RED, LV_SYMBOL_DOWN);
    s_keys = text(box, F_TEXT, 12, 31, GREY, "");
    s_top = 0;
    render(true);
}

static void key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        home_select(home_step(home_sel(), k == OS_KEY_DOWN ? 1 : -1, repeat));
    } else {
        home_select(home_sel() + (k == OS_KEY_RIGHT ? ROWS : -ROWS));
    }
}

const home_layout_t home_rpg = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
