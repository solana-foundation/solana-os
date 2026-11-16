// Home layout: the pearl homes. The first page is My Badge as a wide card
// with the !LAMPs balance, over Wi-Fi and Quests side by side, each with its
// artwork (assets/img_pearl_*.c for the light theme, img_black_pearl_*.c for
// the dark one). Every other app follows, six cards to a page. No hint bar:
// page dots only. Text is in the design's face, Geist (assets/pearl_*.c), not
// the OS's Diatype.

#include <stdio.h>
#include <string.h>
#include "home.h"

#define X0          8
#define Y0          2
#define GAP         6
#define HERO_W      304
#define HERO_H      108
#define CARD_W      149
#define CARD_H      80
#define REST_H      62
#define REST_ROWS   3
#define PER_PAGE    (2 * REST_ROWS)
#define DOTS_Y      204
#define MAX_APPS    32

typedef struct {
    lv_obj_t *obj, *title, *line, *value, *extra, *icon;
    int app;            // index in the home list
} card_t;

static lv_obj_t *s_box, *s_dots;
static card_t s_cards[PER_PAGE];
static int s_order[MAX_APPS];       // home list indexes, in this layout's order
static int s_n, s_first, s_ncards, s_page;

// My Badge, Wi-Fi and Quests lead (the first page); the rest keep their order.
static void order(void)
{
    static const char *const lead[] = {"badge", "wifi", "quests"};
    s_n = s_first = 0;
    for (int i = 0; i < 3; i++) {
        int a = home_find(lead[i]);
        if (a >= 0) {
            s_order[s_n++] = a;
        }
    }
    s_first = s_n;
    for (int i = 0; i < home_count() && s_n < MAX_APPS; i++) {
        bool led = false;
        for (int j = 0; j < s_first; j++) {
            led = led || s_order[j] == i;
        }
        if (!led) {
            s_order[s_n++] = i;
        }
    }
}

static int pos_of(int app)
{
    for (int i = 0; i < s_n; i++) {
        if (s_order[i] == app) {
            return i;
        }
    }
    return 0;
}

static int page_of(int pos)
{
    return pos < s_first ? 0 : 1 + (pos - s_first) / PER_PAGE;
}

static int pages(void)
{
    return 1 + (s_n - s_first + PER_PAGE - 1) / PER_PAGE;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *art(lv_obj_t *card, const lv_image_dsc_t *light, const lv_image_dsc_t *dark, int right, int bottom)
{
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, ui_light() ? light : dark);
    // The sprites are cut to the card's outer corners: ignore its border.
    lv_obj_align(img, LV_ALIGN_BOTTOM_RIGHT, ui_border_w() - right, ui_border_w() - bottom);
    return img;
}

static void card_info(card_t *c)
{
    home_info_t in;
    const os_app_t *app = home_app(c->app);
    home_info(app, &in);
    if (!strcmp(app->id, "badge")) {
        set_text(c->value, in.value);      // no figure until the badge has an owner
        set_text(c->line, in.line);
        set_text(c->extra, in.extra);
        lv_obj_set_hidden(c->extra, !in.extra[0]);
        lv_obj_align_to(c->extra, c->line, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        return;
    }
    set_text(c->line, in.line);
    set_text(c->value, in.value);
    if (c->value) {
        lv_color_t col = ui_col(in.value_color ? in.value_color : COL_DIM);
        lv_obj_set_style_text_color(c->value, col, 0);
        lv_obj_set_style_bg_color(c->icon, col, 0);     // Wi-Fi's state dot
        lv_obj_set_hidden(c->icon, !in.value[0]);
    }
}

static card_t *card_make(int app, int x, int y, int w, int h)
{
    card_t *c = &s_cards[s_ncards++];
    memset(c, 0, sizeof(*c));
    c->app = app;
    c->obj = home_tile(s_box, x, y, w, h);
    return c;
}

static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int w)
{
    lv_obj_t *l = ui_label(parent, font, color, "");
    lv_obj_set_style_max_width(l, w, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font));
    return l;
}

