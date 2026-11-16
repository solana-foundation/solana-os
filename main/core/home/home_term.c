// Home layout: a console listing (the Terminal theme). The status bar gives
// way to the listing's own header line; under it a prompt, one 20 px row per
// app (a marker, its id, what it has to say, a right-hand field), a second
// prompt with a block cursor and the page, and the key hint. No icons.
// Positions are the design's, in screen pixels minus the 22 px bar.

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "home.h"

#define ROWS        7
#define ROW_H       20
#define X0          10
#define W0          (OS_ROOT_W - 2 * X0)
#define ROWS_Y      27
#define AMBER       COL_YELLOW          // the theme's warning colour

#define FONT        (&term_mono_11)
#define FONT_BOLD   (&term_mono_bold_11)

typedef struct {
    lv_obj_t *obj, *mark, *name, *detail, *right;
} row_t;

static row_t s_rows[ROWS];
static lv_obj_t *s_page, *s_wifi;
static int s_top;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) {
        lv_label_set_text(label, text);
    }
}

static void lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static lv_obj_t *text(lv_obj_t *parent, int x, int y, uint32_t col, lv_opa_t opa, const char *s)
{
    lv_obj_t *l = ui_label(parent, FONT, col, s);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *rule(lv_obj_t *parent, int y)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, X0, y);
    lv_obj_set_size(r, W0, 1);
    lv_obj_set_style_bg_color(r, ui_col(COL_TEXT), 0);
    lv_obj_set_style_bg_opa(r, 115, 0);
    return r;
}

