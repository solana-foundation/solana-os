// Settings: user preferences (core/os_settings) plus shortcuts to system
// tools. One row per item; SELECT (or LEFT/RIGHT on a choice) changes it.
// Theme opens the Themes app (apps/themes).
// Brightness steps with LEFT/RIGHT (SELECT goes round from brightest to dimmest).
// Invert colours flips with SELECT or LEFT/RIGHT, at once, so wrong colours
// (RED on cyan: a prototype's panel) are put right where they're seen.
// Software update opens the update screen (apps/update). Factory reset asks
// again in an OS confirm dialog (SELECT twice).

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "esp_app_desc.h"
#include "core/os_settings.h"
#include "core/ui.h"
#include "core/ui_stream.h"

enum { ROW_THEME, ROW_BRIGHTNESS, ROW_INVERT, ROW_UPDATE, ROW_CALIB, ROW_RESET, ROW_COUNT };

static ui_list_t *s_list;

static void row(int i, ui_row_t *r, void *ctx)
{
    switch (i) {
    case ROW_THEME: {
        strlcpy(r->title, "Theme", sizeof(r->title));
        strlcpy(r->sub, ui_theme()->blurb, sizeof(r->sub));
        strlcpy(r->right, ui_theme()->name, sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_BRIGHTNESS: {
        int level = os_setting(OS_SET_BRIGHTNESS);
        strlcpy(r->title, "Brightness", sizeof(r->title));
        strlcpy(r->sub, LV_SYMBOL_LEFT " dimmer  " LV_SYMBOL_RIGHT " brighter; dimmer saves battery",
                sizeof(r->sub));
        // A bar of filled and empty blocks: "#####" reads on both themes.
        char bar[OS_BRIGHTNESS_MAX + 1];
        for (int b = 0; b < OS_BRIGHTNESS_MAX; b++) {
            bar[b] = b < level ? '#' : '-';
        }
        bar[OS_BRIGHTNESS_MAX] = 0;
        strlcpy(r->right, bar, sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_INVERT: {
        bool on = os_setting(OS_SET_LCD_INVERT) != 0;
        strlcpy(r->title, "Invert colours", sizeof(r->title));
        strlcpy(r->sub, on ? "Turn it off only if red looks cyan" : "For prototype badges; on if red looks cyan",
                sizeof(r->sub));
        strlcpy(r->right, on ? "On" : "Off", sizeof(r->right));
        r->right_color = COL_TEXT;
        break;
    }
    case ROW_UPDATE: {
        os_cloud_info_t *in = lv_malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
        }
        strlcpy(r->title, "Software update", sizeof(r->title));
        if (in && in->update[0]) {
            snprintf(r->sub, sizeof(r->sub), "Solana OS %.16s is ready to install", in->update);
            strlcpy(r->right, "New", sizeof(r->right));
            r->right_color = COL_GREEN;
        } else {
            snprintf(r->sub, sizeof(r->sub), "Solana OS %.32s", esp_app_get_description()->version);
            strlcpy(r->right, LV_SYMBOL_RIGHT, sizeof(r->right));
        }
        lv_free(in);
        break;
    }
    case ROW_CALIB:
        strlcpy(r->title, "Calibrate buttons", sizeof(r->title));
        strlcpy(r->sub, "Map the front buttons again", sizeof(r->sub));
        strlcpy(r->right, LV_SYMBOL_RIGHT, sizeof(r->right));
        break;
    case ROW_RESET:
        strlcpy(r->title, "Factory reset", sizeof(r->title));
        strlcpy(r->sub, "Release the owner, erase Wi-Fi and data", sizeof(r->sub));
        strlcpy(r->right, LV_SYMBOL_TRASH, sizeof(r->right));
        r->title_color = COL_ERR;
        break;
    }
}

// ---- The Streaming theme's screen: the same rows, drawn as its design does.
// The selected row is a raised card with its explanation; Brightness is a
// slider and Invert colours a switch.

static lv_obj_t *s_sbox;
static int s_ssel;

static lv_obj_t *plain(lv_obj_t *parent, int w, int h, uint32_t hex, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void stream_control(lv_obj_t *r, int i, const ui_row_t *row, bool sel)
{
    if (i == ROW_BRIGHTNESS) {
        int w = sel ? 150 : 90, level = os_setting(OS_SET_BRIGHTNESS);
        int fill = (w - 12) * (level - OS_BRIGHTNESS_MIN) / (OS_BRIGHTNESS_MAX - OS_BRIGHTNESS_MIN);
        lv_obj_t *track = plain(r, w, 4, 0x5A5A62, 2);
        lv_obj_align(track, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_t *on = plain(track, fill + 6, 4, ui_theme()->accent, 2);
        lv_obj_set_pos(on, 0, 0);
        lv_obj_t *knob = plain(r, 12, 12, 0xFFFFFF, LV_RADIUS_CIRCLE);
        lv_obj_align(knob, LV_ALIGN_RIGHT_MID, -10 - (w - 12 - fill), 0);
        return;
    }
    bool chevron = row->right[0] && !strcmp(row->right, LV_SYMBOL_RIGHT);
    bool trash = !strcmp(row->right, LV_SYMBOL_TRASH);
    int x = -10;
    if (i == ROW_INVERT) {
        bool on = os_setting(OS_SET_LCD_INVERT) != 0;
        lv_obj_t *sw = plain(r, 22, 12, on ? ui_theme()->accent : 0x5A5A62, 6);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, x, 0);
        lv_obj_t *knob = plain(sw, 8, 8, 0xFFFFFF, LV_RADIUS_CIRCLE);
        lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        x -= 29;
    } else if (chevron || trash || i == ROW_THEME) {
        lv_obj_t *c = ui_label(r, &lv_font_montserrat_12, COL_DIM, LV_SYMBOL_RIGHT);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, x, 0);
        x -= 14;
    }
    // What the row is set to; a row with nothing to say shows its explanation
    // there while it isn't the selected one (which has it under the title).
    const char *value = chevron || trash ? (sel ? "" : row->sub) : row->right;
    if (value[0]) {
        lv_obj_t *v = ui_label(r, &pearl_font_11, row->right_color == COL_GREEN ? COL_GREEN : COL_DIM, value);
        lv_obj_set_style_max_width(v, 170, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(v, lv_font_get_line_height(&pearl_font_11));
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, x, 0);
    }
}

static void stream_build(void)
{
    lv_obj_clean(s_sbox);
    int y = 4;
    for (int i = 0; i < ROW_COUNT; i++) {
        ui_row_t rw = {0};
        row(i, &rw, NULL);
        bool sel = i == s_ssel;
        int h = sel ? 38 : 28;
        lv_obj_t *r = plain(s_sbox, OS_ROOT_W - 20, h, 0x26262B, 5);
        lv_obj_set_pos(r, 10, y);
        lv_obj_set_style_bg_opa(r, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) {
            lv_obj_set_style_border_width(r, 2, 0);
            lv_obj_set_style_border_color(r, lv_color_white(), 0);
        } else {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, lv_color_hex(0x222226), 0);
            lv_obj_set_style_radius(r, 0, 0);
        }
        lv_obj_t *t = ui_label(r, sel ? &pearl_font_13 : &stream_bold_11, rw.title_color ? rw.title_color : COL_TEXT, rw.title);
        if (sel) {
            lv_obj_set_pos(t, 8, 4);
            // Brightness says which keys move it only where there is room.
            lv_obj_t *s = ui_label(r, &pearl_font_11, COL_DIM, i == ROW_BRIGHTNESS ? "Dimmer saves battery" : rw.sub);
            lv_obj_set_style_max_width(s, i == ROW_BRIGHTNESS ? 120 : 250, 0);
            lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(s, lv_font_get_line_height(&pearl_font_11));
            lv_obj_set_pos(s, 8, 20);
        } else {
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
        }
        stream_control(r, i, &rw, sel);
        y += h + 2;
    }
}

static void refresh(void)
{
    if (s_list) {
        ui_list_refresh(s_list);
    } else {
        stream_build();
    }
}

static void reset_answer(bool yes, void *ctx)
{
    if (yes) {
        os_toast("Erasing this badge...");
        os_cloud_factory_reset();
    }
}

// Offline the server can't release the owner: say where to do it.
static void ask_reset(void)
{
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    char text[200];
    snprintf(text, sizeof(text), "Erase this badge? Wi-Fi, apps and data on it are wiped and it restarts. "
             "Your !LAMPs and pages stay in your account.%s",
             in->claimed && !os_api_online() ? " Wi-Fi is off: also release it on the web (Profile)." : "");
    lv_free(in);
    os_confirm(text, reset_answer, NULL);
}

// LEFT/RIGHT step (and repeat), stopping at either end; SELECT goes round.
static void set_brightness(os_key_t k)
{
    int level = os_setting(OS_SET_BRIGHTNESS);
    int next = k == OS_KEY_LEFT ? level - 1 : level + 1;
    if (k == OS_KEY_SELECT && next > OS_BRIGHTNESS_MAX) {
        next = OS_BRIGHTNESS_MIN;
    }
    if (next < OS_BRIGHTNESS_MIN || next > OS_BRIGHTNESS_MAX) {
        return;
    }
    os_setting_set(OS_SET_BRIGHTNESS, next);
    os_brightness_apply();
    refresh();
}

static void enter(lv_obj_t *root)
{
    if (ui_stream()) {
        s_list = NULL;
        s_sbox = lv_obj_create(root);
        lv_obj_remove_style_all(s_sbox);
        lv_obj_set_size(s_sbox, OS_ROOT_W, OS_ROOT_H);
        lv_obj_set_scrollable(s_sbox, false);
        stream_build();
        return;
    }
    s_list = ui_list_create(root, 4, OS_ROOT_H - 4 - UI_HINT_H, row, NULL);
    ui_list_set_count(s_list, ROW_COUNT);
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT change  CANCEL exit");
}

static bool on_key(os_key_t k, bool repeat)
{
    if (!s_list && (k == OS_KEY_UP || k == OS_KEY_DOWN)) {
        int n = s_ssel + (k == OS_KEY_DOWN ? 1 : -1);
        s_ssel = n < 0 ? (repeat ? 0 : ROW_COUNT - 1) : n >= ROW_COUNT ? (repeat ? ROW_COUNT - 1 : 0) : n;
        stream_build();
        return true;
    }
    if (s_list && ui_list_key(s_list, k, repeat)) {
        return true;
    }
    int i = s_list ? ui_list_selected(s_list) : s_ssel;
    if (i == ROW_BRIGHTNESS && (k == OS_KEY_LEFT || k == OS_KEY_RIGHT || (k == OS_KEY_SELECT && !repeat))) {
        set_brightness(k);
        return true;
    }
    bool change = k == OS_KEY_SELECT || ((k == OS_KEY_LEFT || k == OS_KEY_RIGHT) && i == ROW_INVERT);
    if (!change || repeat) {
        return k != OS_KEY_BACK;
    }
    switch (i) {
    case ROW_THEME:
        os_launch(&app_themes);
        break;
    case ROW_INVERT:
        os_setting_set(OS_SET_LCD_INVERT, !os_setting(OS_SET_LCD_INVERT));
        os_invert_apply();
        refresh();
        break;
    case ROW_UPDATE:
        os_launch(&app_update);
        break;
    case ROW_CALIB:
        os_launch(&os_calibrate);
        break;
    case ROW_RESET:
        ask_reset();
        break;
    }
    return true;
}

const os_app_t app_settings = {
    .id = "settings",
    .name = "Settings",
    .symbol = LV_SYMBOL_LIST,
    .accent = COL_BLUE,
    .on_enter = enter,
    .on_key = on_key,
};
