// Home layout: the Streaming theme. A feature banner for the game (the
// Hitchhiker's Guide to Solana: Play opens the Guide, My Badge the badge
// screen) over one row of posters: the Lua apps installed on the card, then
// the built-in apps. The tabs beside the logo in the status bar filter the
// row (Home: everything, Games, Lua apps).
//
// Three places take the arrows: the tabs, the banner's two buttons and the
// row. Up and Down move between them, Left and Right inside one. Every app's
// poster is drawn from its icon and accent colour.
// Text is in the design's faces: Bebas Neue (capitals only) and Geist.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "home.h"
#include "core/ui_stream.h"
#include "lua/lua_host.h"
#include "lua/lua_pkg.h"

#define MARGIN      14
#define POSTER_W    51
#define POSTER_H    64
#define POSTER_GAP  7
#define ROW_Y       147
#define ROW_SHOWN   6           // posters built at a time (the sixth runs off the edge, as in the design)
#define LUA_MAX     16
#define ITEMS_MAX   (32 + LUA_MAX)

enum { ZONE_TABS, ZONE_HERO, ZONE_ROW };
enum { TAB_HOME, TAB_GAMES, TAB_LUA, TAB_COUNT };

typedef struct {
    bool lua;
    uint8_t idx;        // into s_pkgs, or the home list
} item_t;

static lv_obj_t *s_box, *s_banner, *s_tabs, *s_tab[TAB_COUNT];
static lv_obj_t *s_lamps, *s_count, *s_btn[2], *s_row_title, *s_row, *s_empty, *s_posters[ROW_SHOWN];
static item_t s_items[ITEMS_MAX];
static int s_n, s_zone = ZONE_HERO, s_tab_sel, s_btn_sel, s_pos, s_first, s_built = -1;

// The card scan runs on its own task (a directory walk); info() takes it.
static lua_pkg_t *s_pkgs, *s_inbox;
static int s_npkgs, s_inbox_n;
static volatile int s_scan;         // 0 idle, 1 running, 2 results waiting

static void scan_task(void *arg)
{
    lua_pkg_t *p = malloc(LUA_MAX * sizeof(*p));
    int n = p ? lua_pkg_scan(p, LUA_MAX) : 0;
    s_inbox = p;
    s_inbox_n = n < 0 ? 0 : n;
    s_scan = 2;
    vTaskDelete(NULL);
}

static void scan_start(void)
{
    if (s_scan == 0) {
        s_scan = 1;
        if (xTaskCreate(scan_task, "home_scan", 4096, NULL, 3, NULL) != pdPASS) {
            s_scan = 0;
        }
    }
}

static bool is_game(const os_app_t *app)
{
    static const char *const games[] = {"guide", "xdex", "trade", "quests"};
    for (int i = 0; i < (int)(sizeof(games) / sizeof(games[0])); i++) {
        if (!strcmp(app->id, games[i])) {
            return true;
        }
    }
    return false;
}

// The row for the selected tab: Lua apps first, then the built-in ones.
static void collect(void)
{
    s_n = 0;
    for (int i = 0; i < s_npkgs && s_n < ITEMS_MAX; i++) {
        if (s_tab_sel != TAB_GAMES || !strncmp(s_pkgs[i].category, "game", 4)) {
            s_items[s_n++] = (item_t){true, (uint8_t)i};
        }
    }
    for (int i = 0; i < home_count() && s_n < ITEMS_MAX && s_tab_sel != TAB_LUA; i++) {
        if (s_tab_sel == TAB_HOME || is_game(home_app(i))) {
            s_items[s_n++] = (item_t){false, (uint8_t)i};
        }
    }
    if (s_pos >= s_n) {
        s_pos = s_n ? s_n - 1 : 0;
    }
    if (s_zone == ZONE_ROW && !s_n) {
        s_zone = ZONE_HERO;
    }
    s_built = -1;
}

