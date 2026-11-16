// Home screen: every registered app (but those whose visible() says no, e.g.
// Beacon on a badge that isn't one), drawn by the active theme's layout
// (core/home/). This file keeps the list, the selection and what the tiles
// can say about their apps; a layout only draws and moves the selection.

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "os.h"
#include "os_cloud.h"
#include "os_mesh.h"
#include "os_wifi.h"
#include "ui.h"
#include "home/home.h"

#define INFO_EVERY  (1000 / OS_TICK_MS)

static const os_app_t *s_list[32];          // the apps shown, in registry order
static int s_count, s_sel, s_ticks;
static const home_layout_t *s_lay;
static char s_saved[33];                    // the saved network, read on enter (an NVS read)

static bool listed(const os_app_t *app)
{
    return !app->visible || app->visible();
}

// Themes drawn from a design that puts My Badge first list the apps in the
// design's order; the classic grid, the pearl homes (their own order) and
// Streaming keep the registry's.
static bool design_order(void)
{
    switch (ui_theme()->home) {
    case UI_HOME_GRID:
    case UI_HOME_PEARL:
    case UI_HOME_STREAM:
        return false;
    default:
        return true;
    }
}

// Rebuilds the list; true if it changed. The selection stays on its app.
static bool collect(void)
{
    const os_app_t *sel = s_count ? s_list[s_sel < s_count ? s_sel : 0] : NULL;
    const os_app_t *next[32];
    int n = 0;
    for (int i = 0; i < os_app_count && n < 32; i++) {
        if (listed(os_apps[i])) {
            next[n++] = os_apps[i];
        }
    }
    if (design_order()) {
        // The designs' order: these lead, the rest follow in registry order.
        static const char *const lead[] = {"badge", "wifi", "quests", "xdex", "guide", "trade", "store", "apps",
                                            "settings", "espnow", "debug"};
        const os_app_t *sorted[32];
        int m = 0;
        for (int l = 0; l < (int)(sizeof(lead) / sizeof(lead[0])); l++) {
            for (int i = 0; i < n; i++) {
                if (next[i] && !strcmp(next[i]->id, lead[l])) {
                    sorted[m++] = next[i];
                    next[i] = NULL;
                }
            }
        }
        for (int i = 0; i < n; i++) {
            if (next[i]) {
                sorted[m++] = next[i];
            }
        }
        memcpy(next, sorted, n * sizeof(next[0]));
    }
    if (n == s_count && !memcmp(next, s_list, n * sizeof(next[0]))) {
        return false;
    }
    memcpy(s_list, next, n * sizeof(next[0]));
    s_count = n;
    s_sel = 0;
    for (int i = 0; i < n; i++) {
        if (s_list[i] == sel) {
            s_sel = i;
        }
    }
    return true;
}

int home_count(void)
{
    return s_count;
}

const os_app_t *home_app(int index)
{
    return index >= 0 && index < s_count ? s_list[index] : NULL;
}

int home_sel(void)
{
    return s_sel;
}

void home_select(int index)
{
    s_sel = index < 0 ? 0 : index >= s_count ? s_count - 1 : index;
}

int home_step(int index, int delta, bool repeat)
{
    int n = index + delta;
    if (n < 0) {
        return repeat ? 0 : s_count - 1;
    }
    if (n >= s_count) {
        return repeat ? s_count - 1 : 0;
    }
    return n;
}

int home_find(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_list[i]->id, id)) {
            return i;
        }
    }
    return -1;
}

