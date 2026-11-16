// Home layout: Control tiles. Dark glass tiles over the theme's wallpaper, no
// status bar. The first page is the design's: a cluster of four round
// controls (Wi-Fi, ESP-NOW, Apps, Settings), the balance tile, App Store, and
// a row of four small tiles. Other apps follow on a second page of small
// tiles. The selected small tile turns white; a selected control or wide tile
// gets a white ring. Positions are the design's, in screen pixels.

#include <stdio.h>
#include <string.h>
#include "home.h"

#define MAX_ITEMS   12
#define GLASS       0x12121C
#define GLASS_OPA   140
#define SOFT        0xD8DAE6
#define F_SMALL     (&apple_font_10)
#define F_LABEL     (&apple_font_12)
#define F_NAME      (&pearl_font_13)
#define F_NUM       (&apple_num_26)

typedef enum { K_ROUND, K_BALANCE, K_STORE, K_SMALL } kind_t;

typedef struct {
    const char *id;
    kind_t kind;
    int16_t x, y;           // the tile's (or the control's cell's) top left
} place_t;

static const place_t s_first[] = {
    {"wifi",     K_ROUND,   17,  18},
    {"espnow",   K_ROUND,   84,  18},
    {"apps",     K_ROUND,   17,  80},
    {"settings", K_ROUND,   84,  80},
    {"badge",    K_BALANCE, 163, 9},
    {"store",    K_STORE,   163, 82},
    {"quests",   K_SMALL,   8,   155},
    {"xdex",     K_SMALL,   86,  155},
    {"guide",    K_SMALL,   163, 155},
    {"trade",    K_SMALL,   241, 155},
};
#define FIRST_N     ((int)(sizeof(s_first) / sizeof(s_first[0])))

typedef struct {
    lv_obj_t *obj, *icon, *name, *line, *value;
    kind_t kind;
    int app;
    int16_t x, y, w, h;
} item_t;

static lv_obj_t *s_box, *s_dots;
static item_t s_items[MAX_ITEMS];
static int s_n, s_page, s_rest[32], s_nrest;

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t hex, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *glass(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = rect(parent, x, y, w, h, GLASS, 20);
    lv_obj_set_style_bg_opa(o, GLASS_OPA, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_white(), 0);
    lv_obj_set_style_border_opa(o, 30, 0);
    lv_obj_set_style_outline_color(o, lv_color_white(), 0);
    lv_obj_set_style_outline_pad(o, 0, 0);
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
    if (!home_app(app) || on_first(home_app(app))) {
        return 0;
    }
    for (int i = 0; i < s_nrest; i++) {
        if (s_rest[i] == app) {
            return 1 + i / MAX_ITEMS;
        }
    }
    return 0;
}

static int pages(void)
{
    return 1 + (s_nrest + MAX_ITEMS - 1) / MAX_ITEMS;
}

