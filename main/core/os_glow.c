// Glow: the OS keeps an app's light scene running after the app closes (os_glow.h).
// The effects are documented for apps in docs/LUA_API.md ("Keeping a scene after
// the app closes"); an app that previews them in Lua can match these formulas.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp.h"
#include "os_cmd.h"
#include "os_glow.h"
#include "theme.h"

static const char *TAG = "os_glow";

#define FRAME_MS        30
#define NVS_NS          "glow"
#define NVS_KEY         "scene"
#define BLOB_VERSION    1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const struct {
    const char *name;
    bool spatial;
} s_fx[OS_GLOW_FX_COUNT] = {
    [OS_GLOW_SOLID] = {"solid"},       [OS_GLOW_BREATHE] = {"breathe"}, [OS_GLOW_BLINK] = {"blink"},
    [OS_GLOW_STROBE] = {"strobe"},     [OS_GLOW_FADE] = {"fade"},       [OS_GLOW_HUE] = {"hue"},
    [OS_GLOW_TWINKLE] = {"twinkle"},   [OS_GLOW_CANDLE] = {"candle"},   [OS_GLOW_RAINBOW] = {"rainbow", true},
    [OS_GLOW_GRADIENT] = {"gradient", true}, [OS_GLOW_WAVE] = {"wave", true}, [OS_GLOW_COMET] = {"comet", true},
    [OS_GLOW_SCANNER] = {"scanner", true},   [OS_GLOW_THEATER] = {"theater", true},
    [OS_GLOW_POLICE] = {"police", true},     [OS_GLOW_OFF] = {"off"},
};

// What NVS keeps: the scene and whether it runs.
typedef struct {
    uint8_t version;
    uint8_t on;
    os_glow_scene_t scene;
} glow_blob_t;

static SemaphoreHandle_t s_mx;
static esp_timer_handle_t s_timer;
static os_glow_scene_t s_scene;
static bool s_on;
static uint32_t s_hold;
static int64_t s_last_ms;
static float s_spark[OS_GLOW_LEDS], s_flame[OS_GLOW_LEDS];
static uint32_t s_rand = 0x9E3779B9;

int os_glow_fx_parse(const char *name)
{
    for (int i = 0; name && i < OS_GLOW_FX_COUNT; i++) {
        if (!strcmp(name, s_fx[i].name)) {
            return i;
        }
    }
    return -1;
}

const char *os_glow_fx_name(int fx)
{
    return fx >= 0 && fx < OS_GLOW_FX_COUNT ? s_fx[fx].name : "?";
}

bool os_glow_fx_spatial(int fx)
{
    return fx >= 0 && fx < OS_GLOW_FX_COUNT && s_fx[fx].spatial;
}

// ---------------------------------------------------------------------------
// Effects: i is the LED (0-based), t the time in ms, p the phase in the effect's cycle

static float rnd(void)                      // 0..1, xorshift32: no locking, no radio needed
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return (s_rand >> 8) / 16777216.0f;
}

static void put(float *o, uint32_t c, float k)
{
    o[0] = ((c >> 16) & 0xFF) * k;
    o[1] = ((c >> 8) & 0xFF) * k;
    o[2] = (c & 0xFF) * k;
}

static void mix(float *o, uint32_t a, uint32_t b, float t)
{
    for (int s = 0; s < 3; s++) {
        int sh = 16 - s * 8;
        float ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        o[s] = ca + (cb - ca) * t;
    }
}

static void hsv(float *o, float h, float s, float v)
{
    h = (h - floorf(h)) * 6;
    int i = (int)h;
    float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    o[0] = r * 255;
    o[1] = g * 255;
    o[2] = b * 255;
}