static void row_fill(row_t *r, int index)
{
    const os_app_t *app = home_app(index);
    lv_obj_set_hidden(r->obj, !app);
    if (!app) {
        return;
    }
    bool on = index == home_sel();
    home_info_t in;
    home_info(app, &in);
    char name[24], detail[64], right[48];
    strlcpy(name, !strcmp(app->id, "badge") ? "my-badge" : app->id, sizeof(name));
    detail[0] = right[0] = 0;
    uint32_t detail_col = COL_TEXT;
    lv_opa_t detail_opa = 178;
    if (!strcmp(app->id, "badge")) {
        // The balance, then the badge ID on the right.
        if (in.value[0]) {
            snprintf(detail, sizeof(detail), "%s !LAMPs", in.value);
            detail_opa = LV_OPA_COVER;
        } else {
            strlcpy(detail, in.line, sizeof(detail));
        }
        strlcpy(right, in.extra, sizeof(right));
    } else if (in.value[0]) {
        // A state (Wi-Fi's), then what it is about on the right.
        strlcpy(detail, in.value, sizeof(detail));
        strlcpy(right, in.line, sizeof(right));
        detail_col = in.value_color == COL_OK ? COL_TEXT : AMBER;
        detail_opa = LV_OPA_COVER;
    } else {
        strlcpy(detail, in.line, sizeof(detail));
    }
    if (strcmp(app->id, "badge")) {
        lower(detail);
    }

    lv_obj_set_style_bg_opa(r->obj, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    set_text(r->mark, on ? ">" : "-");
    set_text(r->name, name);
    set_text(r->detail, detail);
    set_text(r->right, right);
    const lv_font_t *face = on ? FONT_BOLD : FONT;
    lv_obj_set_style_text_font(r->mark, face, 0);
    lv_obj_set_style_text_font(r->name, face, 0);
    lv_obj_set_style_text_font(r->detail, face, 0);
    lv_obj_set_style_text_font(r->right, face, 0);
    lv_obj_set_style_text_color(r->mark, on ? ui_focus_fg() : ui_col(COL_TEXT), 0);
    lv_obj_set_style_text_opa(r->mark, on ? LV_OPA_COVER : 115, 0);
    lv_obj_set_style_text_color(r->name, on ? ui_focus_fg() : ui_col(COL_TEXT), 0);
    lv_obj_set_style_text_color(r->detail, on ? ui_focus_fg() : ui_col(detail_col), 0);
    lv_obj_set_style_text_opa(r->detail, on ? LV_OPA_COVER : detail_opa, 0);
    lv_obj_set_style_text_color(r->right, on ? ui_focus_fg() : ui_col(COL_TEXT), 0);
    lv_obj_set_style_text_opa(r->right, on ? LV_OPA_COVER : 178, 0);
    // Keep the detail clear of the right-hand field.
    lv_obj_update_layout(r->right);
    lv_obj_set_width(r->detail, 304 - X0 - 112 - (right[0] ? lv_obj_get_width(r->right) + 8 : 0));
}

static void header(void)
{
    home_info_t in;
    int i = home_find("wifi");
    if (i < 0) {
        set_text(s_wifi, "");
        return;
    }
    home_info(home_app(i), &in);
    char state[32];
    strlcpy(state, in.value[0] ? in.value : "idle", sizeof(state));
    lower(state);
    set_text(s_wifi, state);
    lv_obj_set_style_text_color(s_wifi, ui_col(in.value_color == COL_OK ? COL_TEXT : AMBER), 0);
}

static void render(bool list)
{
    int sel = home_sel(), count = home_count();
    if (sel < s_top) {
        s_top = sel;
    } else if (sel >= s_top + ROWS) {
        s_top = sel - ROWS + 1;
    }
    if (s_top > count - ROWS) {
        s_top = count - ROWS;
    }
    if (s_top < 0) {
        s_top = 0;
    }
    for (int i = 0; i < ROWS; i++) {
        row_fill(&s_rows[i], s_top + i);
    }
    char page[24];
    snprintf(page, sizeof(page), "[%d/%d]", sel / ROWS + 1, (count + ROWS - 1) / ROWS);
    set_text(s_page, page);
    header();
}

static void info(void)
{
    for (int i = 0; i < ROWS; i++) {
        row_fill(&s_rows[i], s_top + i);
    }
    header();
}

static void enter(lv_obj_t *root)
{
    // The listing's own header where the status bar was: the OS and its
    // version, and the network's state.
    lv_obj_t *box = lv_obj_create(root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_scrollable(box, false);
    lv_obj_t *head = home_header(box);
    char os[40];
    snprintf(os, sizeof(os), "solana-os %.16s", esp_app_get_description()->version);
    lv_obj_t *name = text(head, X0, 7, COL_TEXT, LV_OPA_COVER, os);
    lv_obj_set_style_text_font(name, FONT_BOLD, 0);
    s_wifi = ui_label(head, FONT, AMBER, "");
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -X0, 7);
    lv_obj_t *wifi = ui_label(head, FONT, COL_TEXT, "wifi:");
    lv_obj_align(wifi, LV_ALIGN_TOP_RIGHT, -X0 - 70, 7);
    lv_obj_set_width(s_wifi, 68);

    rule(box, 3);
    text(box, X0, 9, COL_TEXT, 178, "sol>");
    text(box, 42, 9, COL_TEXT, LV_OPA_COVER, "apps --home");
    for (int i = 0; i < ROWS; i++) {
        row_t *r = &s_rows[i];
        r->obj = lv_obj_create(box);
        lv_obj_remove_style_all(r->obj);
        lv_obj_set_pos(r->obj, X0, ROWS_Y + i * ROW_H);
        lv_obj_set_size(r->obj, W0, ROW_H);
        lv_obj_set_style_bg_color(r->obj, ui_focus_bg(), 0);
        lv_obj_set_scrollable(r->obj, false);
        r->mark = text(r->obj, 6, 3, COL_TEXT, 115, "-");
        r->name = text(r->obj, 21, 3, COL_TEXT, LV_OPA_COVER, "");
        r->detail = text(r->obj, 103, 3, COL_TEXT, 178, "");
        lv_label_set_long_mode(r->detail, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(r->detail, lv_font_get_line_height(FONT));
        r->right = ui_label(r->obj, FONT, COL_TEXT, "");
        lv_obj_align(r->right, LV_ALIGN_TOP_RIGHT, -6, 3);
    }
    rule(box, 174);
    text(box, X0, 181, COL_TEXT, 178, "sol>");
    lv_obj_t *cursor = lv_obj_create(box);
    lv_obj_remove_style_all(cursor);
    lv_obj_set_pos(cursor, 42, 182);
    lv_obj_set_size(cursor, 7, 13);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cursor, ui_col(COL_TEXT), 0);
    s_page = ui_label(box, FONT, COL_TEXT, "");
    lv_obj_set_style_text_opa(s_page, 178, 0);
    lv_obj_align(s_page, LV_ALIGN_TOP_RIGHT, -X0, 181);
    lv_obj_t *keys = text(box, X0, 199, COL_TEXT, 140, "[D-PAD] move  [SELECT] open  [BOOT] home");
    lv_obj_set_style_text_font(keys, &term_mono_9, 0);
    s_top = 0;
    render(true);
}

static void key(os_key_t k, bool repeat)
{
    if (k == OS_KEY_UP || k == OS_KEY_DOWN) {
        home_select(home_step(home_sel(), k == OS_KEY_DOWN ? 1 : -1, repeat));
    } else {
        home_select(home_sel() + (k == OS_KEY_RIGHT ? ROWS : -ROWS));
    }
}

const home_layout_t home_term = {
    .enter = enter,
    .render = render,
    .key = key,
    .info = info,
};
