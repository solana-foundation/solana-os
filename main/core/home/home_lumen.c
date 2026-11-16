// Home layout: Lumen. The first page is the pearl homes' arrangement in a
// night palette: My Badge as a wide card with the !LAMPs balance over Wi-Fi
// and Quests, each card's glass, glow and artwork one image rendered from the
// design (assets/img_lumen_*.c). Every other app follows on pages of ten: two
// rows of five glowing tiles with the design's line icons (assets/img_line_*.c)
// and the name under each. Page dots, no hint bar; text in Geist.

#include <stdio.h>
#include <string.h>
#include "home.h"

#define HERO_X      8
#define HERO_Y      2
#define CARD_Y      116
#define TILE        48
#define TILE_GAP    10
#define GRID_X      20
#define GRID_Y      23
#define ROW_PITCH   90
#define COLS        5
#define PER_PAGE    (2 * COLS)
#define DOTS_Y      204
#define MAX_APPS    32

static lv_obj_t *s_box, *s_dots;
static lv_obj_t *s_cards[3], *s_value, *s_line, *s_chip, *s_wifi_line, *s_wifi_state, *s_wifi_dot, *s_quest_line;
static lv_obj_t *s_tiles[PER_PAGE], *s_names[PER_PAGE];
static int s_order[MAX_APPS], s_n, s_first, s_page;

// My Badge, Wi-Fi and Quests lead (the first page); the rest keep their order.
static void order(void)
{
    static const char *const lead[] = {"badge", "wifi", "quests"};
    s_n = 0;
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

static const lv_image_dsc_t *line_icon(const char *id)
{
    static const struct {
        const char *id;
        const lv_image_dsc_t *img;
    } icons[] = {
        {"badge", &img_line_badge}, {"wifi", &img_line_wifi}, {"quests", &img_line_quests}, {"xdex", &img_line_xdex},
        {"guide", &img_line_guide}, {"trade", &img_line_trade}, {"store", &img_line_store}, {"apps", &img_line_apps},
        {"settings", &img_line_settings}, {"espnow", &img_line_espnow}, {"debug", &img_line_debug},
    };
    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
        if (!strcmp(id, icons[i].id)) {
            return icons[i].img;
        }
    }
    return &img_line_app;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font, uint32_t hex, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_obj_set_style_max_width(l, w, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font) + 1);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void info(void)
{
    home_info_t in;
    if (s_page == 0) {
        int a = home_find("badge");
        if (a >= 0 && s_value) {
            home_info(home_app(a), &in);
            set_text(s_value, in.value);
            set_text(s_line, in.line);
            set_text(s_chip, in.extra);
            lv_obj_set_hidden(s_chip, !in.extra[0]);
            lv_obj_update_layout(s_line);
            lv_obj_align_to(s_chip, s_line, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        }
        a = home_find("wifi");
        if (a >= 0 && s_wifi_line) {
            home_info(home_app(a), &in);
            set_text(s_wifi_line, in.line);
            set_text(s_wifi_state, in.value);
            // The design's states: amber for trouble, the theme's green when joined.
            lv_color_t c = in.value_color == COL_OK ? lv_color_hex(0x7DE0B0)
                         : in.value_color == COL_DIM ? lv_color_hex(0xA8ADC0) : lv_color_hex(0xFFB877);
            lv_obj_set_style_text_color(s_wifi_state, c, 0);
            lv_obj_set_style_bg_color(s_wifi_dot, c, 0);
            lv_obj_set_hidden(s_wifi_dot, !in.value[0]);
        }
        a = home_find("quests");
        if (a >= 0 && s_quest_line) {
            home_info(home_app(a), &in);
            set_text(s_quest_line, in.line);
        }
    }
}

static lv_obj_t *card(const lv_image_dsc_t *img, int x, int y)
{
    lv_obj_t *c = lv_image_create(s_box);
    lv_image_set_src(c, img);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_outline_color(c, lv_color_hex(0xB4C6FF), 0);
    lv_obj_set_style_outline_pad(c, 1, 0);
    return c;
}

static void build(int page)
{
    lv_obj_clean(s_box);
    memset(s_cards, 0, sizeof(s_cards));
    memset(s_tiles, 0, sizeof(s_tiles));
    s_value = s_line = s_chip = s_wifi_line = s_wifi_state = s_wifi_dot = s_quest_line = NULL;
    os_status_title(page ? "Apps" : "Solana OS", 0);
    if (page == 0) {
        if (home_find("badge") >= 0) {
            lv_obj_t *c = s_cards[0] = card(&img_lumen_hero, HERO_X, HERO_Y);
            one_line(c, &pearl_font_11, 0xC4C8D8, 14, 12, 150);
            lv_label_set_text(lv_obj_get_child(c, 0), "!LAMPs balance");
            s_value = one_line(c, &pearl_num_46, 0xFFFFFF, 14, 27, 190);
            lv_obj_set_style_text_letter_space(s_value, -2, 0);
            s_line = one_line(c, &pearl_font_13, 0xECEEF6, 14, 81, 150);
            s_chip = one_line(c, &pearl_mono_9, 0xD4D8E6, 0, 0, 60);
            lv_obj_set_style_bg_opa(s_chip, 30, 0);
            lv_obj_set_style_bg_color(s_chip, lv_color_white(), 0);
            lv_obj_set_style_radius(s_chip, 6, 0);
            lv_obj_set_style_pad_hor(s_chip, 5, 0);
            lv_obj_set_style_pad_ver(s_chip, 1, 0);
            lv_obj_set_height(s_chip, LV_SIZE_CONTENT);
        }
        if (home_find("wifi") >= 0) {
            lv_obj_t *c = s_cards[1] = card(&img_lumen_wifi, 8, CARD_Y);
            lv_label_set_text(one_line(c, &pearl_font_13, 0xFFFFFF, 12, 11, 100), "Wi-Fi");
            s_wifi_line = one_line(c, &pearl_font_11, 0xC4C8D8, 12, 28, 106);
            s_wifi_dot = lv_obj_create(c);
            lv_obj_remove_style_all(s_wifi_dot);
            lv_obj_set_size(s_wifi_dot, 5, 5);
            lv_obj_set_pos(s_wifi_dot, 12, 62);
            lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s_wifi_dot, LV_OPA_COVER, 0);
            s_wifi_state = one_line(c, &pearl_font_11, 0xFFB877, 22, 58, 80);
        }
        if (home_find("quests") >= 0) {
            lv_obj_t *c = s_cards[2] = card(&img_lumen_quest, 163, CARD_Y);
            lv_label_set_text(one_line(c, &pearl_font_13, 0xFFFFFF, 12, 11, 100), "Quests");
            s_quest_line = one_line(c, &pearl_font_11, 0xC4C8D8, 12, 28, 86);
        }
        info();
        return;
    }
    // Two rows of five: a dark tile lit from below in the app's colour, the
    // line icon on it, the name under it.
    int base = s_first + (page - 1) * PER_PAGE;
    for (int i = 0; i < PER_PAGE && base + i < s_n; i++) {
        const os_app_t *app = home_app(s_order[base + i]);
        int x = GRID_X + (i % COLS) * (TILE + TILE_GAP), y = GRID_Y + (i / COLS) * ROW_PITCH;
        lv_obj_t *t = s_tiles[i] = lv_obj_create(s_box);
        lv_obj_remove_style_all(t);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_size(t, TILE, TILE);
        lv_obj_set_style_radius(t, 15, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x23283A), 0);
        lv_color_t glow = lv_color_hex(ui_theme_hex(ui_theme(), app->accent));
        lv_obj_set_style_bg_grad_color(t, lv_color_mix(glow, lv_color_hex(0x10121B), 120), 0);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(t, 120, 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_white(), 0);
        lv_obj_set_style_border_opa(t, 36, 0);
        lv_obj_set_style_shadow_color(t, lv_color_hex(0x6080FF), 0);
        lv_obj_set_scrollable(t, false);
        lv_obj_t *icon;
        if (!strcmp(app->id, "about")) {
            icon = lv_image_create(t);
            lv_image_set_src(icon, &img_logomark_16);
            lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            icon = lv_image_create(t);
            lv_image_set_src(icon, line_icon(app->id));
            lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        }
        lv_obj_center(icon);
        lv_obj_t *n = s_names[i] = lv_label_create(s_box);
        lv_obj_set_style_text_font(n, &pearl_font_11, 0);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(n, TILE + TILE_GAP - 2);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(n, lv_font_get_line_height(&pearl_font_11) + 1);
        lv_obj_set_pos(n, x - (TILE_GAP - 2) / 2, y + TILE + 7);
        // "App Store" and "HW Debug" are the design's "Store" and "Debug".
        const char *name = !strcmp(app->id, "store") ? "Store" : !strcmp(app->id, "debug") ? "Debug" : app->name;
        lv_label_set_text(n, name);
    }
}