// A poster for the row (core/ui_stream.c draws it).
static lv_obj_t *poster(lv_obj_t *parent, const item_t *it, int x)
{
    lv_obj_t *p;
    if (it->lua) {
        p = ui_stream_poster(parent, s_pkgs[it->idx].name, s_pkgs[it->idx].symbol, NULL, s_pkgs[it->idx].accent, true);
    } else {
        const os_app_t *app = home_app(it->idx);
        p = ui_stream_poster(parent, app->name, app->symbol, app->icon, ui_theme_hex(ui_theme(), app->accent), false);
    }
    lv_obj_set_pos(p, x, 3);
    return p;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void info(void)
{
    if (s_scan == 2) {
        free(s_pkgs);
        s_pkgs = s_inbox;
        s_npkgs = s_inbox_n;
        s_inbox = NULL;
        s_scan = 0;
        collect();
        home_layout_t const *self = &home_stream;
        self->render(false);
    }
    char text[40];
    int badge = home_find("badge");
    home_info_t in = {0};
    if (badge >= 0) {
        home_info(home_app(badge), &in);
    }
    if (in.value[0]) {
        snprintf(text, sizeof(text), "%s !LAMPs", in.value);
    } else {
        strlcpy(text, in.line[0] ? in.line : "My Badge", sizeof(text));
    }
    set_text(s_lamps, text);
    lv_obj_set_style_text_color(s_lamps, ui_col(in.value[0] ? COL_LAMPS : COL_DIM), 0);
    snprintf(text, sizeof(text), "%d apps", home_count() + s_npkgs);
    set_text(s_count, text);
}

static void render(bool list)
{
    if (list) {
        collect();
    }
    // The row: the posters round the selected one.
    if (s_pos < s_first) {
        s_first = s_pos;
    } else if (s_pos > s_first + ROW_SHOWN - 2) {
        s_first = s_pos - (ROW_SHOWN - 2);
    }
    if (s_first > s_n - (ROW_SHOWN - 1)) {
        s_first = s_n > ROW_SHOWN - 1 ? s_n - (ROW_SHOWN - 1) : 0;
    }
    if (s_built != s_first) {
        s_built = s_first;
        lv_obj_clean(s_row);
        for (int i = 0; i < ROW_SHOWN; i++) {
            int it = s_first + i;
            s_posters[i] = it < s_n ? poster(s_row, &s_items[it], MARGIN + i * (POSTER_W + POSTER_GAP)) : NULL;
        }
        static const char *const titles[TAB_COUNT] = {"Installed on this badge", "Games", "Lua apps"};
        set_text(s_row_title, titles[s_tab_sel]);
        lv_obj_set_hidden(s_empty, s_n > 0);
    }
    for (int i = 0; i < ROW_SHOWN; i++) {
        if (s_posters[i]) {
            ui_stream_ring(s_posters[i], s_zone == ZONE_ROW && s_first + i == s_pos);
        }
    }
    for (int i = 0; i < 2; i++) {
        ui_stream_ring(s_btn[i], s_zone == ZONE_HERO && s_btn_sel == i);
    }
    if (s_tabs) {
        for (int i = 0; i < TAB_COUNT; i++) {
            bool on = i == s_tab_sel;
            lv_obj_set_style_text_color(s_tab[i], ui_col(on ? COL_TEXT : COL_DIM), 0);
            lv_obj_set_style_text_font(s_tab[i], on ? &stream_bold_11 : &pearl_font_11, 0);
            lv_obj_set_style_border_width(s_tab[i], on && s_zone == ZONE_TABS ? 2 : 0, 0);
        }
    }
}

static void tabs_gone(lv_event_t *e)
{
    s_tabs = NULL;
}

// The banner and the tabs live outside the app root (behind and in the
// status bar): they go when this layout's own objects do.
static void box_gone(lv_event_t *e)
{
    if (s_tabs) {
        lv_obj_delete(s_tabs);
    }
    lv_obj_delete(s_banner);
    s_box = NULL;
}

static void enter(lv_obj_t *root)
{
    s_banner = lv_image_create(lv_screen_active());
    lv_image_set_src(s_banner, &img_stream_hero);
    lv_obj_move_to_index(s_banner, 0);      // behind the status bar and the app root

    s_tabs = lv_obj_create(os_status_bar());
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, 200, OS_STATUS_H);
    lv_obj_set_pos(s_tabs, 36, 0);
    lv_obj_set_flex_flow(s_tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_tabs, 10, 0);
    lv_obj_add_event_cb(s_tabs, tabs_gone, LV_EVENT_DELETE, NULL);
    static const char *const names[TAB_COUNT] = {"Home", "Games", "Lua apps"};
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tab[i] = ui_label(s_tabs, &pearl_font_11, COL_DIM, names[i]);
        lv_obj_set_style_pad_bottom(s_tab[i], 2, 0);
        lv_obj_set_style_border_side(s_tab[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(s_tab[i], ui_accent(), 0);
    }

    s_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_scrollable(s_box, false);
    lv_obj_add_event_cb(s_box, box_gone, LV_EVENT_DELETE, NULL);

    // The banner's text, at the design's offsets below the status bar.
    lv_obj_t *tick = lv_obj_create(s_box);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, MARGIN, 6);
    lv_obj_set_size(tick, 2, 9);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tick, ui_accent(), 0);
    lv_obj_t *l = ui_stream_caps_label(s_box, "GAME \xC2\xB7 ON THIS BADGE");
    lv_obj_set_pos(l, MARGIN + 7, 6);
    l = ui_label(s_box, &stream_disp_14, COL_TEXT, "THE HITCHHIKER'S");
    lv_obj_set_style_text_color(l, lv_color_hex(0xE4D9FF), 0);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_set_pos(l, MARGIN, 22);
    l = ui_label(s_box, &stream_disp_30, COL_TEXT, "GUIDE TO SOLANA");
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_pos(l, MARGIN, 38);

    lv_obj_t *meta = lv_obj_create(s_box);
    lv_obj_remove_style_all(meta);
    lv_obj_set_pos(meta, MARGIN, 72);
    lv_obj_set_size(meta, 220, 14);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta, 8, 0);
    s_lamps = ui_label(meta, &stream_bold_11, COL_LAMPS, "");
    char ver[20];
    strlcpy(ver, esp_app_get_description()->version, sizeof(ver));
    ver[strcspn(ver, "-+ ")] = 0;           // "2.1.0", not the git describe tail
    l = ui_stream_caps_label(meta, ver);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_white(), 0);
    lv_obj_set_style_border_opa(l, 140, 0);
    lv_obj_set_style_radius(l, 2, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    s_count = ui_label(meta, &pearl_font_11, COL_DIM, "");

    s_btn[0] = ui_stream_button(s_box, LV_SYMBOL_PLAY, "Play", true);
    lv_obj_set_pos(s_btn[0], MARGIN, 96);
    lv_obj_update_layout(s_btn[0]);
    s_btn[1] = ui_stream_button(s_box, NULL, "My Badge", false);
    lv_obj_set_pos(s_btn[1], MARGIN + lv_obj_get_width(s_btn[0]) + 8, 96);

    s_row_title = ui_label(s_box, &stream_bold_11, COL_TEXT, "");
    lv_obj_set_pos(s_row_title, MARGIN, 129);
    s_row = lv_obj_create(s_box);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_pos(s_row, 0, ROW_Y - 3);     // room for the focus ring round a poster
    lv_obj_set_size(s_row, OS_ROOT_W, POSTER_H + 6);
    lv_obj_set_scrollable(s_row, false);
    s_empty = ui_label(s_box, &pearl_font_11, COL_DIM, "Nothing here yet. Get apps in the App Store.");
    lv_obj_set_pos(s_empty, MARGIN, ROW_Y + 8);

    scan_start();
    render(true);
    info();
}