static void compute(int fx, int i, int64_t t, int ms, const os_glow_led_t *c, float *o)
{
    const int n = OS_GLOW_LEDS;
    int L = 6000 / (c->speed ? c->speed : 1);
    float p = (float)(t % L) / L;
    float pos = (float)i / n;               // i is 0-based here
    switch (fx) {
    case OS_GLOW_SOLID:   put(o, c->c1, 1); break;
    case OS_GLOW_BREATHE: put(o, c->c1, 0.03f + 0.97f * (0.5f - 0.5f * cosf(2 * M_PI * p))); break;
    case OS_GLOW_BLINK:   put(o, p < 0.5f ? c->c1 : c->c2, 1); break;
    case OS_GLOW_STROBE:  put(o, p < 0.12f ? c->c1 : c->c2, 1); break;
    case OS_GLOW_FADE:    mix(o, c->c1, c->c2, 0.5f - 0.5f * cosf(2 * M_PI * p)); break;
    case OS_GLOW_HUE:     hsv(o, p, 1, 1); break;
    case OS_GLOW_RAINBOW: hsv(o, p + pos, 1, 1); break;
    case OS_GLOW_GRADIENT: mix(o, c->c1, c->c2, (float)i / (n - 1)); break;
    case OS_GLOW_WAVE:    mix(o, c->c2, c->c1, 0.5f + 0.5f * sinf(2 * M_PI * (p - pos))); break;
    case OS_GLOW_COMET: {
        float d = fmodf(p * n - i, n);
        if (d < 0) {
            d += n;
        }
        mix(o, c->c2, c->c1, fmaxf(0, 1 - d / 3));
        break;
    }
    case OS_GLOW_SCANNER: {
        float x = (p < 0.5f ? p * 2 : (1 - p) * 2) * (n - 1);
        mix(o, c->c2, c->c1, fmaxf(0, 1 - fabsf(x - i) / 1.5f));
        break;
    }
    case OS_GLOW_THEATER: put(o, (i + (int)(p * 6)) % 3 == 0 ? c->c1 : c->c2, 1); break;
    case OS_GLOW_POLICE: {
        bool left = i < n / 2;
        float q = left ? p : fmodf(p + 0.5f, 1);
        bool lit = q < 0.1f || (q > 0.2f && q < 0.3f);
        put(o, left ? c->c1 : c->c2, lit ? 1 : 0);
        break;
    }
    case OS_GLOW_TWINKLE: {
        float s = s_spark[i] - ms * c->speed / 1500.0f;
        if (rnd() < ms * c->speed / 8000.0f) {
            s = 1;
        }
        s_spark[i] = s > 0 ? s : 0;
        mix(o, c->c2, c->c1, s_spark[i]);
        break;
    }
    case OS_GLOW_CANDLE: {
        float f = s_flame[i] + (rnd() - 0.5f) * 0.06f * c->speed;
        f += (0.75f - f) * 0.1f;
        s_flame[i] = f < 0.2f ? 0.2f : f > 1 ? 1 : f;
        put(o, c->c1, s_flame[i]);
        break;
    }
    default:              o[0] = o[1] = o[2] = 0; break;
    }
}

static uint8_t channel(float v, int bright)
{
    int x = (int)floorf(v + 0.5f);
    x = x < 0 ? 0 : x > 255 ? 255 : x;
    x = x * bright / 100;                   // the scene's brightness, applied before the cap
    return x * OS_LED_MAX / 255;            // then the OS cap, as led.set does
}

static void frame(void *arg)
{
    if (xSemaphoreTake(s_mx, 0) != pdTRUE) {
        return;                             // a hold or a new scene is landing: skip this frame
    }
    int64_t now = esp_timer_get_time() / 1000;
    int ms = s_last_ms ? (int)(now - s_last_ms) : FRAME_MS;
    s_last_ms = now;
    if (s_on && !s_hold) {
        for (int i = 0; i < OS_GLOW_LEDS; i++) {
            const os_glow_led_t *c = &s_scene.led[s_scene.each ? i : 0];
            float o[3];
            compute(c->fx, i, now, ms, c, o);
            bsp_rgb_set(i, channel(o[0], s_scene.bright), channel(o[1], s_scene.bright),
                        channel(o[2], s_scene.bright));
        }
        bsp_rgb_show();
    }
    xSemaphoreGive(s_mx);
}

// ---------------------------------------------------------------------------
// State