static void render(bool list)
{
    if (list) {
        order();
    }
    int pos = pos_of(home_sel()), page = page_of(pos);
    if (list || page != s_page) {
        s_page = page;
        build(page);
        home_dots_set(s_dots, pages(), page);
    }
    if (page == 0) {
        for (int i = 0; i < 3; i++) {
            if (s_cards[i]) {
                lv_obj_set_style_outline_width(s_cards[i], i < s_first && s_order[i] == home_sel() ? 2 : 0, 0);
            }
        }
        return;
    }
    int base = s_first + (page - 1) * PER_PAGE;
    for (int i = 0; i < PER_PAGE; i++) {
        if (!s_tiles[i]) {
            continue;
        }
        bool on = base + i == pos;
        lv_obj_set_style_border_color(s_tiles[i], on ? lv_color_hex(0xFFC79A) : lv_color_white(), 0);
        lv_obj_set_style_border_opa(s_tiles[i], on ? LV_OPA_COVER : 36, 0);
        lv_obj_set_style_shadow_width(s_tiles[i], on ? 14 : 0, 0);
        lv_obj_set_style_shadow_opa(s_tiles[i], on ? 120 : 0, 0);
        lv_obj_set_style_text_color(s_names[i], lv_color_hex(on ? 0xFFFFFF : 0xC4C8D8), 0);
        lv_obj_set_style_text_font(s_names[i], on ? &stream_bold_11 : &pearl_font_11, 0);
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
    int d = k == OS_KEY_DOWN || k == OS_KEY_RIGHT ? 1 : -1;
    if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
        n = pos + d;
        if (n < 0) {
            n = repeat ? 0 : s_n - 1;
        } else if (n >= s_n) {
            n = repeat ? s_n - 1 : 0;
        }
    } else if (pos < s_first) {
        // The first page: My Badge over the two small cards, then the apps.
        if (pos == 0 && d > 0 && s_first > 1) {
            n = 1;
        } else if (pos > 0 && d < 0) {
            n = 0;
        } else if (pos > 0 && d > 0 && s_n > s_first) {
            n = s_first;
        }
    } else {
        int base = s_first + (page_of(pos) - 1) * PER_PAGE, in = pos - base;
        if (d < 0 && in < COLS) {
            n = base > s_first ? pos - COLS : 0;       // up off the top row: the page before
        } else {
            n = pos + d * COLS;
            if (n >= s_n) {
                n = pos;
            }
        }
    }
    home_select(s_order[n]);
}

const home_layout_t home_lumen = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
