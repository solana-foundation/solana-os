// Home layout: bento. Tiles of three sizes on a 4x3 grid per page: a big one
// for My Badge (owner, badge ID, !LAMPs), wide ones for apps with something
// to say (Wi-Fi's network and state, the mesh channel, the OS version) and
// small ones for the rest. Apps keep their registry order; a tile that does
// not fit the page starts the next one.

#include <stdio.h>
#include <string.h>
#include "home.h"

#define COLS        4
#define ROWS        3
#define CELL_W      71
#define CELL_H      60
#define GAP         6
#define X0          8
#define Y0          4
#define MAX_APPS    32

typedef struct {
    uint8_t page, c, r, w, h;
} slot_t;

typedef struct {
    lv_obj_t *obj, *icon, *name, *line, *value, *extra;
    int app;            // index in the home list
} tile_t;

static lv_obj_t *s_box, *s_pos;
static slot_t s_slots[MAX_APPS];
static tile_t s_tiles[COLS * ROWS];
static int s_ntiles, s_pages, s_page;

static void size_of(const os_app_t *app, int *w, int *h)
{
    static const char *const small[] = {"guide", "xdex", "trade", "quests", "beacon"};
    *w = 2;
    *h = !strcmp(app->id, "badge") ? 2 : 1;
    for (int i = 0; i < (int)(sizeof(small) / sizeof(small[0])); i++) {
        if (!strcmp(app->id, small[i])) {
            *w = 1;
        }
    }
}