static void save(void)
{
    glow_blob_t b = { .version = BLOB_VERSION, .on = s_on, .scene = s_scene };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, &b, sizeof(b));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void sanitize(os_glow_scene_t *sc)
{
    sc->bright = sc->bright < 1 ? 1 : sc->bright > 100 ? 100 : sc->bright;
    for (int i = 0; i < OS_GLOW_LEDS; i++) {
        os_glow_led_t *l = &sc->led[i];
        l->fx = l->fx < OS_GLOW_FX_COUNT ? l->fx : OS_GLOW_OFF;
        l->speed = l->speed < 1 ? 1 : l->speed > 10 ? 10 : l->speed;
        l->c1 &= 0xFFFFFF;
        l->c2 &= 0xFFFFFF;
    }
}

// Call with s_mx held.
static void apply_locked(void)
{
    if (s_on) {
        if (!esp_timer_is_active(s_timer)) {
            s_last_ms = 0;
            esp_timer_start_periodic(s_timer, FRAME_MS * 1000);
        }
    } else {
        esp_timer_stop(s_timer);
        if (!s_hold) {
            bsp_rgb_fill(0, 0, 0);
            bsp_rgb_show();
        }
    }
}

void os_glow_set(const os_glow_scene_t *scene)
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (scene) {
        s_scene = *scene;
        sanitize(&s_scene);
        for (int i = 0; i < OS_GLOW_LEDS; i++) {
            s_spark[i] = 0;
            s_flame[i] = 0.7f;
        }
    }
    s_on = scene != NULL;
    save();
    apply_locked();
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "%s", s_on ? "scene set" : "off");
}

bool os_glow_on(void)
{
    return s_on;
}

void os_glow_hold(os_glow_hold_t who, bool on)
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_hold = on ? s_hold | who : s_hold & ~(uint32_t)who;
    xSemaphoreGive(s_mx);
}

// ---------------------------------------------------------------------------
// Console

static int cmd_glow(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        os_glow_set(NULL);
    } else if (argc >= 2 && !strcmp(argv[1], "on")) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        os_glow_scene_t sc = s_scene;
        xSemaphoreGive(s_mx);
        os_glow_set(&sc);
    } else if (argc >= 2) {
        printf("usage: glow [on|off]\n");
        return 1;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    os_glow_scene_t sc = s_scene;
    bool on = s_on;
    uint32_t hold = s_hold;
    xSemaphoreGive(s_mx);
    if (!sc.bright) {
        printf("glow: off, no scene kept yet (an app with the led permission calls led.keep)\n");
        return 0;
    }
    printf("glow: %s, %s, brightness %d%%%s%s%s\n", on ? "on" : "off",
           sc.each ? "one effect per LED" : os_glow_fx_name(sc.led[0].fx), sc.bright,
           hold & OS_GLOW_HOLD_APP ? ", paused for an app" : "",
           hold & OS_GLOW_HOLD_FLASH ? ", paused for a flash" : "",
           hold & OS_GLOW_HOLD_DEBUG ? ", paused for HW Debug" : "");
    if (sc.each) {
        for (int i = 0; i < OS_GLOW_LEDS; i++) {
            const os_glow_led_t *l = &sc.led[i];
            printf("  LED %d: %s #%06lX #%06lX speed %d\n", i + 1, os_glow_fx_name(l->fx),
                   (unsigned long)l->c1, (unsigned long)l->c2, l->speed);
        }
    } else {
        const os_glow_led_t *l = &sc.led[0];
        printf("  %s #%06lX #%06lX speed %d\n", os_glow_fx_name(l->fx), (unsigned long)l->c1,
               (unsigned long)l->c2, l->speed);
    }
    return 0;
}

void os_glow_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = { .callback = frame, .name = "glow" };
    if (!s_mx || esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the glow");
        s_mx = NULL;
        return;
    }
    for (int i = 0; i < OS_GLOW_LEDS; i++) {
        s_flame[i] = 0.7f;
    }
    s_rand ^= (uint32_t)esp_timer_get_time();
    glow_blob_t b;
    size_t len = sizeof(b);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &b, &len) == ESP_OK && len == sizeof(b) && b.version == BLOB_VERSION) {
            s_scene = b.scene;
            sanitize(&s_scene);
            s_on = b.on;
        }
        nvs_close(h);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    apply_locked();
    xSemaphoreGive(s_mx);
    os_cmd_register("glow", "glow [on|off] - the LED scene kept after an app closes (led.keep)", cmd_glow);
    if (s_on) {
        ESP_LOGI(TAG, "running the saved scene");
    }
}
