// Every badge_abi.h import, the WASI calls wasi-libc makes, and the callback
// paths: a timer, a list's on_select, an http answer (offline is fine), a
// key, on_tick, on_mesh's and on_meet's frames, storage, a label, a canvas.
// run.ts presses DOWN, then SELECT (the list), and reads the "ok"/"FAIL"
// lines; "done" means every callback came.
#include "../tlib.h"

// WASI, declared by hand (no libc here)
__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write")))
int wasi_fd_write(int fd, const void *iovs, int n, int *written);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int wasi_clock_time_get(int id, long long precision, long long *time);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("random_get")))
int wasi_random_get(void *buf, int len);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_fdstat_get")))
int wasi_fd_fdstat_get(int fd, void *stat);

static int s_initialized, s_ticks, s_timer_ref, s_label_ref, s_got_timer, s_got_select, s_got_http, s_got_key;
static unsigned short s_px[16 * 8];

__attribute__((export_name("_initialize"))) void initialize(void)
{
    s_initialized = 42;
}

static void stack_basics(void)
{
    check(sos_top() == 0, "on_start's frame starts empty");
    sos_push_nil();
    sos_push_bool(1);
    sos_push_int(-7);
    sos_push_num(2.5f);
    push_s("hello");
    sos_new_table();
    check(sos_top() == 6, "six values pushed");
    check(sos_type(1) == SOS_NIL && sos_type(2) == SOS_BOOL && sos_type(3) == SOS_INT && sos_type(4) == SOS_NUM
          && sos_type(5) == SOS_STR && sos_type(6) == SOS_TABLE, "sos_type of each");
    check(sos_type(7) == SOS_NONE && sos_type(-7) == SOS_NONE && sos_type(0) == SOS_NONE, "sos_type outside the frame");
    check(sos_to_bool(1) == 0 && sos_to_bool(2) == 1 && sos_to_bool(3) == 1, "sos_to_bool");
    check(sos_to_int(3) == -7 && sos_to_int(4) == 2 && sos_to_int(5) == 0, "sos_to_int (float truncates, string 0)");
    check(sos_to_num(4) == 2.5f && sos_to_num(3) == -7.0f && sos_to_num(5) == 0.0f, "sos_to_num");
    char buf[8];
    unsigned n = sos_to_str(5, buf, sizeof(buf));
    check(n == 5 && seq(buf, "hello"), "sos_to_str");
    n = sos_to_str(5, buf, 3);
    check(n == 5 && seq(buf, "he"), "sos_to_str cut, full length back");
    check(sos_to_str(3, buf, sizeof(buf)) == 0 && buf[0] == 0, "sos_to_str of a number is \"\"");
    check(sos_to_str(5, 0, 0) == 5, "sos_to_str with cap 0 writes nothing");
    check(sos_len(5) == 5 && sos_len(6) == 0 && sos_len(3) == 0, "sos_len");
    sos_push_value(-2);                     // "hello" again
    check(sos_top() == 7 && sos_type(-1) == SOS_STR, "sos_push_value");
    sos_remove(1);                          // the nil
    check(sos_top() == 6 && sos_type(1) == SOS_BOOL, "sos_remove");
    sos_pop(6);
    check(sos_top() == 0, "sos_pop");
}

static void tables(void)
{
    sos_new_table();
    sos_push_int(10);
    set_field(1, "a");
    push_s("x");
    sos_seti(1, 1);
    push_s("y");
    sos_seti(1, 2);
    check(sos_top() == 1, "sos_set and sos_seti pop the value");
    check(sos_get(1, "a", 1) == SOS_INT && sos_to_int(-1) == 10, "sos_get");
    sos_pop(1);
    check(sos_get(1, "zz", 2) == SOS_NIL, "sos_get of a missing key");
    sos_pop(1);
    check(sos_geti(1, 2) == SOS_STR && sos_len(-1) == 1, "sos_geti");
    sos_pop(1);
    check(sos_len(1) == 2, "sos_len of a table");
    int keys = 0;
    sos_push_nil();
    while (sos_next(1)) {
        keys++;
        sos_pop(1);                         // the value; the key stays for the next step
    }
    check(keys == 3 && sos_top() == 1, "sos_next walks 3 keys");
    sos_push_int(5);
    check(sos_get(2, "x", 1) == SOS_NIL && sos_top() == 3, "sos_get on a number pushes nil");
    sos_pop(3);
}

