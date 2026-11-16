// The theme table. See ui_theme.h.

#include <stddef.h>
#include "ui_theme.h"
#include "theme.h"
#include "os_settings.h"
#include "assets/assets.h"

// Brand accents as they are (on dark grounds), the deeper shades the Light
// theme has always used on white, deeper ones still for coloured grounds, and
// plain ink for themes with one colour.
#define ACCENTS_DARK    COL_PURPLE, COL_GREEN, COL_PINK, COL_LPURPLE, COL_BLUE, COL_AQUA, COL_LIME, COL_YELLOW, COL_ORANGE
#define ACCENTS_LIGHT   COL_PURPLE, 0x0A9E62, 0xC8319A, 0x6B5CF0, 0x2A76D2, 0x0089A8, 0x3A9A28, 0x8F8A00, 0xE04E1E
#define ACCENTS_DEEP    0x6B2BD9, 0x0A6B43, 0xA3237C, 0x4B3CC8, 0x1D5BA8, 0x006B84, 0x2C7A1E, 0x6B6700, 0xB23A12
#define ACCENTS_INK(c)  c, c, c, c, c, c, c, c, c

static const ui_theme_t s_themes[] = {
    {
        .id = UI_THEME_DARK, .name = "Classic Dark", .blurb = "White on black, the original",
        .pal = {COL_BG, COL_PANEL, COL_PANEL_HI, COL_BORDER, COL_TEXT, COL_DIM, ACCENTS_DARK},
        .focus_bg = COL_GREEN, .focus_fg = 0x000000, .accent = COL_PURPLE, .scrim = 0x000000,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 6, .tile_radius = 10, .border_w = 1, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .focus = UI_FOCUS_BAR, .bar = UI_BAR_HAIRLINE, .home = UI_HOME_GRID,
    },
    {
        .id = UI_THEME_LIGHT, .name = "Classic Light", .blurb = "Black on white",
        .pal = {0xFFFFFF, 0xF4F3F8, 0xE8E6F0, 0xD5D2E0, 0x000000, 0x6B6880, ACCENTS_LIGHT},
        .focus_bg = 0x0A9E62, .focus_fg = 0xFFFFFF, .accent = COL_PURPLE, .scrim = 0x000000,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 6, .tile_radius = 10, .border_w = 1, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .light = true, .focus = UI_FOCUS_BAR, .bar = UI_BAR_HAIRLINE, .home = UI_HOME_GRID,
    },
    {
        .id = UI_THEME_PEARL, .name = "Pearl", .blurb = "Soft glass tiles on a pale wallpaper",
        .pal = {0xF4F0EC, 0xFFFFFF, 0xE6E0EC, 0xFFFFFF, 0x1F1B2E, 0x5A5370, ACCENTS_LIGHT},
        .focus_bg = 0x5B4A86, .focus_fg = 0xFFFFFF, .accent = 0x5B4A86, .scrim = 0x1F1B2E,
        .grad_a = 0xE97FB0, .grad_b = 0x5FCBA8,
        .radius = 12, .tile_radius = 16, .border_w = 1, .panel_opa = 190, .sheet_opa = 150,
        .light = true, .focus = UI_FOCUS_RING, .bar = UI_BAR_PLAIN, .home = UI_HOME_PEARL,
        .wallpaper = &img_wall_pearl,
    },
    {
        .id = UI_THEME_BLACK_PEARL, .name = "Black pearl", .blurb = "Dark glass tiles, teal and violet glow",
        .pal = {0x08070C, 0x24222E, 0x34323F, 0x45435A, 0xFFFFFF, 0xC2C0D2, ACCENTS_DARK},
        .focus_bg = 0x8FEBD8, .focus_fg = 0x08070C, .accent = 0x8FEBD8, .scrim = 0x000000,
        .grad_a = 0x8FEBD8, .grad_b = 0xB890FF,
        .radius = 12, .tile_radius = 16, .border_w = 1, .panel_opa = 150, .sheet_opa = 130,
        .focus = UI_FOCUS_RING, .bar = UI_BAR_PLAIN, .home = UI_HOME_PEARL,
        .wallpaper = &img_wall_black_pearl,
    },
    {
        .id = UI_THEME_ICONS, .name = "Icon grid", .blurb = "Colour icons and a dock",
        .pal = {0x060913, 0x222840, 0x323A58, 0x4A506A, 0xFFFFFF, 0xC8CCE0, ACCENTS_DARK},
        .focus_bg = 0xFFFFFF, .focus_fg = 0x0A0F1C, .accent = 0xFFFFFF, .scrim = 0x000000,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 11, .tile_radius = 16, .border_w = 1, .panel_opa = 150, .sheet_opa = 140,
        .focus = UI_FOCUS_RING, .bar = UI_BAR_SLIM, .home = UI_HOME_ICONS,
        .wallpaper = &img_wall_aurora,
    },
    {
        .id = UI_THEME_GROUPED, .name = "Grouped list", .blurb = "A profile card over one row per app",
        .pal = {0xF2F2F7, 0xFFFFFF, 0xE6E6EE, 0xE4E4EB, 0x0B0C12, 0x4A4F63, ACCENTS_LIGHT},
        .focus_bg = 0x0B0C12, .focus_fg = 0xFFFFFF, .accent = 0x0B0C12, .scrim = 0x0B0C12,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 14, .tile_radius = 14, .border_w = 0, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .light = true, .focus = UI_FOCUS_FILL, .bar = UI_BAR_SLIM, .home = UI_HOME_LIST,
    },
    {
        .id = UI_THEME_DIAL, .name = "Dial", .blurb = "Black face, round buttons at the sides",
        .pal = {0x000000, 0x1C1C1F, 0x2C2C30, 0x2C2C30, 0xFFFFFF, 0xB4B4BE,
                COL_PURPLE, 0x34D17A, COL_PINK, COL_LPURPLE, COL_BLUE, COL_AQUA, COL_LIME, 0xFFB870, 0xFF9F5A},
        .focus_bg = 0xFF9F5A, .focus_fg = 0x1A0A00, .accent = 0xFF9F5A, .scrim = 0x000000,
        .grad_a = 0xFFB45C, .grad_b = 0xFF6A2B,
        .radius = 16, .tile_radius = 20, .border_w = 0, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .focus = UI_FOCUS_FILL, .bar = UI_BAR_SLIM, .home = UI_HOME_DIAL,
    },
    {
        .id = UI_THEME_CONTROLS, .name = "Control tiles", .blurb = "Dark glass tiles on a colour wallpaper",
        .pal = {0x060913, 0x16161E, 0x2A2A36, 0x3A3A48, 0xFFFFFF, 0xD0D0DC, ACCENTS_DARK},
        .focus_bg = 0xFFFFFF, .focus_fg = 0x0A0F1C, .accent = 0xFFFFFF, .scrim = 0x000000,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 12, .tile_radius = 16, .border_w = 0, .panel_opa = 158, .sheet_opa = 140,
        .focus = UI_FOCUS_FILL, .bar = UI_BAR_PLAIN, .home = UI_HOME_CONTROLS,
        .wallpaper = &img_wall_aurora,
    },
    {
        .id = UI_THEME_TERMINAL, .name = "Terminal", .blurb = "Green mono on black, square corners",
        .pal = {0x020503, 0x020503, 0x0A2A1C, 0x1D7A4D, 0x3DFFA2, 0x2BB273,
                COL_PURPLE, 0x3DFFA2, COL_PINK, COL_LPURPLE, COL_BLUE, COL_AQUA, COL_LIME, 0xFFB866, 0xFFB866},
        .focus_bg = 0x3DFFA2, .focus_fg = 0x020503, .accent = 0x3DFFA2, .scrim = 0x000000,
        .grad_a = COL_PURPLE, .grad_b = COL_GREEN,
        .radius = 0, .tile_radius = 0, .border_w = 1, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .mono = true, .focus = UI_FOCUS_FILL, .bar = UI_BAR_RULE, .home = UI_HOME_TERM,
    },
    {
        .id = UI_THEME_EPAPER, .name = "E-paper", .blurb = "Black ink on paper, serif type, hairline rules",
        .pal = {0xECE8DD, 0xECE8DD, 0xDCD7C8, 0x141412, 0x141412, 0x4A4840, ACCENTS_INK(0x141412)},
        .focus_bg = 0x141412, .focus_fg = 0xECE8DD, .accent = 0x141412, .scrim = 0x141412,
        .grad_a = 0x141412, .grad_b = 0x141412,
        .radius = 0, .tile_radius = 0, .border_w = 1, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .light = true, .focus = UI_FOCUS_FILL, .bar = UI_BAR_RULE, .home = UI_HOME_EPAPER,
    },
    {
        .id = UI_THEME_BLOCKS, .name = "Bold blocks", .blurb = "Flat colour blocks, thick outlines, hard shadows",
        .pal = {0xF3ECDD, 0xFFFFFF, 0xFFE9A8, 0x111111, 0x111111, 0x3D3A2E, ACCENTS_DEEP},
        .focus_bg = 0x6B2BD9, .focus_fg = 0xFFFFFF, .accent = 0x111111, .scrim = 0x111111,
        .grad_a = 0x6B2BD9, .grad_b = 0xFF5A36,
        .radius = 10, .tile_radius = 10, .border_w = 2, .shadow = 4, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .light = true, .focus = UI_FOCUS_FILL, .bar = UI_BAR_PLAIN, .home = UI_HOME_BLOCKS,
    },
    {
        .id = UI_THEME_RPG, .name = "Monster RPG", .blurb = "A retro handheld's menus, sky and grass",
        .pal = {0xFFFDF2, 0xFFFDF2, 0xF1EBD2, 0x23233A, 0x23233A, 0x5A5A70, ACCENTS_LIGHT},
        .focus_bg = 0xD8432F, .focus_fg = 0xFFFFFF, .accent = 0xD8432F, .scrim = 0x23233A,
        .grad_a = 0xD8432F, .grad_b = 0xD8B848,
        .radius = 6, .tile_radius = 6, .border_w = 2, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_COVER,
        .light = true, .focus = UI_FOCUS_CURSOR, .bar = UI_BAR_PLAIN, .home = UI_HOME_RPG,
    },
    {
        // Lumen: glass cards lit in blue and peach on a night ground.
        .id = UI_THEME_GLOW, .name = "Lumen", .blurb = "Night glass, blue and peach glow",
        .pal = {0x06070B, 0x171A26, 0x23283A, 0x2C3144, 0xFFFFFF, 0xC4C8D8,
                0xB79BFF, 0x7DE0B0, 0xFF8FC0, 0xB4C6FF, 0x6F9BFF, 0x8CDCFF, 0xB4FFA4, 0xFFD98A, 0xFFB877},
        .focus_bg = 0xB4C6FF, .focus_fg = 0x06070B, .accent = 0xFFC79A, .scrim = 0x000000,
        .grad_a = 0xFFC79A, .grad_b = 0x6F9BFF,
        .radius = 12, .tile_radius = 16, .border_w = 1, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_TRANSP,
        .focus = UI_FOCUS_RING, .bar = UI_BAR_SLIM, .home = UI_HOME_LUMEN,
    },
    {
        // The streaming-app look: near-black ground, white focus rings, one red.
        .id = UI_THEME_STREAM, .name = "Streaming", .blurb = "A feature banner and rows of posters",
        .pal = {0x0B0B0D, 0x17171A, 0x26262B, 0x2A2A2F, 0xFFFFFF, 0xB9B9C0,
                0xC9A2FF, 0x57D98A, 0xF26BC6, 0xBDB7FF, 0x74B4F5, 0x8CEEFF, 0xB4FFA4, 0xFFD23F, 0xFFB48C},
        .focus_bg = 0xFFFFFF, .focus_fg = 0x0B0B0D, .accent = 0xE5383B, .scrim = 0x000000,
        .grad_a = 0xE5383B, .grad_b = 0xE5383B,
        .radius = 5, .tile_radius = 4, .border_w = 0, .panel_opa = LV_OPA_COVER, .sheet_opa = LV_OPA_TRANSP,
        .focus = UI_FOCUS_RING, .bar = UI_BAR_STREAM, .home = UI_HOME_STREAM,
    },
};

