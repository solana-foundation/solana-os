// Home layout: Bold blocks. The first page is the design's: My Badge as the
// big purple block, Wi-Fi beside it, four small blocks and App Store, each
// its own flat colour with a thick outline and a hard shadow. Other apps
// follow, six wide blocks to a page. The status bar gives way to the layout's
// own header (the OS's name and a network chip). Positions are the design's,
// in screen pixels minus the 22 px bar.

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "home.h"

#define MAX_TILES   8
#define PER_PAGE    6               // on the pages after the first
#define INK         COL_TEXT

#define F_TITLE     (&blocks_font_13)
#define F_HEAD      (&blocks_font_15)
#define F_BIG       (&blocks_num_41)
#define F_TEXT      (&blocks_text_12)
#define F_MONO      (&blocks_mono_9)

typedef struct {
    const char *id;
    int16_t x, y, w, h;
    uint32_t fill;
} place_t;

// The first page, by app.
static const place_t s_first[] = {
    {"badge",  9,   4,   149, 122, 0x6B2BD9},
    {"wifi",   168, 4,   141, 57,  0xFFFFFF},
    {"quests", 168, 69,  66,  57,  0xFF8FB8},
    {"xdex",   243, 69,  66,  57,  0xFFD23F},
    {"guide",  9,   136, 70,  57,  0x5FE0A0},
    {"trade",  88,  136, 70,  57,  0x7CC4FF},
    {"store",  168, 136, 141, 57,  0xFF5A36},
};
#define FIRST_N     ((int)(sizeof(s_first) / sizeof(s_first[0])))

typedef struct {
    lv_obj_t *obj, *line, *value, *extra;
    int app;                // index in the home list
    int16_t x, y, w, h;
} tile_t;

static lv_obj_t *s_box, *s_pos, *s_chip;
static tile_t s_tiles[MAX_TILES];
static int s_ntiles, s_page, s_rest[32], s_nrest;

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void upper(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static lv_obj_t *ink(lv_obj_t *parent, const lv_font_t *font, int x, int y, const char *s, bool white)
{
    lv_obj_t *l = ui_label(parent, font, INK, s);
    if (white) {
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

// A small outlined tag: the badge ID, a state.
static lv_obj_t *chip(lv_obj_t *parent, uint32_t fill, const char *s)
{
    lv_obj_t *l = ui_label(parent, F_MONO, INK, s);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(fill), 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, ui_col(INK), 0);
    lv_obj_set_style_radius(l, 8, 0);
    lv_obj_set_style_pad_hor(l, 5, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    return l;
}

// Which apps the first page holds, and which go on the pages after it.
static bool on_first(const os_app_t *app)
{
    for (int i = 0; i < FIRST_N; i++) {
        if (!strcmp(app->id, s_first[i].id)) {
            return true;
        }
    }
    return false;
}

static void split(void)
{
    s_nrest = 0;
    for (int i = 0; i < home_count() && s_nrest < 32; i++) {
        if (!on_first(home_app(i))) {
            s_rest[s_nrest++] = i;
        }
    }
}

static int page_of(int app)
{
    if (on_first(home_app(app))) {
        return 0;
    }
    for (int i = 0; i < s_nrest; i++) {
        if (s_rest[i] == app) {
            return 1 + i / PER_PAGE;
        }
    }
    return 0;
}

static int pages(void)
{
    return 1 + (s_nrest + PER_PAGE - 1) / PER_PAGE;
}

static void tile_info(tile_t *t)
{
    const os_app_t *app = home_app(t->app);
    home_info_t in;
    home_info(app, &in);
    if (!strcmp(app->id, "badge")) {
        set_text(t->value, in.value);
        set_text(t->line, in.line);
        set_text(t->extra, in.extra);
        lv_obj_set_hidden(t->extra, !in.extra[0]);
        return;
    }
    char text[48];
    if (t->value) {
        // Wi-Fi: the network, and its state as a tag.
        strlcpy(text, in.value, sizeof(text));
        upper(text);
        set_text(t->value, text);
        lv_obj_set_hidden(t->value, !text[0]);
        lv_obj_set_style_bg_color(t->value, lv_color_hex(in.value_color == COL_OK ? 0x5FE0A0 : 0xFF5A36), 0);
        set_text(t->line, in.line);
        // The network's name runs up to the tag.
        lv_obj_update_layout(t->value);
        lv_obj_set_width(t->line, t->w - 24 - (text[0] ? lv_obj_get_width(t->value) + 6 : 0));
    } else if (t->line) {
        strlcpy(text, in.line, sizeof(text));
        upper(text);
        set_text(t->line, text);
    }
}

static tile_t *tile_make(int app, int x, int y, int w, int h, uint32_t fill)
{
    tile_t *t = &s_tiles[s_ntiles++];
    memset(t, 0, sizeof(*t));
    t->app = app;
    t->x = x;
    t->y = y;
    t->w = w;
    t->h = h;
    t->obj = home_tile(s_box, x, y, w, h);
    lv_obj_set_style_bg_color(t->obj, lv_color_hex(fill), 0);
    lv_obj_set_style_outline_color(t->obj, ui_col(INK), 0);
    lv_obj_set_style_outline_pad(t->obj, 1, 0);
    return t;
}

// A wide block: the name, a line under it, the icon at the right.
static void wide(tile_t *t, const os_app_t *app, bool button)
{
    ink(t->obj, F_HEAD, 10, 12, app->name, false);
    t->line = ink(t->obj, F_MONO, 10, 31, "", false);
    lv_obj_set_width(t->line, t->w - 54);
    lv_label_set_long_mode(t->line, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t->line, lv_font_get_line_height(F_MONO));
    lv_obj_t *icon;
    if (button) {
        // The round button of the design's App Store block.
        lv_obj_t *b = lv_obj_create(t->obj);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 30, 30);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, ui_col(INK), 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, 0);
        icon = home_line_icon(b, app, 0x111111, 16);
        lv_obj_center(icon);
    } else {
        icon = home_line_icon(t->obj, app, 0x111111, 20);
        lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, -8, 6);
    }
}

