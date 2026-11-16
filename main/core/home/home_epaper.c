// Home layout: E-paper. A printed page: a serif masthead over a double rule,
// My Badge as the left column (the owner in italics, the !LAMPs figure, the
// badge ID), the other apps as a numbered list on the right with the selected
// row in reverse, and a ruled footer. No status bar and no icons. Positions
// are the design's, in screen pixels.

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "home.h"

#define ROWS        6
#define ROW_H       30
#define LIST_X      160
#define LIST_Y      36
#define LIST_W      148
#define INK         0x141412
#define PAPER       0xECE8DD

#define F_NAME      (&paper_font_16)
#define F_ITALIC    (&paper_italic_17)
#define F_NUM       (&paper_num_46)
#define F_CAPS      (&paper_mono_9)
#define F_ID        (&paper_mono_11)

typedef struct {
    lv_obj_t *obj, *name, *right;
} row_t;

static row_t s_rows[ROWS];
static lv_obj_t *s_head, *s_owner, *s_lamps, *s_id, *s_tag, *s_page;
static int s_list[32], s_n, s_top, s_badge;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void upper(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
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

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, int x, int y, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(INK), 0);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Every app but My Badge is a row; My Badge is the left column.
static void collect(void)
{
    s_badge = home_find("badge");
    s_n = 0;
    for (int i = 0; i < home_count() && s_n < 32; i++) {
        if (i != s_badge) {
            s_list[s_n++] = i;
        }
    }
}

static int row_of(int app)
{
    for (int i = 0; i < s_n; i++) {
        if (s_list[i] == app) {
            return i;
        }
    }
    return -1;
}

static void info(void)
{
    home_info_t in;
    char buf[64];
    if (s_badge >= 0) {
        home_info(home_app(s_badge), &in);
        set_text(s_owner, in.line);
        set_text(s_lamps, in.value);
        set_text(s_id, in.extra);
    }
    // The masthead's right: the network, the OS version.
    int w = home_find("wifi");
    bool ok = false;
    if (w >= 0) {
        home_info(home_app(w), &in);
        ok = in.value_color == COL_OK;
    }
    snprintf(buf, sizeof(buf), "%s \xC2\xB7 %.12s", ok ? "WI-FI" : "NO WI-FI", esp_app_get_description()->version);
    set_text(s_head, buf);
    for (int i = 0; i < ROWS; i++) {
        int r = s_top + i;
        if (r >= s_n) {
            continue;
        }
        // A state where the app has one (Wi-Fi), its number otherwise.
        home_info(home_app(s_list[r]), &in);
        if (in.value[0]) {
            strlcpy(buf, in.value, sizeof(buf));
            upper(buf);
        } else {
            snprintf(buf, sizeof(buf), "%02d", r + 1);
        }
        set_text(s_rows[i].right, buf);
    }
}

static void render(bool list)
{
    if (list) {
        collect();
    }
    int sel = home_sel(), r = row_of(sel);
    if (r >= 0) {
        if (r < s_top) {
            s_top = r;
        } else if (r >= s_top + ROWS) {
            s_top = r - ROWS + 1;
        }
    }
    if (s_top > s_n - ROWS) {
        s_top = s_n - ROWS;
    }
    if (s_top < 0) {
        s_top = 0;
    }
    for (int i = 0; i < ROWS; i++) {
        row_t *row = &s_rows[i];
        int idx = s_top + i;
        lv_obj_set_hidden(row->obj, idx >= s_n);
        if (idx >= s_n) {
            continue;
        }
        bool on = s_list[idx] == sel;
        lv_label_set_text(row->name, home_app(s_list[idx])->name);
        lv_obj_set_style_bg_opa(row->obj, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row->obj, on || i == ROWS - 1 ? 0 : 1, 0);
        lv_obj_set_style_text_color(row->name, lv_color_hex(on ? PAPER : INK), 0);
        lv_obj_set_style_text_color(row->right, lv_color_hex(on ? PAPER : INK), 0);
    }
    // My Badge selected: its heading in reverse.
    bool badge = sel == s_badge;
    lv_obj_set_style_bg_opa(s_tag, badge ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_tag, lv_color_hex(badge ? PAPER : INK), 0);
    char page[32];
    int pages = (s_n + ROWS - 1) / ROWS;
    snprintf(page, sizeof(page), "PAGE %d OF %d", s_top / ROWS + 1 > pages ? pages : (s_top + ROWS - 1) / ROWS + 1,
             pages < 1 ? 1 : pages);
    lv_label_set_text(s_page, page);
    info();
}