static void build(int page)
{
    lv_obj_clean(s_box);
    s_ncards = 0;
    int b = ui_border_w();      // the design's offsets are from the card's edge, a child's from inside its border
    if (page == 0) {
        for (int p = 0; p < s_first; p++) {
            const os_app_t *app = home_app(s_order[p]);
            card_t *c;
            if (!strcmp(app->id, "badge")) {
                c = card_make(s_order[p], X0, Y0, HERO_W, HERO_H);
                art(c->obj, &img_pearl_hero, &img_black_pearl_hero, ui_light() ? 4 : 0, 0);
                c->title = ui_label(c->obj, &pearl_font_11, COL_DIM, "!LAMPs balance");
                lv_obj_set_pos(c->title, 14 - b, 12 - b);
                c->value = ui_label(c->obj, &pearl_num_46, COL_TEXT, "");
                lv_obj_set_style_text_letter_space(c->value, -2, 0);
                lv_obj_set_pos(c->value, 14 - b, 27 - b);
                c->line = one_line(c->obj, &pearl_font_13, COL_TEXT, 110);
                lv_obj_set_pos(c->line, 14 - b, 81 - b);
                // The badge ID as a chip beside the handle.
                c->extra = ui_label(c->obj, &pearl_mono_9, COL_DIM, "");
                lv_obj_set_style_bg_color(c->extra, ui_col(COL_TEXT), 0);
                lv_obj_set_style_bg_opa(c->extra, LV_OPA_10, 0);
                lv_obj_set_style_radius(c->extra, 6, 0);
                lv_obj_set_style_pad_hor(c->extra, 5, 0);
                lv_obj_set_style_pad_ver(c->extra, 1, 0);
                continue;
            }
            bool wifi = !strcmp(app->id, "wifi");
            // Wi-Fi sits on the left unless it is the only small card.
            bool left = wifi || home_find("wifi") < 0;
            c = card_make(s_order[p], left ? X0 : X0 + CARD_W + GAP - 1, Y0 + HERO_H + GAP, CARD_W, CARD_H);
            if (wifi) {
                art(c->obj, &img_pearl_wifi, &img_black_pearl_wifi, 0, 0);
            } else {
                art(c->obj, &img_pearl_quest, &img_black_pearl_quest, ui_light() ? 17 : 0, ui_light() ? 7 : 0);
            }
            c->title = ui_label(c->obj, &pearl_font_13, COL_TEXT, app->name);
            lv_obj_set_pos(c->title, 12 - b, 11 - b);
            c->line = one_line(c->obj, &pearl_font_11, COL_DIM, wifi ? 106 : 100);
            lv_obj_set_pos(c->line, 12 - b, 28 - b);
            if (wifi) {
                // The state, after a dot in its colour.
                c->icon = lv_obj_create(c->obj);
                lv_obj_remove_style_all(c->icon);
                lv_obj_set_size(c->icon, 5, 5);
                lv_obj_set_style_radius(c->icon, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_opa(c->icon, LV_OPA_COVER, 0);
                lv_obj_align(c->icon, LV_ALIGN_BOTTOM_LEFT, 12 - b, b - 13);
                c->value = one_line(c->obj, &pearl_font_11, COL_DIM, 74);
                lv_obj_align(c->value, LV_ALIGN_BOTTOM_LEFT, 22 - b, b - 10);
            }
        }
    } else {
        int base = s_first + (page - 1) * PER_PAGE;
        for (int i = 0; i < PER_PAGE && base + i < s_n; i++) {
            const os_app_t *app = home_app(s_order[base + i]);
            card_t *c = card_make(s_order[base + i], i % 2 ? X0 + CARD_W + GAP - 1 : X0, Y0 + (i / 2) * (REST_H + GAP),
                                  CARD_W, REST_H);
            c->title = ui_label(c->obj, &pearl_font_13, COL_TEXT, app->name);
            lv_obj_set_pos(c->title, 12 - b, 11 - b);
            c->line = one_line(c->obj, &pearl_font_11, COL_DIM, CARD_W - 24);
            lv_obj_align(c->line, LV_ALIGN_BOTTOM_LEFT, 12 - b, b - 10);
            if (app->symbol) {
                c->icon = ui_label(c->obj, &lv_font_montserrat_14, app->accent, app->symbol);
            } else {
                c->icon = lv_image_create(c->obj);
                lv_image_set_src(c->icon, &img_logomark_16);
            }
            lv_obj_align(c->icon, LV_ALIGN_TOP_RIGHT, -11, 9);
        }
    }
}

static void render(bool list)
{
    if (list) {
        order();
    }
    int page = page_of(pos_of(home_sel()));
    if (list || page != s_page) {
        s_page = page;
        build(page);
        home_dots_set(s_dots, pages(), page);
    }
    for (int i = 0; i < s_ncards; i++) {
        card_t *c = &s_cards[i];
        home_tile_focus(c->obj, c->app == home_sel());
        card_info(c);
    }
}

static void info(void)
{
    for (int i = 0; i < s_ncards; i++) {
        card_info(&s_cards[i]);
    }
}

static void enter(lv_obj_t *root)
{
    s_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, OS_ROOT_W, DOTS_Y - 2);
    lv_obj_set_scrollable(s_box, false);
    s_dots = home_dots(root, DOTS_Y);
    // The first view is the design's: My Badge selected.
    static bool opened;
    if (!opened && home_find("badge") >= 0) {
        home_select(home_find("badge"));
    }
    opened = true;
    s_page = -1;
    render(true);
}

static void key(os_key_t k, bool repeat)
{
    int pos = pos_of(home_sel()), n = pos;
    bool vert = k == OS_KEY_UP || k == OS_KEY_DOWN;
    int d = k == OS_KEY_DOWN || k == OS_KEY_RIGHT ? 1 : -1;
    if (!vert) {
        // Along the cards in order, onto the next page and round.
        n = pos + d;
        if (n < 0) {
            n = repeat ? 0 : s_n - 1;
        } else if (n >= s_n) {
            n = repeat ? s_n - 1 : 0;
        }
    } else if (pos < s_first) {
        // The first page: My Badge over the two small cards.
        bool hero = !strcmp(home_app(s_order[pos])->id, "badge");
        if (hero && d > 0 && s_first > 1) {
            n = pos + 1;
        } else if (!hero && d < 0 && home_find("badge") >= 0) {
            n = 0;
        }
    } else {
        int base = s_first + (page_of(pos) - 1) * PER_PAGE;
        int m = pos + 2 * d;
        if (m >= base && m < base + PER_PAGE && m < s_n) {
            n = m;
        }
    }
    home_select(s_order[n]);
}

const home_layout_t home_pearl = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
