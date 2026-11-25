// Not shipped: how long ui_frame takes on a badge, for C_API.md "Heavy apps
// and emulators". A 160 x 144 frame (a Game Boy's), 20 times each way, one
// way a tick (a callback's host steps cap the frames it may send):
// unscaled big-endian from a 64-byte-aligned buffer (the no-copy path),
// unscaled little-endian (converted), and scaled 1.5x. Each call returns while
// the panel still takes the frame, and the next waits for it, so the average
// is the cost a game sees per frame. `lua mem` afterwards says how many went
// without a copy. Needs no permission and no server. Lines start "t-c-fbench".
#include "../tlib.h"

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int wasi_clock_time_get(int id, long long precision, long long *time);

#define W 160
#define H 144
#define N 20
static unsigned short px[W * H] __attribute__((aligned(64)));

static long long now_us(void)
{
    long long t = 0;
    wasi_clock_time_get(1, 1000, &t);       // monotonic, ns
    return t / 1000;
}

static void bench(const char *what, int x, int y, int dw, int dh, unsigned flags)
{
    long long t0 = now_us();
    int shown = 0;
    for (int i = 0; i < N; i++) {
        px[i] = (unsigned short)(i * 977);  // a pixel changes: each frame is a new one
        shown += sos_frame(px, W, H, x, y, dw, dh, flags) == 1;
    }
    sos_frame(px, W, H, x, y, dw, dh, flags);   // waits for the last one
    long long us = (now_us() - t0) / (N + 1);
    char b[96], *o = b;
    const char *p = "t-c-fbench ";
    while (*p) *o++ = *p++;
    while (*what) *o++ = *what++;
    *o = 0;
    say_int(b, (int)us);
    say_int("t-c-fbench shown of 20: ", shown);
}

static int s_step;

SOS_EXPORT(on_start) void on_start(void)
{
    for (int i = 0; i < W * H; i++) px[i] = (unsigned short)(i * 37);
}

SOS_EXPORT(on_tick) void on_tick(int32_t ms)
{
    (void)ms;
    switch (s_step++) {
    case 0: bench("direct us per frame: ", 80, 37, W, H, SOS_FRAME_BE); break;
    case 1: bench("copied us per frame: ", 80, 37, W, H, 0); break;
    case 2: bench("scaled 1.5x us per frame: ", 40, 1, W * 3 / 2, H * 3 / 2, SOS_FRAME_BE); break;
    case 3: say("done"); break;
    default: break;
    }
}