void home_info(const os_app_t *app, home_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *id = app->id;
    if (!strcmp(id, "badge")) {
        os_cloud_info_t *in = lv_malloc(sizeof(*in));
        if (!in) {
            return;
        }
        os_cloud_get_info(in);
        strlcpy(out->extra, in->id, sizeof(out->extra));
        if (in->claimed) {
            strlcpy(out->line, in->owner, sizeof(out->line));
            ui_fmt_num(out->value, sizeof(out->value), in->lamps);
            out->value_color = COL_LAMPS;
        } else {
            strlcpy(out->line, "Not linked yet", sizeof(out->line));
        }
        lv_free(in);
    } else if (!strcmp(id, "wifi")) {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        strlcpy(out->line, w.ssid[0] ? w.ssid : s_saved[0] ? s_saved : "No network saved", sizeof(out->line));
        switch (w.state) {
        case OS_WIFI_CONNECTED:
            strlcpy(out->value, "Connected", sizeof(out->value));
            out->value_color = COL_OK;
            break;
        case OS_WIFI_CONNECTING:
            strlcpy(out->value, "Joining", sizeof(out->value));
            out->value_color = COL_WARN;
            break;
        case OS_WIFI_FAILED:
            strlcpy(out->value, "Can't join", sizeof(out->value));
            out->value_color = COL_ERR;
            break;
        case OS_WIFI_SUSPENDED:
            strlcpy(out->value, "Paused", sizeof(out->value));
            out->value_color = COL_PINK;
            break;
        default:
            strlcpy(out->value, w.enabled ? "" : "Off", sizeof(out->value));
            out->value_color = COL_DIM;
            break;
        }
    } else if (!strcmp(id, "espnow")) {
        snprintf(out->line, sizeof(out->line), "Mesh channel %d", os_mesh_channel());
    } else if (!strcmp(id, "about")) {
        snprintf(out->line, sizeof(out->line), "%.16s " LV_SYMBOL_BULLET " ESP32-C5", esp_app_get_description()->version);
    } else if (!strcmp(id, "settings")) {
        strlcpy(out->line, "Brightness, update", sizeof(out->line));
    } else if (!strcmp(id, "themes")) {
        strlcpy(out->line, ui_theme()->name, sizeof(out->line));
    } else if (!strcmp(id, "store")) {
        strlcpy(out->line, "Get Lua apps", sizeof(out->line));
    } else if (!strcmp(id, "apps")) {
        strlcpy(out->line, "Your Lua apps", sizeof(out->line));
    } else if (!strcmp(id, "debug")) {
        strlcpy(out->line, "Pins, LEDs, SD", sizeof(out->line));
    } else if (!strcmp(id, "quests")) {
        strlcpy(out->line, "Find a beacon", sizeof(out->line));
    }
}

lv_obj_t *home_icon(lv_obj_t *parent, const os_app_t *app, uint32_t color)
{
    if (app->symbol) {
        return ui_label(parent, &lv_font_montserrat_20, color ? color : app->accent, app->symbol);
    }
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, app->icon);
    return img;
}

