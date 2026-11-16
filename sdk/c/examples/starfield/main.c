// Starfield: the reason to write an app in C. Every tick the app draws a whole
// frame into its own RGB565 buffer and hands it to the screen in one call
// (ui_canvas_blit). Steer with the d-pad (several keys at once: sys_keys), hold
// SELECT for warp, and fly into the green stars. Integer maths only: the
// badge's CPU has no floating point unit.
#include "badge.h"

#define W 160 // the canvas, shown at 2x: 320 x 216
#define H 108
#define STARS 220
#define DEPTH 1024 // z of a new star; it reaches the screen at z = 1

typedef struct {
    int32_t x, y, z; // x, y in -512..511 around the centre
    int gem;         // green: worth a point when it passes the middle
} star;

static uint16_t frame[W * H];
static star stars[STARS];
static ui_obj canvas, score_label;
static int32_t cx, cy; // where the ship looks, in star units
static int32_t score, best;
static uint32_t seed = 0x9E3779B9u;

static uint32_t rnd(void) // xorshift32: small, no libc
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void spawn(star *s, int32_t z)
{
    s->x = (int32_t)(rnd() % 1024) - 512 + cx;
    s->y = (int32_t)(rnd() % 1024) - 512 + cy;
    s->z = z;
    s->gem = rnd() % 40 == 0;
}

static void show_score(void)
{
    char text[40] = "Score ";
    int32_t v = score, n = 6;
    char digits[12];
    int k = 0;
    do {
        digits[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k) {
        text[n++] = digits[--k];
    }
    text[n] = 0;
    ui_label_text(score_label, text);
}

static void plot(int x, int y, uint16_t c, int size)
{
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < W && py >= 0 && py < H) {
                frame[py * W + px] = c;
            }
        }
    }
}

void on_start(void)
{
    best = storage_get_int("best", 0);
    canvas = ui_canvas(0, 0, W, H, 2, COL_BLACK);
    score_label = ui_label(6, 4, "Score 0", &(ui_label_opts){.size = 14, .color = COL_GREEN});
    ui_hint("D-pad steer   SELECT warp   CANCEL exit");
    seed ^= sys_millis() * 2654435761u;
    for (int i = 0; i < STARS; i++) {
        spawn(&stars[i], 1 + (int32_t)(rnd() % DEPTH));
    }
}

void on_tick(int32_t ms)
{
    uint32_t keys = sys_keys();
    int32_t speed = KEY_HELD(keys, KEY_SELECT) ? 3 : 1; // z units per ms
    int32_t steer = ms / 2;                             // star units per ms
    if (KEY_HELD(keys, KEY_LEFT)) cx -= steer;
    if (KEY_HELD(keys, KEY_RIGHT)) cx += steer;
    if (KEY_HELD(keys, KEY_UP)) cy -= steer;
    if (KEY_HELD(keys, KEY_DOWN)) cy += steer;

    for (int i = 0; i < W * H; i++) {
        frame[i] = RGB565(0, 0, 8);
    }
    for (int i = 0; i < STARS; i++) {
        star *s = &stars[i];
        s->z -= ms * speed;
        if (s->z < 1) {
            // Passing the screen: a gem near the middle scores.
            if (s->gem && s->x - cx > -60 && s->x - cx < 60 && s->y - cy > -60 && s->y - cy < 60) {
                score++;
                show_score();
                led_fill(0x14, 0xF1, 0x95);
            }
            spawn(s, DEPTH);
            continue;
        }
        int sx = W / 2 + (int)((s->x - cx) * 64 / s->z);
        int sy = H / 2 + (int)((s->y - cy) * 64 / s->z);
        int bright = 255 - (int)(s->z * 200 / DEPTH);
        uint16_t c = s->gem ? RGB565(0x14, bright, 0x95 * bright / 255) : RGB565(bright, bright, bright);
        plot(sx, sy, c, s->z < DEPTH / 6 ? 2 : 1);
    }
    plot(W / 2 - 1, H / 2 - 6, RGB565(0x99, 0x45, 0xFF), 2); // the sight
    plot(W / 2 - 1, H / 2 + 4, RGB565(0x99, 0x45, 0xFF), 2);
    plot(W / 2 - 6, H / 2 - 1, RGB565(0x99, 0x45, 0xFF), 2);
    plot(W / 2 + 4, H / 2 - 1, RGB565(0x99, 0x45, 0xFF), 2);
    ui_canvas_blit(canvas, frame, 0, 0, W, H);
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    if (key == KEY_SELECT) {
        led_off();
    }
    return key != KEY_BACK; // steering is read in on_tick; CANCEL exits
}

void on_exit(void)
{
    if (score > best) {
        storage_set_int("best", score);
    }
}