#define THEME_COUNT ((int)(sizeof(s_themes) / sizeof(s_themes[0])))

static const ui_theme_t *s_active;

int ui_theme_count(void)
{
    return THEME_COUNT;
}

const ui_theme_t *ui_theme_at(int index)
{
    return index >= 0 && index < THEME_COUNT ? &s_themes[index] : &s_themes[0];
}

int ui_theme_index(const ui_theme_t *t)
{
    return t >= s_themes && t < s_themes + THEME_COUNT ? (int)(t - s_themes) : 0;
}

void ui_theme_reload(void)
{
    int id = os_setting(OS_SET_THEME);
    s_active = &s_themes[0];
    for (int i = 0; i < THEME_COUNT; i++) {
        if (s_themes[i].id == id) {
            s_active = &s_themes[i];
        }
    }
}

const ui_theme_t *ui_theme(void)
{
    if (!s_active) {
        ui_theme_reload();
    }
    return s_active;
}

uint32_t ui_theme_hex(const ui_theme_t *t, uint32_t col)
{
    static const uint32_t keys[UI_P_COUNT] = {
        COL_BG, COL_PANEL, COL_PANEL_HI, COL_BORDER, COL_TEXT, COL_DIM,
        COL_PURPLE, COL_GREEN, COL_PINK, COL_LPURPLE, COL_BLUE, COL_AQUA, COL_LIME, COL_YELLOW, COL_ORANGE,
    };
    for (int i = 0; i < UI_P_COUNT; i++) {
        if (keys[i] == col) {
            return t->pal[i];
        }
    }
    return col;
}
