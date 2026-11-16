// Work that must stay inside a callback's limits, one case per build
// (build.sh compiles this with -DCASE=n into t-c-l-<name>). Each loops on
// something whose cost isn't one instruction: the interpreter must charge it
// by length (bulk memory), the bridge by bytes (imports that copy or write),
// or the native watchdog must stop it (on a badge, as app.aot). run.ts
// checks the error line; nothing here may get past its loop.
#include "../tlib.h"

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write")))
int wasi_fd_write(int fd, const void *iovs, int n, unsigned *written);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("random_get")))
int wasi_random_get(void *buf, unsigned len);

#define PAGES   60                  // 3.75 MB of linear memory to fill and copy

static unsigned char *grow(void)
{
    unsigned base = __builtin_wasm_memory_size(0) * 65536u;
    if (__builtin_wasm_memory_grow(0, PAGES) < 0) {
        say("FAIL memory.grow refused");
        return 0;
    }
    return (unsigned char *)base;
}

SOS_EXPORT(on_start) void on_start(void)
{
    say("start");
#if CASE == 1           // fill: memory.fill of megabytes, over and over
    unsigned char *p = grow();
    for (int i = 0; p && i < 100000; i++) {
        __builtin_memset(p, i, PAGES * 65536u);
    }
#elif CASE == 2         // copy: memory.copy of megabytes, over and over
    unsigned char *p = grow();
    unsigned half = PAGES * 65536u / 2;
    for (int i = 0; p && i < 100000; i++) {
        __builtin_memmove(p + (i & 1 ? half : 0), p + (i & 1 ? 0 : half), half);
    }
#elif CASE == 3         // pushstr: Lua strings of megabytes, over and over
    unsigned char *p = grow();
    for (int i = 0; p && i < 100000; i++) {
        sos_push_str((const char *)p, 40 * 65536u);
        sos_pop(1);
    }
#elif CASE == 4         // getkey: a key of megabytes, over and over
    unsigned char *p = grow();
    sos_new_table();
    for (int i = 0; p && i < 100000; i++) {
        sos_get(1, (const char *)p, 40 * 65536u);
        sos_pop(1);
    }
#elif CASE == 5         // fdwrite: stdout writes of megabytes (a short count, then the budget)
    unsigned char *p = grow();
    if (p) {
        for (unsigned i = 0; i < 8192; i++) {     // what one call may take is printable text
            p[i] = i % 64 == 63 ? '\n' : 'a' + i % 26;
        }
        struct { const void *buf; unsigned len; } iov[8];
        for (int i = 0; i < 8; i++) {
            iov[i].buf = p;
            iov[i].len = PAGES * 65536u / 8;
        }
        unsigned wrote = 0;
        int rc = wasi_fd_write(1, iov, 8, &wrote);
        say_int("fd_write rc ", rc);
        say_int("fd_write took bytes: ", (int)wrote);
        for (int i = 0; i < 100000; i++) {
            wasi_fd_write(1, iov, 8, &wrote);
        }
    }
#elif CASE == 6         // random: random_get of a megabyte is refused, then 4 KB at a time
    unsigned char *p = grow();
    if (p) {
        say_int("random_get of 1 MB: ", wasi_random_get(p, 1024 * 1024));
        for (int i = 0; i < 100000; i++) {
            wasi_random_get(p, 4096);
        }
    }
#elif CASE == 7         // log: sos_log of a megabyte (cut to 1 KB a call), over and over
    unsigned char *p = grow();
    if (p) {
        for (unsigned i = 0; i < 1024; i++) {
            p[i] = 'a' + i % 26;
        }
        for (int i = 0; i < 100000; i++) {
            sos_log((const char *)p, 1024 * 1024);
        }
    }
#elif CASE == 8         // ctors: exports __wasm_call_ctors (build.sh), which WAMR would run itself
    say("FAIL a module exporting __wasm_call_ctors started");
#elif CASE == 9         // print: Lua's print through sos_call costs a C app's output too
    unsigned char *p = grow();
    if (p) {
        for (unsigned i = 0; i < 65536; i++) {
            p[i] = 'a' + i % 26;
        }
        for (int i = 0; i < 100000; i++) {
            G("print");
            sos_push_str((const char *)p, 65536);
            sos_call(1, 0);
        }
    }
#endif
    say("FAIL got past the loop: must not be printed");
}
