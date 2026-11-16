// Home layout: Grouped list. A dark profile card on top (a pearl, the owner,
// the badge ID and the !LAMPs balance): that is My Badge. Under it one white
// card with a row per app: a small gradient chip with the app's line icon,
// its name, a state where it has one, a chevron. Five rows a page, page dots
// underneath. Positions are the design's, in the app root (screen minus the
// 22 px bar).

#include <stdio.h>
#include <string.h>
#include "home.h"

#define ROWS        5
#define ROW_H       27
#define CARD_X      10
#define CARD_W      300
#define LIST_Y      60
#define INK         0x0B0C12

#define F_OWNER     (&apple_font_14)
#define F_SMALL     (&apple_font_10)
#define F_NAME      (&pearl_font_13)
#define F_STATE     (&apple_font_12)

typedef struct {
    lv_obj_t *obj, *chip, *icon, *name, *state;
    const os_app_t *app;
} row_t;

static row_t s_rows[ROWS];
static lv_obj_t *s_profile, *s_owner, *s_sub, *s_dots;
static int s_list[32], s_n, s_top, s_badge;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
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

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, int x, int y, uint32_t hex, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

// A chip's gradient, top to bottom, by app.
static void chip_colors(const os_app_t *app, uint32_t *top, uint32_t *bottom)
{
    static const struct { const char *id; uint32_t top, bottom; } c[] = {
        {"wifi", 0x5AB0FF, 0x1668E3}, {"quests", 0xFF7CAB, 0xD62F6B}, {"xdex", 0x454857, 0x14151B},
        {"guide", 0xFFBC5C, 0xF07A12}, {"trade", 0x5BE6AA, 0x109C5E}, {"store", 0x6BD2FF, 0x0A84D6},
        {"apps", 0xB49CFF, 0x6B4BE0}, {"espnow", 0x7C8CFF, 0x3A48D8}, {"debug", 0xFFD66B, 0xD69A0A},
        {"about", 0x2A2D3A, 0x0E0F15},
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

// Every app but My Badge is a row; My Badge is the profile card.
static void collect(void)
{
    s_badge = home_find("badge");
    s_n = 0;
    for (int i = 0; i < home_count() && s_n < 32; i++) {
        if (i != s_badge) {
            s_list[s_n++] = i;
        }
    }
}

static int row_of(int app)
{
    for (int i = 0; i < s_n; i++) {
        if (s_list[i] == app) {
            return i;
        }
    }
    return -1;
}

static void info(void)
{
    home_info_t in;
    char buf[80];
    if (s_badge >= 0) {
        home_info(home_app(s_badge), &in);
        set_text(s_owner, in.line);
        if (in.value[0]) {
            snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s !LAMPs", in.extra, in.value);
        } else {
            strlcpy(buf, in.extra, sizeof(buf));
        }
        set_text(s_sub, buf);
    }
    for (int i = 0; i < ROWS; i++) {
        row_t *r = &s_rows[i];
        if (!r->app) {
            continue;
        }
        home_info(r->app, &in);
        set_text(r->state, in.value);
        lv_obj_set_style_text_color(r->state, lv_color_hex(in.value_color == COL_OK ? 0x10804E : 0x9A3C00), 0);
    }
}

static void render(bool list)
{
    if (list) {
        collect();
    }
    int sel = home_sel(), r = row_of(sel);
    if (r >= 0) {
        s_top = r / ROWS * ROWS;        // whole pages, as the dots say
    }
    if (s_top >= s_n) {
        s_top = 0;
    }
    for (int i = 0; i < ROWS; i++) {
        row_t *row = &s_rows[i];
        int idx = s_top + i;
        const os_app_t *app = idx < s_n ? home_app(s_list[idx]) : NULL;
        lv_obj_set_hidden(row->obj, !app);
        if (app != row->app) {
            row->app = app;
            if (row->icon) {
                lv_obj_delete(row->icon);
                row->icon = NULL;
            }
            if (app) {
                uint32_t top, bottom;
                chip_colors(app, &top, &bottom);
                lv_obj_set_style_bg_color(row->chip, lv_color_hex(top), 0);
                lv_obj_set_style_bg_grad_color(row->chip, lv_color_hex(bottom), 0);
                row->icon = home_line_icon(row->chip, app, 0xFFFFFF, 12);
                lv_obj_center(row->icon);
                lv_label_set_text(row->name, app->name);
            }
        }
        if (app) {
            lv_obj_set_style_bg_opa(row->obj, s_list[idx] == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
    // My Badge selected: a ring round the profile card.
    lv_obj_set_style_outline_width(s_profile, sel == s_badge ? 2 : 0, 0);
    home_dots_set(s_dots, (s_n + ROWS - 1) / ROWS, s_top / ROWS);
    info();
}

static void enter(lv_obj_t *root)
{
    // The profile card.
    s_profile = rect(root, CARD_X, 3, CARD_W, 50, 0x2A2D3A, 14);
    lv_obj_set_style_bg_grad_color(s_profile, lv_color_hex(0x0E0F15), 0);
    lv_obj_set_style_bg_grad_dir(s_profile, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_outline_color(s_profile, lv_color_hex(0x5A6BFF), 0);
    lv_obj_set_style_outline_pad(s_profile, 1, 0);
    lv_obj_t *orb = lv_image_create(s_profile);
    lv_image_set_src(orb, &img_list_orb);
    lv_obj_set_pos(orb, 12, 8);
    s_owner = text(s_profile, F_OWNER, 56, 10, 0xFFFFFF, "");
    lv_obj_set_width(s_owner, 214);
    lv_label_set_long_mode(s_owner, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_owner, lv_font_get_line_height(F_OWNER));
    s_sub = text(s_profile, F_SMALL, 56, 29, 0xC4C8D6, "");
    lv_obj_t *go = text(s_profile, &lv_font_montserrat_12, 0, 0, 0xFFFFFF, LV_SYMBOL_RIGHT);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -12, 0);

    // The list card.
    lv_obj_t *card = rect(root, CARD_X, LIST_Y, CARD_W, ROWS * ROW_H, 0xFFFFFF, 14);
    lv_obj_set_style_clip_corner(card, true, 0);
    for (int i = 0; i < ROWS; i++) {
        row_t *r = &s_rows[i];
        memset(r, 0, sizeof(*r));
        r->obj = rect(card, 0, i * ROW_H, CARD_W, ROW_H, 0xE6E6EE, 0);
        if (i) {
            rect(card, 37, i * ROW_H, CARD_W - 37, 1, 0xE4E4EB, 0);
        }
        r->chip = rect(r->obj, 9, 4, 19, 19, 0x9AA0B4, 6);
        lv_obj_set_style_bg_grad_dir(r->chip, LV_GRAD_DIR_VER, 0);
        r->name = text(r->obj, F_NAME, 37, 6, INK, "");
        r->state = text(r->obj, F_STATE, 0, 0, 0x9A3C00, "");
        lv_obj_align(r->state, LV_ALIGN_RIGHT_MID, -28, 0);
        lv_obj_t *chev = text(r->obj, &lv_font_montserrat_12, 0, 0, 0x8A8D9C, LV_SYMBOL_RIGHT);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -11, 0);
    }
    s_dots = home_dots(root, 204);
    s_top = 0;
    render(true);
}

// UP and DOWN along the profile card and the rows; LEFT and RIGHT turn the page.
static void key(os_key_t k, bool repeat)
{
    int sel = home_sel(), r = row_of(sel);
    if (k == OS_KEY_LEFT || k == OS_KEY_RIGHT) {
        int base = (r < 0 ? s_top : r) / ROWS * ROWS + (k == OS_KEY_RIGHT ? ROWS : -ROWS);
        if (base < 0 || base >= s_n) {
            if (repeat) {
                return;
            }
            base = base < 0 ? (s_n - 1) / ROWS * ROWS : 0;
        }
        if (base < s_n) {
            home_select(s_list[base]);
        }
        return;
    }
    int d = k == OS_KEY_DOWN ? 1 : -1;
    if (r < 0) {
        // On the profile card: down to the page's first row, up to its last.
        int n = d > 0 ? s_top : s_top + ROWS - 1;
        if (s_n) {
            home_select(s_list[n < s_n ? n : s_n - 1]);
        }
        return;
    }
    int n = r + d;
    if ((n < s_top || n >= s_top + ROWS || n >= s_n) && s_badge >= 0) {
        home_select(s_badge);           // off either end of the page: the card
    } else if (n >= 0 && n < s_n) {
        home_select(s_list[n]);
    }
}

const home_layout_t home_list = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