static void key(os_key_t k, bool repeat)
{
    int d = k == OS_KEY_RIGHT || k == OS_KEY_DOWN ? 1 : -1;
    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        int z = s_zone + d;
        if (z == ZONE_ROW && !s_n) {
            z = s_zone;
        }
        s_zone = z < ZONE_TABS ? ZONE_TABS : z > ZONE_ROW ? ZONE_ROW : z;
        return;
    }
    if (s_zone == ZONE_TABS) {
        int t = s_tab_sel + d;
        if (t >= 0 && t < TAB_COUNT) {
            s_tab_sel = t;
            s_pos = s_first = 0;
            collect();
        }
    } else if (s_zone == ZONE_HERO) {
        s_btn_sel = d > 0 ? 1 : 0;
    } else if (s_n) {
        int n = s_pos + d;
        s_pos = n < 0 ? (repeat ? 0 : s_n - 1) : n >= s_n ? (repeat ? s_n - 1 : 0) : n;
    }
}

static bool select(void)
{
    if (s_zone == ZONE_HERO) {
        os_launch_id(s_btn_sel ? "badge" : "guide");
    } else if (s_zone == ZONE_ROW && s_pos < s_n) {
        const item_t *it = &s_items[s_pos];
        if (it->lua) {
            lua_host_launch(s_pkgs[it->idx].id);
        } else {
            os_launch(home_app(it->idx));
        }
    } else if (s_zone == ZONE_TABS) {
        s_zone = s_n ? ZONE_ROW : ZONE_HERO;
        render(false);
    }
    return true;
}

const home_layout_t home_stream = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
    .select = select,
};