// First fit, in order: the first free place on the page the tile fits, or
// the top left of a new page.
static void pack(void)
{
    bool used[ROWS][COLS] = {0};
    int page = 0, count = home_count();
    for (int i = 0; i < count && i < MAX_APPS; i++) {
        int w, h;
        size_of(home_app(i), &w, &h);
        bool placed = false;
        for (int pass = 0; pass < 2 && !placed; pass++) {
            for (int r = 0; r + h <= ROWS && !placed; r++) {
                for (int c = 0; c + w <= COLS && !placed; c++) {
                    bool ok = true;
                    for (int y = r; y < r + h; y++) {
                        for (int x = c; x < c + w; x++) {
                            ok = ok && !used[y][x];
                        }
                    }
                    if (ok) {
                        for (int y = r; y < r + h; y++) {
                            for (int x = c; x < c + w; x++) {
                                used[y][x] = true;
                            }
                        }
                        s_slots[i] = (slot_t){page, c, r, w, h};
                        placed = true;
                    }
                }
            }
            if (!placed) {
                page++;
                memset(used, 0, sizeof(used));
            }
        }
    }
    s_pages = page + 1;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void tile_info(tile_t *t)
{
    const os_app_t *app = home_app(t->app);
    bool fill = t->app == home_sel() && home_fills();
    home_info_t in;
    home_info(app, &in);
    set_text(t->line, in.line);
    set_text(t->value, in.value);
    set_text(t->extra, in.extra);
    if (t->value) {
        home_text_focus(t->value, fill, in.value_color && s_slots[t->app].h == 1 ? in.value_color : COL_TEXT);
    }
}

static void tile_focus(tile_t *t)
{
    const os_app_t *app = home_app(t->app);
    bool on = t->app == home_sel(), fill = on && home_fills();
    home_tile_focus(t->obj, on);
    home_icon_focus(t->icon, app, fill, 0);
    home_text_focus(t->name, fill, COL_TEXT);
    if (t->line) {
        home_text_focus(t->line, fill, COL_DIM);
    }
    if (t->extra) {
        home_text_focus(t->extra, fill, COL_DIM);
    }
    tile_info(t);
}

static void build(int page)
{
    lv_obj_clean(s_box);
    s_ntiles = 0;
    for (int i = 0; i < home_count() && i < MAX_APPS; i++) {
        const slot_t *s = &s_slots[i];
        if (s->page != page) {
            continue;
        }
        const os_app_t *app = home_app(i);
        int w = s->w * CELL_W + (s->w - 1) * GAP, h = s->h * CELL_H + (s->h - 1) * GAP;
        tile_t *t = &s_tiles[s_ntiles++];
        memset(t, 0, sizeof(*t));
        t->app = i;
        t->obj = home_tile(s_box, X0 + s->c * (CELL_W + GAP), Y0 + s->r * (CELL_H + GAP), w, h);
        if (s->w == 1) {
            // Small: the icon over the name.
            t->icon = home_icon(t->obj, app, 0);
            lv_obj_set_pos(t->icon, 8, 6);
            t->name = ui_label(t->obj, &sol_font_14, COL_TEXT, app->name);
            lv_obj_set_width(t->name, w - 12);
            lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(t->name, lv_font_get_line_height(&sol_font_14));
            lv_obj_align(t->name, LV_ALIGN_BOTTOM_LEFT, 7, -5);
            continue;
        }
        if (app->symbol) {
            t->icon = ui_label(t->obj, &lv_font_montserrat_14, app->accent, app->symbol);
        } else {
            t->icon = lv_image_create(t->obj);
            lv_image_set_src(t->icon, &img_logomark_16);
        }
        lv_obj_set_pos(t->icon, 8, 8);
        t->name = ui_label(t->obj, &sol_font_14, COL_TEXT, app->name);
        lv_obj_set_pos(t->name, 30, 7);
        t->line = ui_label(t->obj, s->h == 2 ? &sol_font_14 : &sol_mono_12, COL_DIM, "");
        lv_obj_set_width(t->line, w - 18);
        lv_label_set_long_mode(t->line, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(t->line, lv_font_get_line_height(s->h == 2 ? &sol_font_14 : &sol_mono_12));
        lv_obj_align(t->line, LV_ALIGN_BOTTOM_LEFT, 9, -7);
        if (s->h == 2) {
            // Big: the figure in the middle, the badge ID in the corner.
            t->value = ui_label(t->obj, &sol_font_32, COL_TEXT, "");
            lv_obj_set_pos(t->value, 8, 40);
            lv_obj_t *unit = ui_label(t->obj, &sol_mono_12, COL_DIM, "!LAMPs");
            lv_obj_set_pos(unit, 9, 78);
            t->extra = ui_label(t->obj, &sol_mono_12, COL_DIM, "");
            lv_obj_align(t->extra, LV_ALIGN_TOP_RIGHT, -8, 9);
            lv_obj_set_user_data(t->obj, unit);     // recoloured with the tile, in render()
        } else {
            t->value = ui_label(t->obj, &sol_mono_12, COL_TEXT, "");
            lv_obj_align(t->value, LV_ALIGN_TOP_RIGHT, -8, 9);
        }
    }
}

static void render(bool list)
{
    if (list) {
        pack();
    }
    int page = home_sel() < MAX_APPS ? s_slots[home_sel()].page : 0;
    if (list || page != s_page) {
        s_page = page;
        build(page);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", page + 1, s_pages);
        lv_label_set_text(s_pos, s_pages > 1 ? pos : "");
    }
    for (int i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        tile_focus(t);
        lv_obj_t *unit = lv_obj_get_user_data(t->obj);
        if (unit) {
            home_text_focus(unit, t->app == home_sel() && home_fills(), COL_DIM);
        }
    }
}

static void info(void)
{
    for (int i = 0; i < s_ntiles; i++) {
        tile_info(&s_tiles[i]);
    }
}

static void enter(lv_obj_t *root)
{
    s_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_scrollable(s_box, false);
    lv_obj_t *hint = ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " move   SELECT open");
    s_pos = ui_label(lv_obj_get_parent(hint), &sol_mono_12, COL_DIM, "");
    lv_obj_align(s_pos, LV_ALIGN_RIGHT_MID, -6, 0);
    s_page = -1;
    render(true);
}

// The tile on the selected one's page that covers grid cell (c, r), or -1.
static int tile_at(int page, int c, int r)
{
    for (int i = 0; i < home_count() && i < MAX_APPS; i++) {
        const slot_t *s = &s_slots[i];
        if (s->page == page && c >= s->c && c < s->c + s->w && r >= s->r && r < s->r + s->h) {
            return i;
        }
    }
    return -1;
}

static void key(os_key_t k, bool repeat)
{
    int sel = home_sel();
    if (sel >= MAX_APPS) {
        return;
    }
    const slot_t *s = &s_slots[sel];
    int n = -1;
    switch (k) {
    case OS_KEY_UP:    n = tile_at(s->page, s->c, s->r - 1); break;
    case OS_KEY_DOWN:  n = tile_at(s->page, s->c, s->r + s->h); break;
    case OS_KEY_LEFT:  n = tile_at(s->page, s->c - 1, s->r); break;
    default:           n = tile_at(s->page, s->c + s->w, s->r); break;
    }
    if (n < 0 && (k == OS_KEY_LEFT || k == OS_KEY_RIGHT)) {
        // Off the edge: the next or the last tile in order, onto another page.
        n = home_step(sel, k == OS_KEY_RIGHT ? 1 : -1, repeat);
    }
    if (n >= 0) {
        home_select(n);
    }
}

const home_layout_t home_bento = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
