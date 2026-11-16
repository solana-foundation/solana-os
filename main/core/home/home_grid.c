// Home layout: the paged 3x2 grid of tiles (the classic themes, and any theme
// without a layout of its own).

#include "home.h"

#define COLS        3
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define TILE_W      96
#define TILE_H      80
#define GAP         8
#define X0          ((OS_ROOT_W - COLS * TILE_W - (COLS - 1) * GAP) / 2)
#define Y0          8

static lv_obj_t *s_tiles[PER_PAGE], *s_icons[PER_PAGE], *s_names[PER_PAGE], *s_dots;
static int s_page;

static void render(bool list)
{
    int page = home_sel() / PER_PAGE;
    if (list || page != s_page) {
        s_page = page;
        for (int i = 0; i < PER_PAGE; i++) {
            const os_app_t *app = home_app(page * PER_PAGE + i);
            lv_obj_t *t = s_tiles[i];
            lv_obj_set_hidden(t, !app);
            if (!app) {
                continue;
            }
            lv_label_set_text(s_names[i], app->name);
            // Icons are either a symbol label or an image; swap the widget as needed.
            if (s_icons[i]) {
                lv_obj_delete(s_icons[i]);
            }
            s_icons[i] = home_icon(t, app, 0);
            lv_obj_align(s_icons[i], LV_ALIGN_CENTER, 0, -12);
        }
        home_dots_set(s_dots, (home_count() + PER_PAGE - 1) / PER_PAGE, page);
    }
    for (int i = 0; i < PER_PAGE; i++) {
        const os_app_t *app = home_app(page * PER_PAGE + i);
        if (app) {
            bool on = page * PER_PAGE + i == home_sel();
            home_tile_focus(s_tiles[i], on);
            home_text_focus(s_names[i], on && home_fills(), COL_TEXT);
            home_icon_focus(s_icons[i], app, on && home_fills(), 0);
        }
    }
}

static void enter(lv_obj_t *root)
{
    for (int i = 0; i < PER_PAGE; i++) {
        lv_obj_t *t = home_tile(root, X0 + (i % COLS) * (TILE_W + GAP), Y0 + (i / COLS) * (TILE_H + GAP), TILE_W, TILE_H);
        s_tiles[i] = t;
        s_icons[i] = NULL;
        s_names[i] = ui_label(t, &sol_font_14, COL_TEXT, "");
        lv_obj_set_width(s_names[i], TILE_W - 8);
        lv_obj_set_style_text_align(s_names[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_names[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_names[i], LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    s_dots = home_dots(root, OS_ROOT_H - UI_HINT_H - 14);
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " move   SELECT open   BOOT home");
    render(true);
}

static void key(os_key_t k, bool repeat)
{
    int n = home_sel(), count = home_count();
    switch (k) {
    case OS_KEY_LEFT:  n--; break;
    case OS_KEY_RIGHT: n++; break;
    case OS_KEY_UP:    n -= COLS; break;
    default:           n += COLS; break;
    }
    if (n < 0) {
        n = repeat ? 0 : (k == OS_KEY_LEFT ? count - 1 : home_sel());
    } else if (n >= count) {
        n = repeat ? count - 1 : (k == OS_KEY_RIGHT ? 0 : count - 1);
    }
    home_select(n);
}

const home_layout_t home_grid = {
    .enter = enter,
    .render = render,
    .key = key,
};