// A round control's fill: blue when on, green for the library, plain glass
// when off or neutral; its icon amber when the network is not joined.
static void round_state(item_t *it)
{
    const os_app_t *app = home_app(it->app);
    home_info_t in;
    home_info(app, &in);
    uint32_t top = 0, bottom = 0, icon = 0xFFFFFF;
    bool lit = false;
    if (!strcmp(app->id, "wifi")) {
        lit = in.value_color == COL_OK;
        top = 0x5AA0FF;
        bottom = 0x1D5BE0;
        icon = lit ? 0xFFFFFF : 0xFFC078;
    } else if (!strcmp(app->id, "espnow")) {
        lit = true;
        top = 0x5AA0FF;
        bottom = 0x1D5BE0;
    } else if (!strcmp(app->id, "apps")) {
        lit = true;
        top = 0x4FE39A;
        bottom = 0x12A65E;
    }
    lv_obj_set_style_bg_color(it->obj, lv_color_hex(lit ? top : 0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_color(it->obj, lv_color_hex(lit ? bottom : 0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_dir(it->obj, lit ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(it->obj, lit ? LV_OPA_COVER : 36, 0);
    home_line_icon_color(it->icon, icon);
}

static void item_info(item_t *it)
{
    const os_app_t *app = home_app(it->app);
    home_info_t in;
    if (it->kind == K_ROUND) {
        round_state(it);
    } else if (it->kind == K_BALANCE) {
        home_info(app, &in);
        set_text(it->line, in.line);
        set_text(it->value, in.value);
        lv_obj_set_hidden(it->name, !in.value[0]);
        lv_obj_align_to(it->name, it->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -5);
    } else if (it->kind == K_STORE) {
        home_info(app, &in);
        set_text(it->line, in.line);
    }
}

static void item_focus(item_t *it)
{
    bool on = it->app == home_sel();
    if (it->kind == K_SMALL) {
        // The selected small tile is white, its icon in the app's accent.
        const os_app_t *app = home_app(it->app);
        lv_obj_set_style_bg_color(it->obj, lv_color_hex(on ? 0xFFFFFF : GLASS), 0);
        lv_obj_set_style_bg_opa(it->obj, on ? LV_OPA_COVER : GLASS_OPA, 0);
        lv_obj_set_style_text_color(it->name, lv_color_hex(on ? 0x0B0C12 : 0xFFFFFF), 0);
        if (strcmp(app->id, "about")) {
            home_line_icon_color(it->icon, on ? ui_theme_hex(ui_theme_at(1), app->accent) : 0xFFFFFF);
        }
    } else {
        lv_obj_set_style_outline_width(it->obj, on ? 2 : 0, 0);
        if (it->kind == K_ROUND) {
            lv_obj_set_style_outline_pad(it->obj, 2, 0);
        }
    }
}

static item_t *item_make(int app, kind_t kind, int x, int y)
{
    item_t *it = &s_items[s_n++];
    const os_app_t *a = home_app(app);
    memset(it, 0, sizeof(*it));
    it->app = app;
    it->kind = kind;
    it->x = x;
    it->y = y;
    switch (kind) {
    case K_ROUND:
        // A 40 px circle in a 63 px cell, its name under it.
        it->w = 63;
        it->h = 58;
        it->obj = rect(s_box, x + 12, y + 1, 40, 40, 0xFFFFFF, LV_RADIUS_CIRCLE);
        lv_obj_set_style_outline_color(it->obj, lv_color_white(), 0);
        it->icon = home_line_icon(it->obj, a, 0xFFFFFF, 20);
        lv_obj_center(it->icon);
        it->name = text(s_box, F_SMALL, 0, 0, 0xFFFFFF, a->name);
        lv_obj_set_width(it->name, 63);
        lv_obj_set_style_text_align(it->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(it->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(it->name, lv_font_get_line_height(F_SMALL));
        lv_obj_set_pos(it->name, x, y + 45);
        break;
    case K_BALANCE: {
        it->w = 149;
        it->h = 66;
        it->obj = glass(s_box, x, y, it->w, it->h);
        lv_obj_t *orb = lv_image_create(it->obj);
        lv_image_set_src(orb, &img_list_orb);
        lv_image_set_scale(orb, 256 * 26 / 34);
        lv_obj_set_pos(orb, 108, 3);
        it->line = text(it->obj, F_SMALL, 11, 8, SOFT, "");
        lv_obj_set_width(it->line, 96);
        lv_label_set_long_mode(it->line, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(it->line, lv_font_get_line_height(F_SMALL));
        it->value = text(it->obj, F_NUM, 11, 27, 0xFFFFFF, "");
        lv_obj_set_style_text_letter_space(it->value, -1, 0);
        it->name = text(it->obj, F_SMALL, 80, 41, SOFT, "!LAMPs");
        break;
    }
    case K_STORE: {
        it->w = 149;
        it->h = 66;
        it->obj = glass(s_box, x, y, it->w, it->h);
        lv_obj_t *chip = rect(it->obj, 11, 14, 36, 36, 0x6BD2FF, 11);
        lv_obj_set_style_bg_grad_color(chip, lv_color_hex(0x0A84D6), 0);
        lv_obj_set_style_bg_grad_dir(chip, LV_GRAD_DIR_VER, 0);
        it->icon = home_line_icon(chip, a, 0xFFFFFF, 20);
        lv_obj_center(it->icon);
        it->name = text(it->obj, F_NAME, 57, 17, 0xFFFFFF, a->name);
        it->line = text(it->obj, F_SMALL, 57, 35, SOFT, "");
        lv_obj_set_width(it->line, 84);
        lv_label_set_long_mode(it->line, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(it->line, lv_font_get_line_height(F_SMALL));
        break;
    }
    default:
        it->w = 71;
        it->h = 66;
        it->obj = glass(s_box, x, y, it->w, it->h);
        it->icon = home_line_icon(it->obj, a, 0xFFFFFF, 22);
        lv_obj_set_pos(it->icon, 9, 9);
        it->name = text(it->obj, F_LABEL, 9, 41, 0xFFFFFF, a->name);
        lv_obj_set_width(it->name, 58);
        lv_label_set_long_mode(it->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(it->name, lv_font_get_line_height(F_LABEL));
        break;
    }
    return it;
}

static void build(int page)
{
    lv_obj_clean(s_box);
    s_n = 0;
    if (page == 0) {
        // The cluster's own tile sits behind its four controls.
        bool any = false;
        for (int p = 0; p < FIRST_N; p++) {
            any = any || (s_first[p].kind == K_ROUND && home_find(s_first[p].id) >= 0);
        }
        if (any) {
            glass(s_box, 8, 9, 149, 139);
        }
        for (int p = 0; p < FIRST_N; p++) {
            int a = home_find(s_first[p].id);
            if (a >= 0) {
                item_make(a, s_first[p].kind, s_first[p].x, s_first[p].y);
            }
        }
        return;
    }
    static const int16_t xs[] = {8, 86, 163, 241}, ys[] = {9, 82, 155};
    int base = (page - 1) * MAX_ITEMS;
    for (int i = 0; i < MAX_ITEMS && base + i < s_nrest; i++) {
        item_make(s_rest[base + i], K_SMALL, xs[i % 4], ys[i / 4]);
    }
}

static void render(bool list)
{
    if (list) {
        split();
    }
    int page = page_of(home_sel());
    if (list || page != s_page) {
        s_page = page;
        build(page);
        home_dots_set(s_dots, pages(), page);
    }
    for (int i = 0; i < s_n; i++) {
        item_info(&s_items[i]);
        item_focus(&s_items[i]);
    }
}

static void info(void)
{
    for (int i = 0; i < s_n; i++) {
        item_info(&s_items[i]);
    }
}

static void enter(lv_obj_t *root)
{
    lv_obj_t *owner = lv_obj_create(root);
    lv_obj_remove_style_all(owner);
    lv_obj_t *scr = home_fullscreen(owner);
    lv_obj_set_style_bg_image_src(scr, ui_theme()->wallpaper, 0);
    s_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, OS_ROOT_W, 226);
    lv_obj_set_scrollable(s_box, false);
    s_dots = home_dots(scr, 227);
    s_page = -1;
    render(true);
}

// The nearest item in the key's direction on this page; off the page's left
// or right edge, the page before or after.
static void key(os_key_t k, bool repeat)
{
    item_t *cur = NULL;
    for (int i = 0; i < s_n; i++) {
        if (s_items[i].app == home_sel()) {
            cur = &s_items[i];
        }
    }
    if (!cur) {
        return;
    }
    int cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < s_n; i++) {
        item_t *t = &s_items[i];
        if (t == cur) {
            continue;
        }
        int dx = t->x + t->w / 2 - cx, dy = t->y + t->h / 2 - cy;
        bool ok = k == OS_KEY_LEFT ? dx < -8 : k == OS_KEY_RIGHT ? dx > 8 : k == OS_KEY_UP ? dy < -8 : dy > 8;
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
        if (repeat || n < 2) {
            return;
        }
        page = page < 0 ? n - 1 : 0;
    }
    if (page == 0) {
        int a = home_find("badge");
        home_select(a >= 0 ? a : 0);
    } else if ((page - 1) * MAX_ITEMS < s_nrest) {
        home_select(s_rest[(page - 1) * MAX_ITEMS]);
    }
}

const home_layout_t home_controls = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
