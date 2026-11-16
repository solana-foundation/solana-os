// Home layout: a dial. My Badge and Wi-Fi are two cards in the middle; the
// other apps are round buttons down each side, six to a page, each with its
// line icon in its own colour. The buttons carry no names: the status bar's
// title names the selected item, in the focus orange. The selected button is
// solid orange with a ring; a selected card gets the ring. Positions are the
// design's, in the app root (screen minus the 22 px bar).

#include <stdio.h>
#include <string.h>
#include "home.h"

#define SIDE        3
#define PER_PAGE    (SIDE * 2)
#define BTN         50
#define ORANGE      0xFF9F5A
#define CARD_X      69
#define CARD_W      182

#define F_LABEL     (&apple_font_12)
#define F_SMALL     (&apple_font_10)
#define F_NUM       (&apple_num_48)
#define F_ID        (&pearl_mono_9)

typedef struct {
    lv_obj_t *obj, *icon;
    const os_app_t *app;
} btn_t;

static btn_t s_btns[PER_PAGE];
static lv_obj_t *s_badge, *s_wifi, *s_owner, *s_id, *s_lamps, *s_unit, *s_net, *s_state, *s_wicon, *s_wdisc, *s_dots;
static int s_ibadge, s_iwifi, s_page;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x232327), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x131315), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_outline_color(o, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_outline_pad(o, 2, 0);
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

// A button's icon colour, by app.
static uint32_t tint(const os_app_t *app)
{
    static const struct { const char *id; uint32_t hex; } c[] = {
        {"quests", 0xFF9F5A}, {"xdex", 0xFF6FB5}, {"guide", 0xA8F58C}, {"trade", 0x6FDCFF}, {"store", 0xB6ACFF},
        {"apps", 0xFFD66B}, {"espnow", 0x8FA8FF}, {"debug", 0xFF8576}, {"settings", 0xD0D0D8},
    };
    for (int i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++) {
        if (!strcmp(app->id, c[i].id)) {
            return c[i].hex;
        }
    }
    return 0xFFFFFF;
}

// The n-th app that is a button (not one of the two cards), or -1.
static int button_app(int n)
{
    for (int i = 0; i < home_count(); i++) {
        if (i != s_ibadge && i != s_iwifi && n-- == 0) {
            return i;
        }
    }
    return -1;
}

// Which button an app is, counting only the buttons, or -1 for a card.
static int button_of(int index)
{
    if (index == s_ibadge || index == s_iwifi) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < index; i++) {
        n += i != s_ibadge && i != s_iwifi;
    }
    return n;
}

static void info(void)
{
    home_info_t in;
    int sel = home_sel();
    if (s_ibadge >= 0) {
        home_info(home_app(s_ibadge), &in);
        set_text(s_owner, in.line);
        set_text(s_lamps, in.value);
        set_text(s_id, in.extra);
        lv_obj_set_hidden(s_id, !in.extra[0]);
        lv_obj_set_hidden(s_unit, !in.value[0]);
        lv_obj_set_style_outline_width(s_badge, sel == s_ibadge ? 2 : 0, 0);
    }
    if (s_iwifi >= 0) {
        home_info(home_app(s_iwifi), &in);
        bool ok = in.value_color == COL_OK;
        uint32_t hex = ok ? 0x34D17A : 0xFFB870;
        set_text(s_state, in.value[0] ? in.value : "Wi-Fi");
        set_text(s_net, in.line);
        lv_obj_set_style_text_color(s_state, lv_color_hex(hex), 0);
        home_line_icon_color(s_wicon, hex);
        lv_obj_set_style_bg_color(s_wdisc, lv_color_hex(hex), 0);
        lv_obj_set_style_outline_width(s_wifi, sel == s_iwifi ? 2 : 0, 0);
    }
}

static void render(bool list)
{
    if (list) {
        s_ibadge = home_find("badge");
        s_iwifi = home_find("wifi");
        lv_obj_set_hidden(s_badge, s_ibadge < 0);
        lv_obj_set_hidden(s_wifi, s_iwifi < 0);
    }
    int sel = home_sel(), b = button_of(sel);
    int buttons = home_count() - (s_ibadge >= 0) - (s_iwifi >= 0);
    int page = b >= 0 ? b / PER_PAGE : s_page < 0 ? 0 : s_page;
    if (list || page != s_page) {
        s_page = page;
        for (int i = 0; i < PER_PAGE; i++) {
            btn_t *t = &s_btns[i];
            const os_app_t *app = home_app(button_app(page * PER_PAGE + i));
            lv_obj_set_hidden(t->obj, !app);
            if (app == t->app) {
                continue;
            }
            t->app = app;
            if (t->icon) {
                lv_obj_delete(t->icon);
                t->icon = NULL;
            }
            if (app) {
                t->icon = home_line_icon(t->obj, app, tint(app), 24);
                lv_obj_center(t->icon);
            }
        }
        home_dots_set(s_dots, (buttons + PER_PAGE - 1) / PER_PAGE, page);
    }
    for (int i = 0; i < PER_PAGE; i++) {
        btn_t *t = &s_btns[i];
        if (!t->app) {
            continue;
        }
        // The selected button: solid orange, a dark icon, a ring.
        bool on = t->app == home_app(sel);
        lv_obj_set_style_bg_color(t->obj, lv_color_hex(on ? 0xFFB45C : 0x26262B), 0);
        lv_obj_set_style_bg_grad_color(t->obj, lv_color_hex(on ? 0xFF6A2B : 0x151517), 0);
        lv_obj_set_style_outline_width(t->obj, on ? 2 : 0, 0);
        if (strcmp(t->app->id, "about")) {
            home_line_icon_color(t->icon, on ? 0x1A0A00 : tint(t->app));
        }
    }
    const os_app_t *app = home_app(sel);
    os_status_title(app ? app->name : "Solana OS", COL_ORANGE);
    info();
}