lv_obj_t *home_line_icon(lv_obj_t *parent, const os_app_t *app, uint32_t hex, int px)
{
    static const struct { const char *id; const lv_image_dsc_t *img; } icons[] = {
        {"badge", &img_line_badge}, {"wifi", &img_line_wifi}, {"quests", &img_line_quests}, {"xdex", &img_line_xdex},
        {"guide", &img_line_guide}, {"trade", &img_line_trade}, {"store", &img_line_store}, {"apps", &img_line_apps},
        {"settings", &img_line_settings}, {"espnow", &img_line_espnow}, {"debug", &img_line_debug},
    };
    lv_obj_t *img = lv_image_create(parent);
    if (!strcmp(app->id, "about")) {
        lv_image_set_src(img, &img_logomark_16);
        lv_image_set_scale(img, 256 * px / 18);
        return img;
    }
    const lv_image_dsc_t *src = &img_line_app;
    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
        if (!strcmp(app->id, icons[i].id)) {
            src = icons[i].img;
        }
    }
    lv_image_set_src(img, src);
    lv_image_set_scale(img, 256 * px / 24);
    lv_obj_set_style_image_recolor(img, lv_color_hex(hex), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

void home_gradient(const os_app_t *app, uint32_t *top, uint32_t *bottom)
{
    static const struct { const char *id; uint32_t top, bottom; } c[] = {
        {"badge", 0x9C8CFF, 0x5B43E0}, {"wifi", 0x5AB0FF, 0x1668E3}, {"quests", 0xFF7CAB, 0xD62F6B},
        {"xdex", 0x454857, 0x14151B}, {"guide", 0xFFBC5C, 0xF07A12}, {"trade", 0x5BE6AA, 0x109C5E},
        {"store", 0x6BD2FF, 0x0A84D6}, {"apps", 0xCE8CFF, 0x7A3FD0}, {"settings", 0xA6ABBB, 0x5B6072},
        {"espnow", 0x7C95FF, 0x2E4FD6}, {"debug", 0xFF8576, 0xD43A2C}, {"about", 0x2A2D3A, 0x0E0F15},
    };
    *top = 0x9AA0B4;
    *bottom = 0x5A6078;
    for (int i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++) {
        if (!strcmp(app->id, c[i].id)) {
            *top = c[i].top;
            *bottom = c[i].bottom;
        }
    }
}

void home_line_icon_color(lv_obj_t *icon, uint32_t hex)
{
    lv_obj_set_style_image_recolor(icon, lv_color_hex(hex), 0);
}

void home_icon_focus(lv_obj_t *icon, const os_app_t *app, bool on, uint32_t color)
{
    if (app->symbol) {
        lv_obj_set_style_text_color(icon, on ? ui_focus_fg() : ui_col(color ? color : app->accent), 0);
    } else {
        // A colour logo on the focused tile is low contrast; show it as a
        // solid shape in the tile's text colour instead.
        lv_obj_set_style_image_recolor(icon, ui_focus_fg(), 0);
        lv_obj_set_style_image_recolor_opa(icon, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void home_text_focus(lv_obj_t *label, bool on, uint32_t color)
{
    lv_obj_set_style_text_color(label, on ? ui_focus_fg() : ui_col(color), 0);
}

bool home_fills(void)
{
    int f = ui_theme()->focus;
    return f == UI_FOCUS_FILL || f == UI_FOCUS_BAR;
}

lv_obj_t *home_tile(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *t = ui_panel(parent, x, y, w, h);
    lv_obj_set_style_radius(t, ui_theme()->tile_radius, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    return t;
}

void home_tile_focus(lv_obj_t *tile, bool on)
{
    const ui_theme_t *t = ui_theme();
    bool fill = on && home_fills();
    lv_obj_set_style_bg_color(tile, fill ? ui_focus_bg() : ui_col(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(tile, fill ? LV_OPA_COVER : t->panel_opa, 0);
    if (home_fills()) {
        // Classic: the filled tile drops its border (a hard shadow keeps it).
        lv_obj_set_style_border_width(tile, fill && !t->shadow ? 0 : t->border_w, 0);
    } else {
        lv_obj_set_style_border_color(tile, on ? ui_focus_bg() : ui_col(COL_BORDER), 0);
        lv_obj_set_style_border_width(tile, on && t->border_w < 2 ? 2 : t->border_w, 0);
    }
}

lv_obj_t *home_dots(lv_obj_t *parent, int y)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 120, 8);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(d, 4, 0);
    return d;
}

void home_dots_set(lv_obj_t *dots, int pages, int page)
{
    lv_obj_clean(dots);
    lv_obj_set_hidden(dots, pages < 2);
    for (int p = 0; p < pages; p++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, p == page ? 16 : 6, 6);
        lv_obj_set_style_radius(d, ui_radius() ? 3 : 0, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, p == page ? ui_focus_bg() : ui_col(COL_BORDER), 0);
    }
}

// A home layout's own header in place of the status bar's: a cover over the
// bar, there until `owner` (an object of the layout) is deleted. The layout
// draws its header on it, in screen coordinates.
static lv_obj_t *s_header;

static void header_gone(lv_event_t *e)
{
    s_header = NULL;
}

static void header_owner_gone(lv_event_t *e)
{
    if (s_header) {
        lv_obj_delete(s_header);
    }
}

lv_obj_t *home_header(lv_obj_t *owner)
{
    s_header = lv_obj_create(os_status_bar());
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, OS_ROOT_W, OS_STATUS_H);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_header, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(s_header, false);
    lv_obj_add_event_cb(s_header, header_gone, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(owner, header_owner_gone, LV_EVENT_DELETE, NULL);
    return s_header;
}

// A layout that takes the whole screen, status bar included: an opaque sheet
// over everything, there until `owner` (an object of the layout in the app
// root) is deleted. Its children are in screen coordinates.
static lv_obj_t *s_full;

static void full_gone(lv_event_t *e)
{
    s_full = NULL;
}

static void full_owner_gone(lv_event_t *e)
{
    if (s_full) {
        lv_obj_delete(s_full);
    }
}

lv_obj_t *home_fullscreen(lv_obj_t *owner)
{
    s_full = lv_obj_create(lv_obj_get_parent(os_status_bar()));
    lv_obj_remove_style_all(s_full);
    lv_obj_set_size(s_full, OS_ROOT_W, OS_STATUS_H + OS_ROOT_H);
    lv_obj_set_style_bg_opa(s_full, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_full, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(s_full, false);
    lv_obj_add_event_cb(s_full, full_gone, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(owner, full_owner_gone, LV_EVENT_DELETE, NULL);
    return s_full;
}

static const home_layout_t *layout(void)
{
    switch (ui_theme()->home) {
    case UI_HOME_LIST:  return &home_list;
    case UI_HOME_BENTO: return &home_bento;
    case UI_HOME_PEARL: return &home_pearl;
    case UI_HOME_RPG:   return &home_rpg;
    case UI_HOME_STREAM: return &home_stream;
    case UI_HOME_LUMEN: return &home_lumen;
    case UI_HOME_ICONS: return &home_icons;
    case UI_HOME_DIAL:  return &home_dial;
    case UI_HOME_TERM:  return &home_term;
    case UI_HOME_BLOCKS: return &home_blocks;
    case UI_HOME_CONTROLS: return &home_controls;
    case UI_HOME_EPAPER: return &home_epaper;
    default:            return &home_grid;
    }
}

static void enter(lv_obj_t *root)
{
    collect();
    if (s_sel >= s_count) {
        s_sel = 0;
    }
    if (!os_wifi_saved_ssid(s_saved, sizeof(s_saved))) {
        s_saved[0] = 0;
    }
    // A theme seen for the first time opens the way its design does: on My Badge.
    static int theme = -1;
    if (theme != ui_theme()->id && design_order() && home_find("badge") >= 0) {
        s_sel = home_find("badge");
    }
    theme = ui_theme()->id;
    s_ticks = 0;
    s_lay = layout();
    s_lay->enter(root);
}

static bool key(os_key_t k, bool repeat)
{
    switch (k) {
    case OS_KEY_LEFT:
    case OS_KEY_RIGHT:
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        if (s_count) {
            s_lay->key(k, repeat);
            s_lay->render(false);
        }
        return true;
    case OS_KEY_SELECT:
        if (s_lay->select && s_lay->select()) {
            return true;
        }
        if (s_sel < s_count) {
            os_launch(s_list[s_sel]);
        }
        return true;
    default:
        return true;    // Back on the home screen does nothing
    }
}

// An app showing up or going (a badge paired as a beacon) redraws the home
// screen; tiles with live state refresh once a second.
static void tick(void)
{
    if (collect()) {
        s_lay->render(true);
    }
    if (++s_ticks >= INFO_EVERY) {
        s_ticks = 0;
        if (s_lay->info) {
            s_lay->info();
        }
    }
}

const os_app_t os_launcher = {
    .id = "launcher",
    .name = "Apps",
    .on_enter = enter,
    .on_key = key,
    .on_tick = tick,
};
