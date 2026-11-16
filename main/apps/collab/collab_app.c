// Collaborator (game/collab.h; docs/HHGTS.md "Collaborator"): the owner's
// side, and the hidden takeover screen the server drives.
//
// The app: the state, today's live time and !LAMPs from the server, what it
// pays, and two rows, Collaborator On/Off and Lend the screen Yes/No (a third,
// End the takeover, while one runs). Turning it on asks first, in the OS's
// own dialog (os_confirm), which no app or server can answer; off is one
// press.
//
// The takeover screen (collab_screen): a green header that says HOME ends it,
// and the server's content below (a card, a QR code, an image, a colour, a
// countdown). It is an app like any other, so HOME and CANCEL leave it; its
// on_exit tells the service, which ends the takeover unless it closed the
// screen itself.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/ui.h"
#include "game/collab.h"
#include "game/game.h"

#define ROW_H       30
#define ROWS_Y      118
#define HEAD_H      20

// ---------------------------------------------------------------------------
// The app

static const char CONSENT[] =
    "Lend this badge to the event? While it's on, the server can use its radio to send and receive "
    "mesh messages, and, if you lend the screen, show things on it and light its LEDs. It never runs "
    "code, spends or sees your data. You earn !LAMPs while it's on and more while it's in use. HOME "
    "ends a takeover (you lose that takeover's !LAMPs). Turn it off here or in Services.";

static struct {
    lv_obj_t *state, *detail, *today, *rates, *hint;
    lv_obj_t *row[3], *row_l[3], *row_r[3];
    int sel;
    uint32_t rev;
} s_a;

static int rows(void)
{
    return collab_info()->phase == COLLAB_TAKEN ? 3 : 2;
}

static void render(void)
{
    const collab_info_t *in = collab_info();
    s_a.rev = in->rev;
    static const char *const names[] = { "Off", "Waiting", "Live", "Taken over" };
    uint32_t col = in->phase == COLLAB_OFF ? COL_DIM : in->phase == COLLAB_WAITING ? COL_WARN : COL_GREEN;
    char line[96];
    if (in->phase == COLLAB_TAKEN) {
        snprintf(line, sizeof(line), "Taken over (%s)", in->screen ? "screen" : "radio");
    } else {
        strlcpy(line, names[in->phase], sizeof(line));
    }
    lv_label_set_text(s_a.state, line);
    lv_obj_set_style_text_color(s_a.state, ui_col(col), 0);

    switch (in->phase) {
    case COLLAB_OFF:
        strlcpy(line, "Lend this badge to the event and earn !LAMPs.", sizeof(line));
        break;
    case COLLAB_WAITING:
        strlcpy(line, in->why[0] ? in->why : "Waiting for Wi-Fi", sizeof(line));
        break;
    case COLLAB_LIVE:
        snprintf(line, sizeof(line), "%s", in->earns ? "The event can take it over at any time." : in->why);
        break;
    case COLLAB_TAKEN: {
        char amt[32];
        snprintf(line, sizeof(line), "%d min, %s pending%s", in->minutes, ui_fmt_lamps(amt, sizeof(amt), in->pending),
                 in->sent || in->heard ? "" : "");
        break;
    }
    }
    lv_label_set_text(s_a.detail, line);

    if (in->synced_ms) {
        snprintf(line, sizeof(line), "Today: %d min live, %d of %d !LAMPs", in->live_min, in->earned, in->cap);
    } else {
        strlcpy(line, "Today: not synced yet", sizeof(line));
    }
    lv_label_set_text(s_a.today, line);
    snprintf(line, sizeof(line), "Pays %d per %d min live, %d a minute in use", in->rate_live, in->rate_minutes,
             in->rate_take);
    lv_label_set_text(s_a.rates, line);

    int n = rows();
    const char *left[3] = { "Collaborator", "Lend the screen", "End the takeover" };
    const char *right[3] = { in->phase == COLLAB_OFF ? "Off" : "On", in->lend ? "Yes" : "No", "No !LAMPs" };
    if (s_a.sel >= n) {
        s_a.sel = n - 1;
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_hidden(s_a.row[i], i >= n);
        lv_label_set_text(s_a.row_l[i], left[i]);
        lv_label_set_text(s_a.row_r[i], right[i]);
        uint32_t rc = i == 0 ? (in->phase == COLLAB_OFF ? COL_DIM : COL_GREEN) : i == 1 ? (in->lend ? COL_GREEN : COL_DIM) : COL_ERR;
        lv_obj_set_style_text_color(s_a.row_r[i], ui_col(rc), 0);
        lv_obj_set_style_bg_color(s_a.row[i], ui_col(i == s_a.sel ? COL_PANEL_HI : COL_PANEL), 0);
        lv_obj_set_style_border_color(s_a.row[i], ui_col(i == s_a.sel ? COL_GREEN : COL_BORDER), 0);
    }
}