static void api_calls(void)
{
    check(G("ui.W") == SOS_INT && sos_to_int(-1) == 320, "sos_global ui.W");
    sos_pop(1);
    check(G("ui.label") == SOS_FN, "sos_global ui.label");
    sos_pop(1);
    check(G("no.such.thing") == SOS_NIL, "sos_global of nothing is nil");
    sos_pop(1);
    check(G("string.upper") == SOS_FN, "sos_global string.upper (lazy library)");
    push_s("abc");
    check(sos_call(1, 1) == 0, "sos_call string.upper");
    char b[8];
    sos_to_str(-1, b, sizeof(b));
    check(seq(b, "ABC"), "its result");
    sos_pop(1);
    // An API refusal comes back to the app.
    G("ui.label");
    push_s("not a number");
    int rc = sos_call(1, 0);
    char err[100];
    sos_to_str(-1, err, sizeof(err));
    check(rc != 0 && sos_top() == 1 && sos_type(-1) == SOS_STR, "sos_call returns an API error");
    say(err);
    sos_pop(1);
    // MULTRET
    G("string.byte");
    push_s("AB");
    sos_push_int(1);
    sos_push_int(2);
    check(sos_call(3, SOS_MULTRET) == 0 && sos_top() == 2 && sos_to_int(1) == 65 && sos_to_int(2) == 66,
          "sos_call SOS_MULTRET");
    sos_pop(2);
    // json round trip through the API
    G("json.encode");
    sos_new_table();
    sos_push_int(3);
    set_field(-2, "n");
    sos_call(1, 1);
    sos_to_str(-1, b, sizeof(b));
    check(seq(b, "{\"n\":3}"), "json.encode");
    sos_pop(1);
    // sys.memory counts the module too
    G("sys.memory");
    sos_call(0, 1);
    sos_get(1, "used", 4);
    int used = sos_to_int(-1);
    sos_get(1, "max", 3);
    int max = sos_to_int(-1);
    say_int("sys.memory used ", used);
    say_int("sys.memory max ", max);
    check(used > 65536 && max >= 96 * 1024, "sys.memory counts the module's memory");
    sos_pop(3);
}

static void refs(void)
{
    push_s("kept");
    int r = sos_ref();
    check(r > 0 && sos_top() == 0, "sos_ref pops and names it");
    sos_push_nil();
    check(sos_ref() == 0 && sos_top() == 0, "sos_ref of nil is 0");
    check(sos_push_ref(r) == SOS_STR, "sos_push_ref");
    char b[8];
    sos_to_str(-1, b, sizeof(b));
    check(seq(b, "kept"), "the kept value");
    sos_pop(1);
    check(sos_push_ref(0) == SOS_NIL, "sos_push_ref(0) is nil");
    sos_pop(1);
    sos_unref(r);
    sos_unref(0);
    check(sos_top() == 0, "sos_unref");
}

static void storage(void)
{
    G("storage.set");
    push_s("best");
    sos_push_int(1234);
    check(sos_call(2, 0) == 0, "storage.set");
    G("storage.get");
    push_s("best");
    sos_call(1, 1);
    check(sos_to_int(-1) == 1234, "storage.get");
    sos_pop(1);
}

