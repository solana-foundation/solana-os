// ui_frame and files_read (2.3.0) with hostile sizes, pointers and names.
// CASE 0 (t-c-frame): checks that must answer without stopping the app, one
// "ok"/"FAIL" line each; run.ts puts data.bin (1000 bytes, byte i = i % 251)
// in the app's folder first. CASE 1..n (t-c-fr-*): misuse that must stop the
// app with its error, which run.ts checks.
#include "../tlib.h"

#define W 32
#define H 16
static unsigned short px[W * H] __attribute__((aligned(64)));
static unsigned char buf[2048];

static void chk(int good, const char *what)
{
    char line[96], *o = line;
    const char *p = good ? "ok " : "FAIL ";
    while (*p) *o++ = *p++;
    while (*what && o < line + 90) *o++ = *what++;
    *o = 0;
    say(line);
}

static int rd(const char *name, unsigned off, void *dst, unsigned len)
{
    return sos_file_read(name, slen(name), off, dst, len);
}

SOS_EXPORT(on_start) void on_start(void)
{
#if CASE == 0
    for (int i = 0; i < W * H; i++) px[i] = (unsigned short)(i * 37);
    chk(sos_frame(px, W, H, 0, 0, W, H, 0) == 1, "frame shown");
    chk(sos_frame(px, W, H, 0, 0, W, H, SOS_FRAME_BE) == 1, "frame big-endian, no copy");
    chk(sos_frame(px, W, H, 10, 10, 3 * W, 2 * H, 0) == 1, "frame scaled");
    chk(sos_frame(px, W, H, 320 - W + 1, 0, W, H, 0) == -1, "frame past the right edge");
    chk(sos_frame(px, W, H, 0, 218 - H + 1, W, H, 0) == -1, "frame past the bottom");
    chk(sos_frame(px, W, H, -1, 0, W, H, 0) == -1, "frame at x -1");
    chk(sos_frame(px, W, H, 0, 0, 0, H, 0) == -1, "frame of width 0");
    chk(sos_frame(px, W, H, 0, 0, 0x7fffffff, 0x7fffffff, 0) == -1, "frame of a huge size");
    chk(sos_frame(px, W, H, 0x7fffffff, 0x7fffffff, W, H, 0) == -1, "frame at a huge position");
    chk(rd("data.bin", 0, buf, 1000) == 1000, "read whole");
    int same = 1;
    for (int i = 0; i < 1000; i++) same &= buf[i] == i % 251;
    chk(same, "read the right bytes");
    chk(rd("data.bin", 990, buf, 100) == 10, "read the end");
    chk(rd("data.bin", 1000, buf, 100) == 0, "read at the end");
    chk(rd("data.bin", 0xffffffffu, buf, 100) == 0, "read far past the end");
    chk(rd("data.bin", 5, buf, 0) == 0, "read of nothing");
    chk(rd("missing.bin", 0, buf, 10) == -1, "missing file");
    chk(rd("../app.json", 0, buf, 10) == -1, "a path up");
    chk(rd("..", 0, buf, 10) == -1, "dot dot");
    chk(rd("save/../../app.json", 0, buf, 10) == -1, "a path up from save/");
    chk(rd("/sdcard/hh/apps/t-c-frame/app.json", 0, buf, 10) == -1, "an absolute path");
    chk(rd(".hidden", 0, buf, 10) == -1, "a dot file");
    chk(rd("", 0, buf, 10) == -1, "an empty name");
    chk(rd("a\\b", 0, buf, 10) == -1, "a backslash");
    chk(sos_file_read("data.bin", 4000, 0, buf, 10) == -1, "a name longer than any");
    chk(rd("save/none.sav", 0, buf, 10) == -1, "a missing save");
    say("done");
#elif CASE == 1     // the pixels outside the app's memory
    sos_frame((const unsigned short *)0x7ffffff0, W, H, 0, 0, W, H, 0);
#elif CASE == 2     // a side over 1024
    sos_frame(px, 2000, 1, 0, 0, 200, 1, 0);
#elif CASE == 3     // flags it doesn't know
    sos_frame(px, W, H, 0, 0, W, H, 0x80);
#elif CASE == 4     // more than 256 KB in one read
    rd("data.bin", 0, buf, 300000);
#elif CASE == 5     // a buffer outside the app's memory
    rd("data.bin", 0, (void *)0x7ffff000, 2048);
#elif CASE == 6     // the name outside the app's memory
    sos_file_read((const char *)0x7ffffff0, 32, 0, buf, 10);
#elif CASE == 7     // no "files" permission (built with an app.json without it)
    rd("data.bin", 0, buf, 10);
#elif CASE == 8     // a frame of 1024 x 1024 pixels: 2 MB the app doesn't have
    sos_frame(px, 1024, 1024, 0, 0, 320, 218, 0);
#elif CASE == 9     // the name outside the app's memory, nothing to read (len 0)
    sos_file_read((const char *)0x7ffffff0, 32, 0, buf, 0);
#elif CASE == 10    // the name outside the app's memory, a read
    sos_file_read((const char *)0x7ffffff0, 8, 0, buf, 10);
#elif CASE == 11    // the name straddles the end of the app's memory
    sos_file_read((const char *)(__builtin_wasm_memory_size(0) * 65536 - 4), 16, 0, buf, 0);
#endif
    say("still running");
}