static void consent_answer(bool yes, void *ctx)
{
    if (yes) {
        collab_turn_on();
    }
    render();
}

static void enter(lv_obj_t *root)
{
    memset(&s_a, 0, sizeof(s_a));
    s_a.state = ui_label(root, &sol_font_20, COL_DIM, "");
    lv_obj_set_pos(s_a.state, 10, 6);
    s_a.detail = ui_label(root, &sol_font_14, COL_TEXT, "");
    lv_obj_set_pos(s_a.detail, 10, 36);
    lv_obj_set_width(s_a.detail, OS_ROOT_W - 20);
    s_a.today = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_a.today, 10, 76);
    s_a.rates = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_a.rates, 10, 94);
    lv_obj_set_width(s_a.rates, OS_ROOT_W - 20);
    lv_label_set_long_mode(s_a.rates, LV_LABEL_LONG_DOT);
    for (int i = 0; i < 3; i++) {
        s_a.row[i] = ui_panel(root, 8, ROWS_Y + i * (ROW_H + 4), OS_ROOT_W - 16, ROW_H);
        lv_obj_set_style_pad_hor(s_a.row[i], 8, 0);
        s_a.row_l[i] = ui_label(s_a.row[i], &sol_font_14, COL_TEXT, "");
        lv_obj_align(s_a.row_l[i], LV_ALIGN_LEFT_MID, 0, 0);
        s_a.row_r[i] = ui_label(s_a.row[i], &sol_font_14, COL_DIM, "");
        lv_obj_align(s_a.row_r[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    s_a.hint = ui_hint(root, "SELECT change   CANCEL exit");
    render();
}

static bool key(os_key_t k, bool repeat)
{
    const collab_info_t *in = collab_info();
    int n = rows();
    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        s_a.sel = (s_a.sel + (k == OS_KEY_DOWN ? 1 : n - 1)) % n;
        render();
        return true;
    }
    if (k != OS_KEY_SELECT || repeat) {
        return false;
    }
    if (s_a.sel == 0) {
        if (in->phase == COLLAB_OFF) {
            os_confirm(CONSENT, consent_answer, NULL);
        } else {
            collab_turn_off();
        }
    } else if (s_a.sel == 1) {
        collab_set_lend(!in->lend);
    } else if (s_a.sel == 2) {
        collab_end_takeover();
    }
    render();
    return true;
}

static void tick(void)
{
    if (collab_info()->rev != s_a.rev) {
        render();
    }
}

static void leave(void)
{
    memset(&s_a, 0, sizeof(s_a));
}

const os_app_t app_collab = {
    .id = "collab",
    .name = "Collaborator",
    .symbol = LV_SYMBOL_EYE_OPEN,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};

// ---------------------------------------------------------------------------
// The takeover screen

static struct {
    lv_obj_t *root, *body, *count;
    uint32_t show_rev, info_rev;
    int64_t count_to;
    int last_s;
} s_s;

static lv_color_t true_col(uint32_t rgb)
{
    return lv_color_hex(rgb);       // the server's colours are true colours, not the theme's
}

// Black or white, whichever reads on bg.
static lv_color_t contrast(uint32_t bg)
{
    int lum = (299 * (int)((bg >> 16) & 0xFF) + 587 * (int)((bg >> 8) & 0xFF) + 114 * (int)(bg & 0xFF)) / 1000;
    return lum > 140 ? lv_color_black() : lv_color_white();
}

static void build_body(void)
{
    const collab_show_t *sh = collab_show();
    const collab_info_t *in = collab_info();
    s_s.show_rev = in->show_rev;
    s_s.info_rev = in->rev;
    s_s.count = NULL;
    s_s.last_s = -1;
    if (s_s.body) {
        lv_obj_delete(s_s.body);
    }
    lv_obj_t *b = s_s.body = lv_obj_create(s_s.root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 0, HEAD_H);
    lv_obj_set_size(b, OS_ROOT_W, OS_ROOT_H - HEAD_H);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, sh->has_bg ? true_col(sh->bg) : ui_col(COL_BG), 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 6, 0);
    lv_color_t fg = sh->has_col ? true_col(sh->col) : sh->has_bg ? contrast(sh->bg) : ui_col(COL_TEXT);

    lv_obj_t *t = NULL;
    switch (sh->k) {
    case COLLAB_SHOW_IDLE: {
        lv_obj_t *eye = ui_label(b, &lv_font_montserrat_20, COL_GREEN, LV_SYMBOL_EYE_OPEN);
        (void)eye;
        ui_label(b, &sol_font_20, COL_TEXT, "The event is using this badge");
        char line[64], amt[32];
        snprintf(line, sizeof(line), "%d min so far, %s pending", in->minutes, ui_fmt_lamps(amt, sizeof(amt), in->pending));
        ui_label(b, &sol_mono_12, COL_DIM, line);
        break;
    }
    case COLLAB_SHOW_CARD:
        if (sh->title[0]) {
            t = ui_label(b, &sol_font_32, COL_TEXT, sh->title);
            lv_obj_set_width(t, OS_ROOT_W - 20);    // a long title wraps (it ran off the screen)
            lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(t, fg, 0);
        }
        if (sh->body[0]) {
            lv_obj_t *l = ui_label(b, &sol_font_20, COL_TEXT, sh->body);
            lv_obj_set_width(l, OS_ROOT_W - 20);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(l, fg, 0);
        }
        break;
    case COLLAB_SHOW_QR: {
        lv_obj_t *plate = lv_obj_create(b);
        lv_obj_remove_style_all(plate);
        lv_obj_set_size(plate, 156, 156);
        lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(plate, lv_color_white(), 0);
        lv_obj_set_style_radius(plate, 6, 0);
        lv_obj_t *qr = lv_qrcode_create(plate);
        lv_qrcode_set_size(qr, 148);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_update(qr, sh->text, strlen(sh->text));
        lv_obj_center(qr);
        if (sh->title[0]) {
            t = ui_label(b, &sol_font_14, COL_TEXT, sh->title);
            lv_obj_set_style_text_color(t, fg, 0);
        }
        break;
    }
    case COLLAB_SHOW_IMAGE:
        if (sh->img) {
            lv_obj_t *im = lv_image_create(b);
            lv_image_set_src(im, sh->img);
        } else if (!sh->img_failed) {
            ui_label(b, &sol_mono_12, COL_DIM, "Loading the image...");
        }
        if (sh->title[0]) {
            t = ui_label(b, &sol_font_14, COL_TEXT, sh->title);
            lv_obj_set_style_text_color(t, fg, 0);
        }
        break;
    case COLLAB_SHOW_COLOR:
        if (sh->title[0]) {
            t = ui_label(b, &sol_font_32, COL_TEXT, sh->title);
            lv_obj_set_width(t, OS_ROOT_W - 20);    // a long title wraps (it ran off the screen)
            lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(t, fg, 0);
        }
        break;
    case COLLAB_SHOW_COUNT:
        if (sh->title[0]) {
            t = ui_label(b, &sol_font_20, COL_TEXT, sh->title);
            lv_obj_set_style_text_color(t, fg, 0);
        }
        s_s.count = ui_label(b, &sol_font_32, COL_TEXT, "");
        lv_obj_set_style_text_color(s_s.count, fg, 0);
        s_s.count_to = sh->to;
        break;
    }
}