static void wasi(void)
{
    struct { const char *p; int n; } iov[2] = { { "printf-style ", 13 }, { "line\nsecond\x01line\n", 17 } };
    int written = 0;
    check(wasi_fd_write(1, iov, 2, &written) == 0 && written == 30, "fd_write");
    long long t = 0, t2 = 0;
    check(wasi_clock_time_get(1, 1, &t) == 0 && t > 0, "clock_time_get monotonic");
    wasi_clock_time_get(1, 1, &t2);
    check(t2 >= t, "monotonic moves forward");
    unsigned char r[16] = { 0 };
    int any = 0;
    check(wasi_random_get(r, 16) == 0, "random_get");
    for (int i = 0; i < 16; i++) {
        any |= r[i];
    }
    check(any != 0, "random bytes");
    unsigned char st[24];
    check(wasi_fd_fdstat_get(1, st) == 0 && st[0] == 2, "fd_fdstat_get: stdout is a character device");
    check(wasi_fd_fdstat_get(5, st) == 8, "fd_fdstat_get of another fd: EBADF");
}

static int on_timer(void *ud, int nargs)
{
    s_got_timer = 1;
    check(ud == (void *)77 && nargs == 0, "timer callback (ud, no arguments)");
    sos_unref(s_timer_ref);
    return 0;
}

static int on_select(void *ud, int nargs)
{
    (void)ud;
    s_got_select = 1;
    check(nargs == 2 && sos_type(1) == SOS_INT && sos_type(2) == SOS_STR, "list on_select (i, item)");
    say_int("selected ", sos_to_int(1));
    return 0;
}

static int on_http(void *ud, int nargs)
{
    (void)ud;
    s_got_http = 1;
    char b[64];
    sos_to_str(2, b, sizeof(b));
    check(nargs == 2 && sos_type(1) == SOS_BOOL, "http callback (ok, res)");
    say(b);
    return 0;
}

// Returns two values to Lua: called through pcall below.
static int two(void *ud, int nargs)
{
    (void)ud;
    sos_push_int(nargs);
    sos_push_int(5);
    return 2;
}

static void callbacks(void)
{
    G("timer.after");
    sos_push_int(50);
    sos_push_fn(on_timer, (void *)77);
    sos_push_value(-1);
    s_timer_ref = sos_ref();
    check(sos_call(2, 1) == 0 && sos_type(-1) == SOS_INT, "timer.after with a C callback");
    sos_pop(1);
    // A C function called from Lua with results: pcall(two, 1, 2, 3) -> true, 3, 5
    G("pcall");
    sos_push_fn(two, 0);
    sos_push_int(1);
    sos_push_int(2);
    sos_push_int(3);
    check(sos_call(4, 3) == 0 && sos_to_bool(1) && sos_to_int(2) == 3 && sos_to_int(3) == 5,
          "a C function returns 2 results through pcall");
    sos_pop(3);
    // A list with an on_select
    G("ui.list");
    sos_push_int(0);
    sos_push_int(40);
    sos_push_int(320);
    sos_push_int(120);
    sos_new_table();
    push_s("first");
    sos_seti(-2, 1);
    push_s("second");
    sos_seti(-2, 2);
    sos_new_table();
    sos_push_fn(on_select, 0);
    set_field(-2, "on_select");
    check(sos_call(6, 1) == 0 && sos_type(-1) == SOS_OBJ, "ui.list");
    sos_pop(1);
    // http (needs Wi-Fi: offline in the test, which still calls back)
    G("http.get");
    push_s("https://example.com/");
    sos_push_fn(on_http, 0);
    int rc = sos_call(2, 0);
    check(rc == 0, "http.get queued");
}

