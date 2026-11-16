#pragma once

// The home screen's layouts. core/launcher.c owns the list of apps shown, the
// selection and opening an app; the active theme (ui_theme()->home) picks the
// layout that draws them and moves the selection.
//
// A layout builds its objects in enter() and draws the list and selection
// there. render() redraws after the selection moved, or after the list
// changed (an app showing up or going: `list` is true). key() gets the four
// arrows and answers by calling home_select(); SELECT, BACK and HOME never
// reach it. info() runs once a second while the layout is on screen, for
// tiles that show live state (home_info()).

#include "core/os.h"
#include "core/ui.h"

typedef struct {
    void (*enter)(lv_obj_t *root);
    void (*render)(bool list);
    void (*key)(os_key_t key, bool repeat);
    void (*info)(void);
    // Optional: SELECT. True if the layout used it (it opens things that are
    // not in the home list, e.g. an installed Lua app); else the launcher
    // opens the selected app.
    bool (*select)(void);
} home_layout_t;

extern const home_layout_t home_term, home_blocks, home_controls, home_epaper;
extern const home_layout_t home_lumen;
extern const home_layout_t home_grid, home_list, home_bento, home_pearl, home_rpg, home_icons, home_dial, home_stream;

// The apps shown, in registry order, and the selected one.
int home_count(void);
const os_app_t *home_app(int index);
int home_sel(void);
void home_select(int index);            // clamps; the launcher then calls render(false)
// index + delta, wrapping round on a fresh press and stopping at the ends on
// a repeat, as every list in the OS does.
int home_step(int index, int delta, bool repeat);
int home_find(const char *id);          // index of the app with this id, or -1

// What a tile can say about its app beyond the name. Cheap (no I/O): states
// the OS already holds.
typedef struct {
    char line[48];          // "Wysdom.xyz", "@handle", "Mesh ch 1"; "" if nothing
    char value[24];         // "1,250", "Connected"; "" if nothing
    char extra[20];         // My Badge: the badge ID
    uint32_t value_color;   // a COL_*, 0 = text
} home_info_t;
void home_info(const os_app_t *app, home_info_t *out);

// An app's launcher icon as a child of parent: its symbol in colour `color`
// (a COL_*; 0 = the app's accent), or its image.
lv_obj_t *home_icon(lv_obj_t *parent, const os_app_t *app, uint32_t color);
// The design's line icon for an app, px pixels square (24 is its own size), in
// a colour: a hex 0xRRGGBB, not a COL_*. About keeps the Solana mark.
lv_obj_t *home_line_icon(lv_obj_t *parent, const os_app_t *app, uint32_t hex, int px);
void home_line_icon_color(lv_obj_t *icon, uint32_t hex);
// The design's gradient for an app's icon tile, top to bottom.
void home_gradient(const os_app_t *app, uint32_t *top, uint32_t *bottom);
// Recolour an icon or a label for a focused (filled) tile, or back.
void home_icon_focus(lv_obj_t *icon, const os_app_t *app, bool on, uint32_t color);
void home_text_focus(lv_obj_t *label, bool on, uint32_t color);
// True if the theme shows the selected tile filled with the focus colour.
bool home_fills(void);
// A tile: ui_panel with the theme's tile radius, no padding.
lv_obj_t *home_tile(lv_obj_t *parent, int x, int y, int w, int h);
// The tile as the selected one, or not, in the theme's way.
void home_tile_focus(lv_obj_t *tile, bool on);
// The layout's own header over the status bar (see launcher.c): draw on what
// this returns, in screen coordinates; it goes when `owner` is deleted.
lv_obj_t *home_header(lv_obj_t *owner);
// The whole screen for a layout with no status bar: an opaque sheet over the
// bar and the app root, in screen coordinates; it goes when `owner` is deleted.
lv_obj_t *home_fullscreen(lv_obj_t *owner);
// Page dots, centred at y; redraw with home_dots_set().
lv_obj_t *home_dots(lv_obj_t *parent, int y);
void home_dots_set(lv_obj_t *dots, int pages, int page);