static void count_tick(void)
{
    if (!s_s.count) {
        return;
    }
    int64_t now = os_cloud_time_ms();
    int64_t left = now > 0 ? s_s.count_to - now : 0;
    int s = left > 0 ? (int)((left + 999) / 1000) : 0;
    if (s == s_s.last_s) {
        return;
    }
    s_s.last_s = s;
    if (s >= 3600) {
        lv_label_set_text_fmt(s_s.count, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        lv_label_set_text_fmt(s_s.count, "%d:%02d", s / 60, s % 60);
    }
}

static void screen_enter(lv_obj_t *root)
{
    memset(&s_s, 0, sizeof(s_s));
    s_s.root = root;
    lv_obj_t *head = lv_obj_create(root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, OS_ROOT_W, HEAD_H);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(COL_GREEN), 0);
    lv_obj_t *l = lv_label_create(head);
    lv_obj_set_style_text_font(l, &sol_mono_12, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, LV_SYMBOL_EYE_OPEN " Lent to the event: HOME ends it");
    lv_obj_center(l);
    build_body();
    count_tick();
    collab_screen_entered();
}

static bool screen_key(os_key_t k, bool repeat)
{
    return k != OS_KEY_BACK;            // CANCEL leaves (and ends it), as HOME does; the rest do nothing
}

static void screen_tick(void)
{
    const collab_info_t *in = collab_info();
    if (in->show_rev != s_s.show_rev || (collab_show()->k == COLLAB_SHOW_IDLE && in->rev != s_s.info_rev)) {
        build_body();
    }
    count_tick();
}

static void screen_exit(void)
{
    memset(&s_s, 0, sizeof(s_s));
    collab_screen_left();
}

const os_app_t app_collab_screen = {
    .id = "collab_screen",
    .name = "Collaborator",
    .symbol = LV_SYMBOL_EYE_OPEN,
    .accent = COL_GREEN,
    .on_enter = screen_enter,
    .on_exit = screen_exit,
    .on_key = screen_key,
    .on_tick = screen_tick,
};
