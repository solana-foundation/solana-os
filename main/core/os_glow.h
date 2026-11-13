#pragma once

// Glow: a light scene the OS keeps running on the RGB LEDs after the app that
// set it has closed (Lua `led.keep`, docs/LUA_API.md). Rendered by an
// esp_timer every 30 ms and saved in NVS ("glow"), so it comes back after a
// restart. Whatever else drives the LEDs (a Lua app with the `led` permission,
// the game's flashes, HW Debug) holds the glow while it does: no frame is
// drawn until every hold is released. Portable (the emulator builds it too).

#include <stdbool.h>
#include <stdint.h>

#define OS_GLOW_LEDS    8

typedef enum {
    OS_GLOW_SOLID, OS_GLOW_BREATHE, OS_GLOW_BLINK, OS_GLOW_STROBE, OS_GLOW_FADE, OS_GLOW_HUE,
    OS_GLOW_TWINKLE, OS_GLOW_CANDLE, OS_GLOW_RAINBOW, OS_GLOW_GRADIENT, OS_GLOW_WAVE, OS_GLOW_COMET,
    OS_GLOW_SCANNER, OS_GLOW_THEATER, OS_GLOW_POLICE, OS_GLOW_OFF,
    OS_GLOW_FX_COUNT,
} os_glow_fx_t;

typedef struct {
    uint8_t fx;             // os_glow_fx_t
    uint8_t speed;          // 1..10: one cycle takes 6000 / speed ms
    uint32_t c1, c2;        // 0xRRGGBB
} os_glow_led_t;

typedef struct {
    uint8_t bright;         // 1..100 % (of OS_LED_MAX, like an app's led.set)
    bool each;              // true: led[i] runs on LED i; false: led[0] runs across the strip
    os_glow_led_t led[OS_GLOW_LEDS];
} os_glow_scene_t;

typedef enum {
    OS_GLOW_HOLD_APP   = 1 << 0,    // a Lua app with the led permission is open
    OS_GLOW_HOLD_FLASH = 1 << 1,    // game_led_flash / game_led_sweep
    OS_GLOW_HOLD_DEBUG = 1 << 2,    // HW Debug drives the LEDs itself
} os_glow_hold_t;

void os_glow_init(void);                    // loads the saved scene, registers the `glow` console command

int os_glow_fx_parse(const char *name);     // "wave" -> OS_GLOW_WAVE; -1 if unknown
const char *os_glow_fx_name(int fx);
bool os_glow_fx_spatial(int fx);            // works across the strip only (not per LED)

void os_glow_set(const os_glow_scene_t *scene);     // run (and save) a scene; NULL stops it
bool os_glow_on(void);

// While any hold is set the glow draws nothing and leaves the LEDs to the holder.
void os_glow_hold(os_glow_hold_t who, bool on);
