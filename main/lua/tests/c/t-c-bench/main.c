// Speed of a C app, interpreted (app.wasm) or native (app.aot): prints what
// one API call through the bridge costs, a full-screen sos_blit, a burst of
// plain computation (with `lua mem`'s instruction count, the interpreter's
// rate), and a starfield frame (examples/starfield's drawing, 160 x 108
// shown at 2x) as frames a second over 3 s. Built like a store app
// (badge-cc); run.ts and the badge pass check that it prints its lines.
#include <stdio.h>
#include "badge.h"

#define W 160
#define H 108
#define STARS 220
#define DEPTH 1024
#define FULL_W 320
#define FULL_H 216

typedef struct {
    int32_t x, y, z;
    int gem;
} star;

static uint16_t frame[W * H];
static uint16_t full[FULL_W * FULL_H];
static star stars[STARS];
static ui_obj canvas, label;
static uint32_t seed = 0x9E3779B9u;
static uint32_t t_first, frames, frame_ms, burned;
static volatile int sink;

static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void spawn(star *s, int32_t z)
{
    s->x = (int32_t)(rnd() % 1024) - 512;
    s->y = (int32_t)(rnd() % 1024) - 512;
    s->z = z;
    s->gem = rnd() % 40 == 0;
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

// examples/starfield's frame, without the keys and the score.
static void draw(int32_t ms)
{
    for (int i = 0; i < W * H; i++) {
        frame[i] = RGB565(0, 0, 8);
    }
    for (int i = 0; i < STARS; i++) {
        star *s = &stars[i];
        s->z -= ms;
        if (s->z < 1) {
            spawn(s, DEPTH);
            continue;
        }
        int sx = W / 2 + (int)(s->x * 64 / s->z);
        int sy = H / 2 + (int)(s->y * 64 / s->z);
        int bright = 255 - (int)(s->z * 200 / DEPTH);
        uint16_t c = s->gem ? RGB565(0x14, bright, 0x95 * bright / 255) : RGB565(bright, bright, bright);
        plot(sx, sy, c, s->z < DEPTH / 6 ? 2 : 1);
    }
    ui_canvas_blit(canvas, frame, 0, 0, W, H);
}

void on_start(void)
{
    label = ui_label(6, 200, "bench", &(ui_label_opts){.size = 14, .color = COL_GREEN});
    // One API call: label text, through badge.h (a method call on the object).
    uint32_t t = sys_millis();
    for (int i = 0; i < 500; i++) {
        ui_label_text(label, i & 1 ? "bench" : "bench.");
    }
    uint32_t calls = sys_millis() - t;
    t = sys_millis();
    for (int i = 0; i < 2000; i++) {
        sink += (int)sys_keys();            // the cheapest import there is
    }
    uint32_t keys = sys_millis() - t;
    // A full-screen canvas, blitted whole.
    ui_obj big = ui_canvas(0, 0, FULL_W, FULL_H, 1, COL_BLACK);
    for (int i = 0; i < FULL_W * FULL_H; i++) {
        full[i] = (uint16_t)(i * 7);
    }
    t = sys_millis();
    for (int i = 0; i < 10; i++) {
        ui_canvas_blit(big, full, 0, 0, FULL_W, FULL_H);
    }
    uint32_t blits = sys_millis() - t;
    ui_obj_remove(&big);
    printf("bench: label text %u us a call, sys_keys %u us a call, full-canvas blit %u us\n",
           (unsigned)(calls * 1000 / 500), (unsigned)(keys * 1000 / 2000), (unsigned)(blits * 1000 / 10));
    canvas = ui_canvas(0, 0, W, H, 2, COL_BLACK);
    for (int i = 0; i < STARS; i++) {
        spawn(&stars[i], 1 + (int32_t)(rnd() % DEPTH));
    }
}

void on_tick(int32_t ms)
{
    if (!burned) {
        // About 1.5 M wasm instructions of plain work (12 a round at -O2).
        volatile int i = 0;
        uint32_t t = sys_millis();
        while (i < 1500000 / 12) {
            i = i + 1;
        }
        burned = sys_millis() - t + 1;
        printf("bench: burn %u ms (lua mem: most wasm instructions in one callback / this = per ms)\n",
               (unsigned)burned - 1);
        return;
    }
    uint32_t t = sys_millis();
    draw(ms);
    frame_ms += sys_millis() - t;
    if (!frames++) {
        t_first = t;
    }
    if (frames == 1 + 100 || (frames > 1 && sys_millis() - t_first >= 3000)) {
        uint32_t span = sys_millis() - t_first;
        printf("bench: starfield %u frames in %u ms: %u.%u fps, %u ms drawing a frame\n", (unsigned)(frames - 1),
               (unsigned)span, (unsigned)((frames - 1) * 1000 / span), (unsigned)((frames - 1) * 10000 / span % 10),
               (unsigned)(frame_ms / frames));
        printf("bench: done\n");
        frames = 0;
        frame_ms = 0;
        t_first = 0;
    }
}
