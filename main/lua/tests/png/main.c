// Host test for lua_png.c and lua_gif.c (see png_test.py). PNG: prints "w h"
// and one RGB565 value per pixel in hex. GIF: "GIF w h loops", then for each
// frame "F <delay ms>" and its canvas. Or "ERR <message>".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lua_gif.h"
#include "../../lua_png.h"

static size_t rd(void *ud, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ud);
}

static int gif(FILE *f)
{
    lh_gif_t g;
    lh_gif_work_t *w = malloc(sizeof(*w));
    uint32_t pos;
    const char *err = lh_gif_open(&g, w, rd, f, &pos);
    if (err) {
        printf("ERR %s\n", err);
        return 0;
    }
    uint16_t *canvas = malloc((size_t)g.w * g.h * 2);
    for (size_t i = 0; i < (size_t)g.w * g.h; i++) {
        canvas[i] = LH_PX_CLEAR;
    }
    printf("GIF %d %d\n", g.w, g.h);
    for (int n = 0; n < 1000; n++) {
        bool done;
        fseek(f, pos, SEEK_SET);        // as an animation does: reopen at the offset
        err = lh_gif_frame(&g, w, rd, f, canvas, &pos, &done);
        if (err) {
            printf("ERR %s\n", err);
            return 0;
        }
        if (done) {
            break;
        }
        printf("F %d\n", g.delay_ms);
        for (size_t i = 0; i < (size_t)g.w * g.h; i++) {
            printf("%04x\n", canvas[i]);
        }
    }
    printf("LOOPS %d\n", g.loops);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) {
        printf("ERR open\n");
        return 0;
    }
    char sig[4] = {0};
    if (fread(sig, 1, 4, f) == 4 && !memcmp(sig, "GIF8", 4)) {
        rewind(f);
        return gif(f);
    }
    rewind(f);
    lh_png_info_t info;
    const char *err = lh_png_info(rd, f, &info);
    if (err) {
        printf("ERR %s\n", err);
        return 0;
    }
    uint8_t *raw = malloc(lh_png_raw_size(&info));
    uint16_t *out = malloc((size_t)info.w * info.h * 2);
    lh_png_work_t *w = malloc(sizeof(*w));
    err = lh_png_decode(rd, f, &info, w, raw, out);
    if (err) {
        printf("ERR %s\n", err);
        return 0;
    }
    printf("%d %d\n", info.w, info.h);
    for (size_t i = 0; i < (size_t)info.w * info.h; i++) {
        printf("%04x\n", out[i]);
    }
    return 0;
}