static void ui_bits(void)
{
    G("ui.label");
    sos_push_int(8);
    sos_push_int(8);
    push_s("C app");
    check(sos_call(3, 1) == 0 && sos_type(-1) == SOS_OBJ, "ui.label");
    sos_push_value(-1);
    s_label_ref = sos_ref();
    // label:text("C app running")
    sos_get(1, "text", 4);
    sos_push_value(1);
    push_s("C app running");
    check(sos_call(2, 0) == 0, "label:text");
    sos_pop(1);
    // A canvas and a blit
    G("ui.canvas");
    sos_push_int(200);
    sos_push_int(8);
    sos_push_int(16);
    sos_push_int(8);
    check(sos_call(4, 1) == 0 && sos_type(-1) == SOS_OBJ, "ui.canvas");
    for (int i = 0; i < 16 * 8; i++) {
        s_px[i] = (unsigned short)(i * 512);
    }
    check(sos_blit(1, s_px, 0, 0, 16, 8) == 0, "sos_blit");
    check(sos_blit(1, s_px, -10, -4, 16, 8) == 0, "sos_blit clipped");
    // c:get(1, 0): the pixel blitted there
    sos_get(1, "get", 3);
    sos_push_value(1);
    sos_push_int(1);
    sos_push_int(0);
    sos_call(3, 1);
    check(sos_type(-1) == SOS_INT || sos_type(-1) == SOS_NIL, "canvas:get after the blit");
    say_int("canvas pixel ", sos_to_int(-1));
    sos_pop(1);
    push_s("not a canvas");
    check(sos_blit(-1, s_px, 0, 0, 16, 8) == -1, "sos_blit on a string is -1");
    sos_pop(2);
}

SOS_EXPORT(on_start) void on_start(void)
{
    check(s_initialized == 42, "_initialize ran first");
    check(sos_host_abi() == SOS_ABI, "sos_host_abi");
    check(sos_keys() == 0, "sos_keys: nothing held");
    say_int("millis at start ", (int)sos_millis());
    stack_basics();
    tables();
    api_calls();
    refs();
    storage();
    wasi();
    ui_bits();
    callbacks();
    // on_mesh's and on_meet's frames, through the globals the host calls
    G("on_mesh");
    push_s("ABC123");
    sos_new_table();
    sos_push_int(9);
    set_field(-2, "v");
    check(sos_call(2, 0) == 0, "on_mesh called as the host does");
    G("on_meet");
    push_s("XYZ789");
    sos_new_table();
    check(sos_call(2, 1) == 0 && sos_type(-1) == SOS_TABLE, "on_meet returns the table to sign");
    sos_get(-1, "ok", 2);
    check(sos_to_bool(-1), "on_meet's table");
    sos_pop(2);
    say("on_start done");
}

SOS_EXPORT(on_mesh) void on_mesh(void)
{
    char from[8];
    sos_to_str(1, from, sizeof(from));
    sos_get(2, "v", 1);
    check(sos_top() == 3 && seq(from, "ABC123") && sos_to_int(3) == 9, "on_mesh frame: 1 sender, 2 message");
}

SOS_EXPORT(on_meet) int on_meet(void)
{
    check(sos_top() == 2 && sos_type(1) == SOS_STR, "on_meet frame");
    sos_new_table();
    sos_push_bool(1);
    set_field(-2, "ok");
    return 1;
}

SOS_EXPORT(on_tick) void on_tick(int ms)
{
    if (s_ticks++ == 0) {
        check(ms > 0 && ms < 1000 && sos_top() == 0, "on_tick(ms), empty frame");
    }
    if (s_ticks == 20 && s_got_timer) {
        say_int("ticks ", s_ticks);
    }
}

SOS_EXPORT(on_key) int on_key(int key, int repeat)
{
    if (!s_got_key) {
        s_got_key = 1;
        say_int("key ", key);
        check(sos_top() == 0 && repeat == 0, "on_key(key, repeat), empty frame");
    }
    if (key == SOS_KEY_RIGHT) {
        say(s_got_timer && s_got_select && s_got_http ? "done" : "not done");
        check(s_got_timer, "the timer called back");
        check(s_got_select, "the list called back");
        check(s_got_http, "http called back");
        sos_push_ref(s_label_ref);
        check(sos_type(-1) == SOS_OBJ, "a kept label");
        sos_pop(1);
        return 1;
    }
    return 0;
}

SOS_EXPORT(on_exit) void on_exit(void)
{
    say("on_exit");
}
