// Home layout: a grid of colour icons with their names, and a dock. The last
// four apps live in the dock, the rest fill pages of 4x2 above it. A caption
// between the two names the selected app and what it has to say (the dock's
// icons carry no names of their own).

#include <stdio.h>
#include <string.h>
#include "home.h"

#define COLS        4
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define DOCK        4
#define ICON        46
#define CELL_W      69
#define CELL_H      64
#define X0          22
#define Y0          4
#define DOCK_H      56
#define DOCK_Y      154
#define DOCK_X      30
#define DOCK_W      260

typedef struct {
    lv_obj_t *box, *plate, *icon, *name;
    const os_app_t *app;
} cell_t;

static cell_t s_cells[PER_PAGE + DOCK];
static lv_obj_t *s_caption, *s_dots;
static int s_page;

// How many apps the dock holds: four, once there are enough to fill a row above.
static int docked(void)
{
    return home_count() > COLS + DOCK ? DOCK : 0;
}

static void cell_set(cell_t *c, const os_app_t *app)
{
    lv_obj_set_hidden(c->box, !app);
    if (app == c->app) {
        return;
    }
    c->app = app;
    if (!app) {
        return;
    }
    if (c->icon) {
        lv_obj_delete(c->icon);
    }
    // The design's gradient as the icon's plate, its line icon in white.
    uint32_t top, bottom;
    home_gradient(app, &top, &bottom);
    lv_obj_set_style_bg_color(c->plate, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(c->plate, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(c->plate, LV_GRAD_DIR_VER, 0);
    c->icon = home_line_icon(c->plate, app, 0xFFFFFF, c->name ? 24 : 22);
    lv_obj_center(c->icon);
    if (c->name) {
        lv_label_set_text(c->name, app->name);
    }
}

static void caption(void)
{
    // The design has no caption: the dock's icons stay unnamed but for the
    // selected one, named in the status bar.
    const os_app_t *app = home_app(home_sel());
    os_status_title(app && home_sel() >= home_count() - docked() ? app->name : "Solana OS", 0);
    if (!app || !s_caption) {
        return;
    }
    home_info_t in;
    home_info(app, &in);
    char text[96];
    const char *more = in.value[0] ? in.value : in.line;
    if (more[0]) {
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_BULLET " %s", app->name, more);
    } else {
        strlcpy(text, app->name, sizeof(text));
    }
    if (strcmp(lv_label_get_text(s_caption), text)) {
        lv_label_set_text(s_caption, text);
    }
}

static void render(bool list)
{
    int dock = docked(), grid = home_count() - dock, sel = home_sel();
    int page = sel < grid ? sel / PER_PAGE : s_page < 0 ? 0 : s_page;
    if (list || page != s_page) {
        s_page = page;
        for (int i = 0; i < PER_PAGE; i++) {
            int idx = page * PER_PAGE + i;
            cell_set(&s_cells[i], idx < grid ? home_app(idx) : NULL);
        }
        for (int i = 0; i < DOCK; i++) {
            cell_set(&s_cells[PER_PAGE + i], i < dock ? home_app(grid + i) : NULL);
        }
        home_dots_set(s_dots, (grid + PER_PAGE - 1) / PER_PAGE, page);
    }
    for (int i = 0; i < PER_PAGE + DOCK; i++) {
        cell_t *c = &s_cells[i];
        bool on = c->app && c->app == home_app(sel);
        lv_obj_set_style_outline_width(c->plate, on ? 2 : 0, 0);
    }
    caption();
}

static void cell_make(cell_t *c, lv_obj_t *parent, int x, int y, bool named)
{
    memset(c, 0, sizeof(*c));
    c->box = lv_obj_create(parent);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_pos(c->box, x, y);
    int icon = named ? ICON : 42;
    lv_obj_set_size(c->box, named ? CELL_W : icon + 8, named ? CELL_H : icon + 8);
    lv_obj_set_scrollable(c->box, false);
    c->plate = lv_obj_create(c->box);
    lv_obj_remove_style_all(c->plate);
    lv_obj_set_size(c->plate, icon, icon);
    lv_obj_align(c->plate, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(c->plate, named ? 13 : 12, 0);
    lv_obj_set_style_bg_opa(c->plate, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_color(c->plate, ui_focus_bg(), 0);
    lv_obj_set_style_outline_pad(c->plate, 2, 0);
    lv_obj_set_scrollable(c->plate, false);
    if (named) {
        c->name = ui_label(c->box, &apple_font_10, COL_TEXT, "");
        lv_obj_set_width(c->name, CELL_W - 2);
        lv_obj_set_style_text_align(c->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(c->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(c->name, lv_font_get_line_height(&apple_font_10));
        lv_obj_align(c->name, LV_ALIGN_TOP_MID, 0, 54);
    }
}

static void enter(lv_obj_t *root)
{
    for (int i = 0; i < PER_PAGE; i++) {
        cell_make(&s_cells[i], root, X0 + (i % COLS) * CELL_W, Y0 + (i / COLS) * 71, true);
    }
    s_caption = NULL;
    s_dots = home_dots(root, DOCK_Y - 9);

    // The dock: a frosted bar, four unnamed icons.
    lv_obj_t *dock = lv_obj_create(root);
    lv_obj_remove_style_all(dock);
    lv_obj_set_pos(dock, DOCK_X, DOCK_Y);
    lv_obj_set_size(dock, DOCK_W, DOCK_H);
    lv_obj_set_style_radius(dock, 19, 0);
    lv_obj_set_style_bg_color(dock, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dock, 51, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, lv_color_white(), 0);
    lv_obj_set_style_border_opa(dock, 56, 0);
    lv_obj_set_scrollable(dock, false);
    lv_obj_set_hidden(dock, !docked());
    for (int i = 0; i < DOCK; i++) {
        cell_make(&s_cells[PER_PAGE + i], dock, 13 + (int)(i * 60.4f), 2, false);
    }
    s_page = -1;
    render(true);
}

static void key(os_key_t k, bool repeat)
{
    int dock = docked(), grid = home_count() - dock, sel = home_sel();
    if (sel >= grid) {
        // In the dock: along it, or up to the grid above.
        int col = sel - grid;
        if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
            home_select(grid + (col + (k == OS_KEY_RIGHT ? 1 : dock - 1)) % dock);
        } else if (k == OS_KEY_UP) {
            int first = (s_page < 0 ? 0 : s_page) * PER_PAGE;
            int n = first + COLS + col;
            while (n >= grid && n >= first + COLS) {
                n -= COLS;
            }
            home_select(n < grid ? n : grid - 1);
        }
        return;
    }
    int col = sel % COLS, row = (sel % PER_PAGE) / COLS;
    switch (k) {
    case OS_KEY_LEFT:
        home_select(sel > 0 ? sel - 1 : repeat ? 0 : grid - 1);
        break;
    case OS_KEY_RIGHT:
        home_select(sel + 1 < grid ? sel + 1 : repeat ? sel : 0);
        break;
    case OS_KEY_UP:
        if (row > 0) {
            home_select(sel - COLS);
        }
        break;
    default:
        if (row + 1 < ROWS && sel + COLS < grid) {
            home_select(sel + COLS);
        } else if (dock) {
            home_select(grid + (col < dock ? col : dock - 1));
        }
        break;
    }
}

const home_layout_t home_icons = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = caption,
};