static void enter(lv_obj_t *root)
{
    lv_obj_t *owner = lv_obj_create(root);
    lv_obj_remove_style_all(owner);
    lv_obj_t *scr = home_fullscreen(owner);
    lv_obj_set_style_bg_color(scr, lv_color_hex(PAPER), 0);

    // The masthead over a double rule.
    text(scr, F_NAME, 12, 5, "Solana OS");
    s_head = text(scr, F_CAPS, 0, 0, "");
    lv_obj_align(s_head, LV_ALIGN_TOP_RIGHT, -12, 9);
    rect(scr, 12, 26, 296, 2, INK);
    rect(scr, 12, 30, 296, 1, INK);

    // My Badge, the left column.
    s_tag = text(scr, F_CAPS, 10, 37, "MY BADGE");
    lv_obj_set_style_bg_color(s_tag, lv_color_hex(INK), 0);
    lv_obj_set_style_pad_hor(s_tag, 2, 0);
    lv_obj_set_style_pad_ver(s_tag, 1, 0);
    s_owner = text(scr, F_ITALIC, 12, 52, "");
    lv_obj_set_width(s_owner, 132);
    lv_label_set_long_mode(s_owner, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_owner, lv_font_get_line_height(F_ITALIC));
    s_lamps = text(scr, F_NUM, 12, 100, "");
    lv_obj_set_style_text_letter_space(s_lamps, -2, 0);
    text(scr, F_CAPS, 12, 150, "!LAMPS");
    rect(scr, 12, 191, 128, 1, INK);
    text(scr, F_CAPS, 12, 200, "BADGE ID");
    s_id = text(scr, F_ID, 0, 0, "");
    lv_obj_align(s_id, LV_ALIGN_TOP_RIGHT, -(OS_ROOT_W - 140), 199);
    rect(scr, 150, 38, 1, 176, INK);

    // The list.
    for (int i = 0; i < ROWS; i++) {
        row_t *r = &s_rows[i];
        r->obj = rect(scr, LIST_X, LIST_Y + i * ROW_H, LIST_W, ROW_H, INK);
        lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(r->obj, lv_color_hex(INK), 0);
        r->name = text(r->obj, F_NAME, 8, 7, "");
        r->right = text(r->obj, F_CAPS, 0, 0, "");
        lv_obj_align(r->right, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    rect(scr, 12, 220, 296, 1, INK);
    text(scr, F_CAPS, 12, 224, "D-PAD MOVE \xC2\xB7 SELECT OPEN");
    s_page = text(scr, F_CAPS, 0, 0, "");
    lv_obj_align(s_page, LV_ALIGN_TOP_RIGHT, -12, 224);
    s_top = 0;
    render(true);
}

// UP and DOWN along the list; LEFT to My Badge and RIGHT back.
static void key(os_key_t k, bool repeat)
{
    int sel = home_sel(), r = row_of(sel);
    if (k == OS_KEY_LEFT) {
        if (s_badge >= 0) {
            home_select(s_badge);
        }
        return;
    }
    if (r < 0) {
        // On My Badge: any other arrow goes to the list's top of page.
        if (s_n) {
            home_select(s_list[s_top < s_n ? s_top : 0]);
        }
        return;
    }
    if (k == OS_KEY_RIGHT) {
        return;
    }
    int n = r + (k == OS_KEY_DOWN ? 1 : -1);
    if (n < 0) {
        n = repeat ? 0 : s_n - 1;
    } else if (n >= s_n) {
        n = repeat ? s_n - 1 : 0;
    }
    home_select(s_list[n]);
}

const home_layout_t home_epaper = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
