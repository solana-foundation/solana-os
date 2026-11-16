// Themes: pick how Solana OS looks (core/ui_theme.h). The list on the left
// names every theme; the pane on the right previews the highlighted one,
// drawn from its own tokens, without switching to it. SELECT switches: the
// setting is saved and os_theme_apply() rebuilds the screen, this app
// included, so enter() runs again in the new theme and keeps the row.

#include <string.h>
#include "core/os.h"
#include "core/os_settings.h"
#include "core/ui.h"

#define LIST_W      168
#define PV_X        (LIST_W + 4)
#define PV_W        (OS_ROOT_W - PV_X - 4)
#define PV_H        (OS_ROOT_H - UI_HINT_H - 8)

static ui_list_t *s_list;
static lv_obj_t *s_pv;
static int s_sel = -1;      // the row to come back to after a switch
static int s_shown;

static void row(int i, ui_row_t *r, void *ctx)
{
    const ui_theme_t *t = ui_theme_at(i);
    strlcpy(r->title, t->name, sizeof(r->title));
    strlcpy(r->sub, t->blurb, sizeof(r->sub));
    if (t == ui_theme()) {
        strlcpy(r->right, "On", sizeof(r->right));
        r->right_color = COL_GREEN;
    }
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t hex)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const ui_theme_t *t, bool body, uint32_t hex, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, body && !t->mono ? &sol_font_14 : &sol_mono_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_label_set_text(l, s);
    return l;
}

// A small screen in theme t: a status strip, a panel, the selected row and
// the accent colours. Its own colours throughout, never ui_col().
static void preview(const ui_theme_t *t)
{
    lv_obj_clean(s_pv);
    lv_obj_set_style_bg_color(s_pv, lv_color_hex(t->pal[UI_P_BG]), 0);
    lv_obj_set_style_bg_image_src(s_pv, t->wallpaper, 0);
    int w = PV_W - 2;

    lv_obj_t *name = text(s_pv, t, true, t->pal[UI_P_TEXT], t->name);
    lv_obj_set_pos(name, 8, 4);
    if (t->bar == UI_BAR_HAIRLINE) {
        lv_obj_t *line = box(s_pv, 0, 22, w, 2, t->grad_a);
        lv_obj_set_style_bg_grad_color(line, lv_color_hex(t->grad_b), 0);
        lv_obj_set_style_bg_grad_dir(line, LV_GRAD_DIR_HOR, 0);
    } else if (t->bar == UI_BAR_RULE) {
        box(s_pv, 0, 24 - t->border_w, w, t->border_w, t->pal[UI_P_BORDER]);
    }

    lv_obj_t *panel = box(s_pv, 8, 32, w - 16 - t->shadow, 56, t->pal[UI_P_PANEL]);
    lv_obj_set_style_bg_opa(panel, t->panel_opa, 0);
    lv_obj_set_style_radius(panel, t->radius, 0);
    lv_obj_set_style_border_width(panel, t->border_w, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(t->pal[UI_P_BORDER]), 0);
    if (t->shadow) {
        lv_obj_set_style_shadow_width(panel, 1, 0);
        lv_obj_set_style_shadow_offset_x(panel, t->shadow, 0);
        lv_obj_set_style_shadow_offset_y(panel, t->shadow, 0);
        lv_obj_set_style_shadow_color(panel, lv_color_hex(t->pal[UI_P_BORDER]), 0);
        lv_obj_set_style_shadow_opa(panel, LV_OPA_COVER, 0);
    }
    lv_obj_t *l = text(panel, t, true, t->pal[UI_P_TEXT], "My Badge");
    lv_obj_set_pos(l, 8, 6);
    l = text(panel, t, false, t->pal[UI_P_DIM], "1,250 !LAMPs");
    lv_obj_set_pos(l, 8, 28);

    // The selected row, as ui_list draws it in this theme.
    bool fill = t->focus == UI_FOCUS_FILL;
    lv_obj_t *sel = box(s_pv, 8, 98, w - 16, 24, fill ? t->focus_bg : t->pal[UI_P_PANEL_HI]);
    lv_obj_set_style_radius(sel, t->focus == UI_FOCUS_BAR ? 0 : t->radius, 0);
    if (t->focus == UI_FOCUS_RING) {
        lv_obj_set_style_bg_opa(sel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sel, 2, 0);
        lv_obj_set_style_border_color(sel, lv_color_hex(t->focus_bg), 0);
    } else if (t->focus == UI_FOCUS_BAR) {
        box(sel, 0, 4, 3, 16, t->pal[UI_P_GREEN]);
    }
    l = text(sel, t, true, fill ? t->focus_fg : t->pal[UI_P_TEXT], t->focus == UI_FOCUS_CURSOR ? "> Selected" : "Selected");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    l = text(s_pv, t, true, t->pal[UI_P_TEXT], "Not selected");
    lv_obj_set_pos(l, 18, 128);

    static const uint8_t dots[] = {UI_P_PURPLE, UI_P_GREEN, UI_P_PINK, UI_P_BLUE, UI_P_YELLOW, UI_P_ORANGE};
    for (int i = 0; i < (int)sizeof(dots); i++) {
        lv_obj_t *d = box(s_pv, 10 + i * 21, PV_H - 30, 14, 14, t->pal[dots[i]]);
        lv_obj_set_style_radius(d, t->radius ? LV_RADIUS_CIRCLE : 0, 0);
    }
}

static void show(void)
{
    int i = ui_list_selected(s_list);
    if (i != s_shown) {
        s_shown = i;
        preview(ui_theme_at(i));
    }
}

static void enter(lv_obj_t *root)
{
    s_list = ui_list_create_at(root, 0, 4, LIST_W, OS_ROOT_H - 4 - UI_HINT_H, row, NULL);
    ui_list_set_count(s_list, ui_theme_count());
    ui_list_select(s_list, s_sel >= 0 ? s_sel : ui_theme_index(ui_theme()));
    s_sel = -1;

    s_pv = box(root, PV_X, 4, PV_W, PV_H, 0);
    lv_obj_set_style_border_width(s_pv, 1, 0);
    lv_obj_set_style_border_color(s_pv, ui_col(COL_BORDER), 0);
    lv_obj_set_style_radius(s_pv, ui_radius(), 0);
    lv_obj_set_style_clip_corner(s_pv, true, 0);
    s_shown = -1;
    show();
    ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " pick  SELECT use  CANCEL exit");
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_list_key(s_list, k, repeat)) {
        show();
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        const ui_theme_t *t = ui_theme_at(ui_list_selected(s_list));
        if (t != ui_theme()) {
            s_sel = ui_list_selected(s_list);
            os_setting_set(OS_SET_THEME, t->id);
            os_theme_apply();       // rebuilds this app too; s_list is new afterwards
            os_toast("%s", t->name);
        }
        return true;
    }
    return k != OS_KEY_BACK;
}

const os_app_t app_themes = {
    .id = "themes",
    .name = "Themes",
    .symbol = LV_SYMBOL_TINT,
    .accent = COL_PINK,
    .on_enter = enter,
    .on_key = on_key,
};