static void build(int page)
{
    lv_obj_clean(s_box);
    s_ntiles = 0;
    if (page > 0) {
        static const uint32_t fills[] = {0xFFFFFF, 0xFFD23F, 0x5FE0A0, 0x7CC4FF, 0xFF8FB8, 0xFF5A36};
        int base = (page - 1) * PER_PAGE;
        for (int i = 0; i < PER_PAGE && base + i < s_nrest; i++) {
            const os_app_t *app = home_app(s_rest[base + i]);
            tile_t *t = tile_make(s_rest[base + i], i % 2 ? 168 : 9, 4 + (i / 2) * 66, i % 2 ? 141 : 149, 57,
                                  fills[(base + i) % 6]);
            wide(t, app, false);
        }
        return;
    }
    for (int p = 0; p < FIRST_N; p++) {
        int a = home_find(s_first[p].id);
        if (a < 0) {
            continue;
        }
        const place_t *pl = &s_first[p];
        const os_app_t *app = home_app(a);
        tile_t *t = tile_make(a, pl->x, pl->y, pl->w, pl->h, pl->fill);
        if (!strcmp(pl->id, "badge")) {
            // The big block: the balance, the owner, the badge ID as a sticker.
            lv_obj_t *n = ink(t->obj, F_TITLE, 10, 9, "MY BADGE", true);
            (void)n;
            t->extra = chip(t->obj, 0xFFD23F, "");
            lv_obj_align(t->extra, LV_ALIGN_TOP_RIGHT, -8, 8);
            t->value = ink(t->obj, F_BIG, 10, 30, "", true);
            lv_obj_set_style_text_letter_space(t->value, -2, 0);
            ink(t->obj, F_MONO, 10, 77, "!LAMPs", true);
            t->line = ink(t->obj, F_TEXT, 10, 96, "", true);
            lv_obj_set_width(t->line, pl->w - 24);
            lv_label_set_long_mode(t->line, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(t->line, lv_font_get_line_height(F_TEXT));
        } else if (!strcmp(pl->id, "wifi")) {
            ink(t->obj, F_HEAD, 10, 9, app->name, false);
            lv_obj_t *icon = home_line_icon(t->obj, app, 0x111111, 20);
            lv_obj_align(icon, LV_ALIGN_TOP_RIGHT, -8, 6);
            t->line = ink(t->obj, F_MONO, 10, 34, "", false);
            lv_obj_set_width(t->line, 62);
            lv_label_set_long_mode(t->line, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(t->line, lv_font_get_line_height(F_MONO));
            t->value = ui_label(t->obj, F_MONO, INK, "");
            lv_obj_set_style_bg_opa(t->value, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(t->value, 4, 0);
            lv_obj_align(t->value, LV_ALIGN_TOP_RIGHT, -10, 34);
        } else if (pl->w > 100) {
            wide(t, app, true);
        } else {
            // A small block: the icon over the name.
            lv_obj_t *icon = home_line_icon(t->obj, app, 0x111111, 22);
            lv_obj_set_pos(icon, 7, 5);
            lv_obj_t *n = ink(t->obj, F_TITLE, 8, 32, app->name, false);
            lv_obj_set_width(n, pl->w - 14);
            lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(n, lv_font_get_line_height(F_TITLE));
        }
    }
}

static void header(void)
{
    home_info_t in;
    int i = home_find("wifi");
    char text[32] = "NO WI-FI";
    uint32_t fill = 0xFFD23F;
    if (i >= 0) {
        home_info(home_app(i), &in);
        if (in.value_color == COL_OK) {
            strlcpy(text, "WI-FI", sizeof(text));
            fill = 0x5FE0A0;
        }
    }
    set_text(s_chip, text);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(fill), 0);
}

static void render(bool list)
{
    if (list) {
        split();
    }
    int page = home_app(home_sel()) ? page_of(home_sel()) : 0;
    if (list || page != s_page) {
        s_page = page;
        build(page);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", page + 1, pages());
        lv_label_set_text(s_pos, pos);
    }
    for (int i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        lv_obj_set_style_outline_width(t->obj, t->app == home_sel() ? 3 : 0, 0);
        tile_info(t);
    }
    header();
}

static void info(void)
{
    for (int i = 0; i < s_ntiles; i++) {
        tile_info(&s_tiles[i]);
    }
    header();
}

static void enter(lv_obj_t *root)
{
    lv_obj_t *all = lv_obj_create(root);
    lv_obj_remove_style_all(all);
    lv_obj_set_size(all, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_scrollable(all, false);
    lv_obj_t *head = home_header(all);
    ink(head, F_TITLE, 10, 5, "SOLANA OS", false);
    s_chip = chip(head, 0xFFD23F, "");
    lv_obj_set_style_pad_ver(s_chip, 1, 0);
    lv_obj_align(s_chip, LV_ALIGN_TOP_RIGHT, -10, 4);

    s_box = lv_obj_create(all);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, OS_ROOT_W, 200);
    lv_obj_set_scrollable(s_box, false);
    ink(all, F_MONO, 10, 202, "D-PAD MOVE / SELECT OPEN", false);
    s_pos = ui_label(all, F_MONO, INK, "");
    lv_obj_align(s_pos, LV_ALIGN_TOP_RIGHT, -10, 202);
    s_page = -1;
    render(true);
}

// The nearest block in the key's direction on this page; off the page's left
// or right edge, the page before or after.
static void key(os_key_t k, bool repeat)
{
    tile_t *cur = NULL;
    for (int i = 0; i < s_ntiles; i++) {
        if (s_tiles[i].app == home_sel()) {
            cur = &s_tiles[i];
        }
    }
    if (!cur) {
        return;
    }
    int cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        if (t == cur) {
            continue;
        }
        int dx = t->x + t->w / 2 - cx, dy = t->y + t->h / 2 - cy;
        bool ok = k == OS_KEY_LEFT ? t->x + t->w <= cur->x : k == OS_KEY_RIGHT ? t->x >= cur->x + cur->w
                  : k == OS_KEY_UP ? t->y + t->h <= cur->y : t->y >= cur->y + cur->h;
        // Along the key's axis first, then how far off it.
        int d = k == OS_KEY_LEFT || k == OS_KEY_RIGHT ? LV_ABS(dx) + 3 * LV_ABS(dy) : LV_ABS(dy) + 3 * LV_ABS(dx);
        if (ok && d < best_d) {
            best = t->app;
            best_d = d;
        }
    }
    if (best >= 0) {
        home_select(best);
        return;
    }
    if (k != OS_KEY_LEFT && k != OS_KEY_RIGHT) {
        return;
    }
    int n = pages(), page = s_page + (k == OS_KEY_RIGHT ? 1 : -1);
    if (page < 0 || page >= n) {
        if (repeat) {
            return;
        }
        page = page < 0 ? n - 1 : 0;
    }
    if (page == 0) {
        int a = home_find("badge");
        home_select(a >= 0 ? a : 0);
    } else if ((page - 1) * PER_PAGE < s_nrest) {
        home_select(s_rest[(page - 1) * PER_PAGE]);
    }
}

const home_layout_t home_blocks = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
