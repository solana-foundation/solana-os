#pragma once

// Themes: one const table of looks (core/ui_theme.c). A theme is a palette
// for every COL_* of theme.h plus the shape, focus and chrome tokens the
// shared widgets (core/ui.c), the OS chrome (core/os.c) and the home screen
// (core/launcher.c) read. The Themes app (apps/themes) picks one; the choice
// is the OS_SET_THEME setting, a theme's `id`.
//
// Ids are saved on badges: never renumber one. 0 and 1 are the Dark and Light
// of every release before themes. An id no theme has falls back to 0.

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

enum {
    UI_THEME_DARK = 0,
    UI_THEME_LIGHT = 1,
    UI_THEME_PEARL = 2,
    UI_THEME_BLACK_PEARL = 3,
    UI_THEME_GLOW = 4,          // "Lumen"
    UI_THEME_ICONS = 5,
    UI_THEME_GROUPED = 6,
    UI_THEME_DIAL = 7,
    UI_THEME_CONTROLS = 8,
    UI_THEME_TERMINAL = 9,
    UI_THEME_EPAPER = 10,
    UI_THEME_BLOCKS = 11,
    UI_THEME_RPG = 12,
    UI_THEME_STREAM = 13,
};

// Palette slots, in the order of theme.h's surface and brand colours.
typedef enum {
    UI_P_BG, UI_P_PANEL, UI_P_PANEL_HI, UI_P_BORDER, UI_P_TEXT, UI_P_DIM,
    UI_P_PURPLE, UI_P_GREEN, UI_P_PINK, UI_P_LPURPLE, UI_P_BLUE, UI_P_AQUA, UI_P_LIME, UI_P_YELLOW, UI_P_ORANGE,
    UI_P_COUNT,
} ui_pal_t;

// How the selected thing (a list row, a home tile, a tab) is shown.
typedef enum {
    UI_FOCUS_BAR,       // raised row with an accent bar on its left (the classic list)
    UI_FOCUS_FILL,      // filled with focus_bg, text in focus_fg
    UI_FOCUS_RING,      // a focus_bg outline, colours unchanged
    UI_FOCUS_CURSOR,    // a ">" cursor before it, colours unchanged
} ui_focus_t;

// The status bar's bottom edge.
typedef enum {
    UI_BAR_HAIRLINE,    // the brand gradient hairline
    UI_BAR_PLAIN,       // nothing
    UI_BAR_RULE,        // a solid rule in the border colour, border_w thick
    UI_BAR_STREAM,      // no edge; a back chevron and the app's name in bold (the home screen puts its tabs there)
    UI_BAR_SLIM,        // no edge, no logo, no percentage: a small bold title, the Wi-Fi glyph and a battery outline
} ui_bar_t;

typedef enum {
    UI_HOME_GRID,       // paged 3x2 tiles
    UI_HOME_BENTO,      // tiles of three sizes, 4x3 cells a page
    UI_HOME_PEARL,      // a wide My Badge card over Wi-Fi and Quests, with artwork
    UI_HOME_ICONS,
    UI_HOME_LIST,
    UI_HOME_DIAL,
    UI_HOME_RPG,
    UI_HOME_BLOCKS,     // the Bold blocks page: a block per app, each its own colour (core/home/home_blocks.c)
    UI_HOME_CONTROLS,   // round controls and glass tiles over the wallpaper, no status bar (core/home/home_controls.c)
    UI_HOME_EPAPER,     // a printed page: My Badge as a column, a numbered list, no status bar (core/home/home_epaper.c)
    UI_HOME_TERM,       // a console listing under a prompt, no status bar (core/home/home_term.c)
    UI_HOME_LUMEN,      // the pearl arrangement at night, then pages of glowing icon tiles (core/home/home_lumen.c)
    UI_HOME_STREAM,     // a feature banner over a row of posters (core/home/home_stream.c)
} ui_home_t;

typedef struct {
    uint8_t id;                 // saved in NVS: fixed for good
    const char *name;           // "Classic Dark"
    const char *blurb;          // one line for the Themes app
    uint32_t pal[UI_P_COUNT];
    uint32_t focus_bg, focus_fg;    // the selected tile or key, toasts
    uint32_t accent;            // scrollbars, dialog and input borders
    uint32_t scrim;             // dims the screen behind a dialog
    uint32_t grad_a, grad_b;    // the gradient bar, left to right (the brand's purple to green)
    uint8_t radius;             // panels
    uint8_t tile_radius;        // home tiles
    uint8_t border_w;           // panels and tiles
    uint8_t shadow;             // hard offset shadow in the border colour, px (0 = none)
    lv_opa_t panel_opa;         // panel fill: under LV_OPA_COVER over a wallpaper
    lv_opa_t sheet_opa;         // an app's full-area background over a wallpaper
    bool light;                 // dark ink on a light ground: QR codes, logos, game_ink()
    bool mono;                  // body text in the mono face
    uint8_t focus;              // ui_focus_t
    uint8_t bar;                // ui_bar_t
    uint8_t home;               // ui_home_t
    uint8_t home_opt;           // layout variant, the layout's own meaning
    const lv_image_dsc_t *wallpaper;    // full screen, behind everything, or NULL
} ui_theme_t;

int ui_theme_count(void);
const ui_theme_t *ui_theme_at(int index);       // in the Themes app's order
int ui_theme_index(const ui_theme_t *t);
const ui_theme_t *ui_theme(void);               // the active one
void ui_theme_reload(void);                     // after OS_SET_THEME changed

// A COL_* value (theme.h) in theme t; other values pass through.
uint32_t ui_theme_hex(const ui_theme_t *t, uint32_t col);