static void enter(lv_obj_t *root)
{
    for (int i = 0; i < PER_PAGE; i++) {
        btn_t *t = &s_btns[i];
        memset(t, 0, sizeof(*t));
        t->obj = card(root, i < SIDE ? 9 : 261, 9 + (i % SIDE) * 63, BTN, BTN, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(t->obj, lv_color_hex(0x26262B), 0);
        lv_obj_set_style_bg_grad_color(t->obj, lv_color_hex(0x151517), 0);
    }

    // My Badge: the balance, the owner, the badge ID as a chip.
    s_badge = card(root, CARD_X, 6, CARD_W, 126, 20);
    text(s_badge, F_LABEL, 13, 11, 0x34D17A, "My Badge");
    s_id = text(s_badge, F_ID, 0, 0, 0xD0D0D8, "");
    lv_obj_set_style_bg_color(s_id, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_id, 26, 0);
    lv_obj_set_style_radius(s_id, 6, 0);
    lv_obj_set_style_pad_hor(s_id, 5, 0);
    lv_obj_set_style_pad_ver(s_id, 3, 0);
    lv_obj_align(s_id, LV_ALIGN_TOP_RIGHT, -13, 11);
    s_lamps = text(s_badge, F_NUM, 13, 33, 0xFFFFFF, "");
    lv_obj_set_style_text_letter_space(s_lamps, -2, 0);
    s_unit = text(s_badge, F_SMALL, 13, 83, 0xB4B4BE, "!LAMPs");
    s_owner = text(s_badge, F_LABEL, 13, 100, 0xFFFFFF, "");
    lv_obj_set_width(s_owner, CARD_W - 26);
    lv_label_set_long_mode(s_owner, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_owner, lv_font_get_line_height(F_LABEL));

    // Wi-Fi: the state in its colour, the network under it.
    s_wifi = card(root, CARD_X, 138, CARD_W, 50, 18);
    s_wdisc = lv_obj_create(s_wifi);
    lv_obj_remove_style_all(s_wdisc);
    lv_obj_set_pos(s_wdisc, 13, 11);
    lv_obj_set_size(s_wdisc, 28, 28);
    lv_obj_set_style_radius(s_wdisc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_wdisc, 46, 0);
    lv_obj_set_scrollable(s_wdisc, false);
    s_wicon = lv_image_create(s_wifi);
    lv_image_set_src(s_wicon, &img_line_wifi);
    lv_image_set_scale(s_wicon, 256 * 16 / 24);
    lv_obj_set_style_image_recolor_opa(s_wicon, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_wicon, 15, 13);
    s_state = text(s_wifi, F_LABEL, 50, 10, 0xFFB870, "");
    s_net = text(s_wifi, F_SMALL, 50, 26, 0xB4B4BE, "");
    lv_obj_set_width(s_net, CARD_W - 63);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_net, lv_font_get_line_height(F_SMALL));

    s_dots = home_dots(root, 201);
    s_page = -1;
    render(true);
}

// Left column, the two cards, right column: LEFT and RIGHT cross between
// them, UP and DOWN move within one (and on to the next page of buttons).
static void key(os_key_t k, bool repeat)
{
    int sel = home_sel(), b = button_of(sel);
    int buttons = home_count() - (s_ibadge >= 0) - (s_iwifi >= 0);
    bool vert = k == OS_KEY_UP || k == OS_KEY_DOWN;
    int d = k == OS_KEY_DOWN || k == OS_KEY_RIGHT ? 1 : -1;
    if (b < 0) {
        if (vert) {
            int other = sel == s_ibadge ? s_iwifi : s_ibadge;
            if (other >= 0) {
                home_select(other);
            }
        } else if (buttons) {
            int first = (s_page < 0 ? 0 : s_page) * PER_PAGE + (d > 0 ? SIDE : 0) + (sel == s_iwifi ? SIDE - 1 : 0);
            home_select(button_app(first < buttons ? first : buttons - 1));
        }
        return;
    }
    if (vert) {
        int n = b + d;
        if (n < 0) {
            n = repeat ? 0 : buttons - 1;
        } else if (n >= buttons) {
            n = repeat ? buttons - 1 : 0;
        }
        home_select(button_app(n));
        return;
    }
    bool right = b % PER_PAGE >= SIDE;
    if (right == (d > 0)) {
        return;         // already on that side
    }
    int row = b % SIDE;
    int mid = row == SIDE - 1 && s_iwifi >= 0 ? s_iwifi : s_ibadge >= 0 ? s_ibadge : s_iwifi;
    if (mid >= 0) {
        home_select(mid);
    }
}

const home_layout_t home_dial = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
