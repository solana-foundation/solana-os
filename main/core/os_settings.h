#pragma once

// User settings, persisted in NVS namespace "settings". Portable (the emulator
// keeps NVS in localStorage). Values are cached after the first read; call
// these from the OS task (app callbacks).

typedef enum {
    OS_SET_THEME,           // OS_THEME_DARK (default) or OS_THEME_LIGHT; see ui_col()
    OS_SET_BRIGHTNESS,      // screen backlight level 1..OS_BRIGHTNESS_MAX (default: brightest); see os_brightness_apply()
    OS_SET_LCD_INVERT,      // 1 (default: production panels) or 0 (prototypes): colour inversion; see os_invert_apply()
    OS_SET_GOSSIP,          // 1 (default) or 0: Gossip on (game/gossip.h, 2.4.0; the gossip service's switch)
    OS_SET_LEADER,          // 0 (default) or 1: "May become a leader", Gossip roles (game/gossip_role.h, 2.4.0)
    OS_SET_COUNT,
} os_setting_t;

enum { OS_THEME_DARK = 0, OS_THEME_LIGHT = 1 };
enum { OS_BRIGHTNESS_MIN = 1, OS_BRIGHTNESS_MAX = 5 };

int os_setting(os_setting_t s);
void os_setting_set(os_setting_t s, int value);
