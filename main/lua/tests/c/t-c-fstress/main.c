// Not shipped: frames sent without pause (the no-copy path and the copied one in
// turn, every tick) while the console makes the badge write its flash (NVS
// commits: `collab lend on` / `off`): the DMA reads the frame from PSRAM, which
// is unreachable while the flash is written. Prints a line every 200 frames;
// a hang, a DMA error or a restart is the failure. Needs no permission or server.
#include "../tlib.h"

#define W 160
#define H 144
static unsigned short px[2][W * H] __attribute__((aligned(64)));
static int s_n, s_shown;

SOS_EXPORT(on_start) void on_start(void)
{
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < W * H; i++) px[f][i] = (unsigned short)((i + f * 977) * 37);
}

SOS_EXPORT(on_tick) void on_tick(int32_t ms)
{
    (void)ms;
    for (int k = 0; k < 2; k++) {
        int f = s_n & 1;
        s_shown += sos_frame(px[f], W, H, 80, 37, W, H, (s_n % 3) ? SOS_FRAME_BE : 0) == 1;
        if (++s_n % 200 == 0) say_int("t-c-fstress frames shown: ", s_shown);
    }
}
