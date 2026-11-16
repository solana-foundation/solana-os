// Ways a C app stops, one per build (build.sh compiles this with -DCASE=n
// into t-c-f-<name>). Each must end on the error screen with a readable
// message, or (exit, grow) not at all; run.ts checks the console line
// "[lua:<id>] <title>: <message>" and that the badge carries on.
#include "../tlib.h"

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_exit")))
void wasi_proc_exit(int code);

static char s_big[64 * 1024];
static volatile int s_zero;

static int spin(void *ud, int nargs)
{
    (void)ud;
    (void)nargs;
    for (;;) {
    }
}

static int crash(void *ud, int nargs)
{
    (void)ud;
    (void)nargs;
    __builtin_trap();
}

static int deep(int n)
{
    volatile int pad[16];
    pad[0] = n;
    return deep(n + 1) + pad[0];
}

static void nothing(void)
{
}

// About n wasm instructions (a volatile loop: 12 a round at -O2).
static void burn(int n)
{
    volatile int i = 0;
    while (i < n / 12) {
        i = i + 1;
    }
}

static int burner(void *ud, int nargs)
{
    (void)ud;
    (void)nargs;
    burn(1200000);
    say("burner finished: the budget wasn't shared");
    return 0;
}

static int short_cb(void *ud, int nargs)
{
    (void)ud;
    (void)nargs;
    return 0;
}

static int s_depth;

// One more level through pcall each time, until pcall reports an error.
static int nest(void *ud, int nargs)
{
    (void)nargs;
    int depth = (int)ud;
    s_depth = depth;
    G("pcall");
    sos_push_fn(nest, (void *)(depth + 1));
    int rc = sos_call(1, 2);
    if (rc == 0 && !sos_to_bool(-2) && depth == s_depth - 1) {
        char err[100];
        sos_to_str(-1, err, sizeof(err));
        say(err);
    }
    sos_pop(rc == 0 ? 2 : 1);
    return 0;
}

// Calls fn(ud) through Lua's pcall: a nested callback.
static void through_pcall(sos_fn fn)
{
    G("pcall");
    sos_push_fn(fn, 0);
    int rc = sos_call(1, SOS_MULTRET);
    say_int("pcall came back: rc ", rc);     // must not be reached: the app stops
}

SOS_EXPORT(on_start) void on_start(void)
{
    say("start");
#if CASE == 1           // loop: a stuck loop
    for (;;) {
    }
#elif CASE == 2         // loopcb: a stuck loop in a callback, caught by pcall
    through_pcall(spin);
#elif CASE == 3         // trapcb: a trap in a callback, caught by pcall
    through_pcall(crash);
#elif CASE == 4         // grow: linear memory up to the cap, refused politely
    int pages = 0;
    while (__builtin_wasm_memory_grow(0, 16) != -1) {     // 1 MB at a time
        pages += 16;
    }
    say_int("memory.grow refused after pages: ", pages);
    G("sys.memory");
    sos_call(0, 1);
    sos_get(1, "used", 4);
    say_int("sys.memory used ", sos_to_int(-1));
    sos_pop(2);
    check(pages > 0, "grew, then -1 at the cap");
    say("still running");
#elif CASE == 5         // luamem: the Lua side of the heap runs out (on_tick, below: copies cost host steps)
    say("filling the Lua heap");
#elif CASE == 6         // index: an index outside the frame
    sos_push_int(1);
    sos_to_int(5);
#elif CASE == 7         // pointer: a pointer outside the app's memory
    sos_push_str((const char *)0x7ffffff0, 100);
#elif CASE == 8         // oob: a load outside memory
    say_int("read ", *(volatile int *)0x7ffffff0);
#elif CASE == 9         // unreachable
    __builtin_trap();
#elif CASE == 10        // div0
    say_int("divided ", 10 / s_zero);
#elif CASE == 11        // stack: unbounded recursion
    say_int("deep ", deep(0));
#elif CASE == 12        // indirect: a function pointer of the wrong type
    int (*volatile f)(int, int, int) = (int (*)(int, int, int))(void *)nothing;
    say_int("called ", f(1, 2, 3));
#elif CASE == 13        // fail: sos_fail
    sos_fail("Custom stop from the app", 24);
#elif CASE == 14        // exit: proc_exit, like sys.exit()
    wasi_proc_exit(0);
    say("after proc_exit: must not be printed");
#elif CASE == 15        // pushfn: not a function of the table
    sos_push_fn((sos_fn)(void *)12345, 0);
#elif CASE == 16        // frame: 65 values
    for (int i = 0; i < 65; i++) {
        sos_push_int(i);
    }
#elif CASE == 17        // unref: a ref that isn't one
    sos_unref(77);
#elif CASE == 18        // nullfn: a NULL function pointer
    void (*volatile g)(void) = 0;
    g();
#elif CASE == 19        // shared: on_start and a callback it calls share one budget (2 M)
    burn(1200000);
    through_pcall(burner);
#elif CASE == 20        // unshared: the same, with a short callback: fits
    burn(1200000);
    G("pcall");
    sos_push_fn(short_cb, 0);
    check(sos_call(1, 1) == 0 && sos_to_bool(-1), "a short callback after a long start");
    sos_pop(1);
    say("still running");
#elif CASE == 21        // nest: callbacks nested through pcall until something gives
    nest(0, 0);
    say_int("nesting stopped at depth ", s_depth);
    say("still running");
#elif CASE == 22        // fspin: a stuck loop of soft-float maths (native: mostly in the helpers)
    volatile float f = 1.0f;
    for (;;) {
        f = f * 1.0001f + 0.5f / (f + 1.0f);
    }
#elif CASE == 23        // importloop: a stuck loop that calls the badge all the time
    for (;;) {
        sos_top();
    }
#elif CASE == 24        // nres: a result count no frame holds (with values below, the sum mustn't wrap)
    sos_push_int(1);
    sos_push_int(2);
    G("pcall");
    sos_call(0, 0x7fffffff);
#endif
    say("on_start end");
}

#if CASE == 5
// 8 strings of 64 KB a tick: 512 KB, 131 072 of a callback's 200 000 host
// steps (LW_COPY_BYTES), until the heap's cap stops it.
SOS_EXPORT(on_tick) void on_tick(int32_t ms)
{
    (void)ms;
    for (int i = 0; i < 8; i++) {
        sos_push_str(s_big, sizeof(s_big));
        sos_ref();
    }
}
#endif
