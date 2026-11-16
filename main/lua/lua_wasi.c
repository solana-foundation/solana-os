// The few WASI calls (module "wasi_snapshot_preview1") wasi-libc makes in a
// C app (docs/C_API.md "libc"). No files, no environment, no arguments: a C
// app reaches the card through the Lua API's fs, like a Lua app.
//   fd_write          fd 1 and 2 to the console, a line at a time, exactly as
//                     print: "[<id>] ", control characters as '?'; at most
//                     LW_WRITE_IOVS buffers and LW_WRITE_MAX bytes a call (a
//                     short count: libc writes the rest with another call)
//   clock_time_get    realtime: the server's clock (EINVAL until the badge
//                     has it, as sys.time() is nil); the others: uptime
//   random_get        the hardware RNG, up to LW_RANDOM_MAX bytes a call
//   proc_exit         ends the app like sys.exit()
//   fd_close, fd_seek, fd_fdstat_get, environ_*, args_*: what wasi-libc asks
//                     at start-up and for stdio; stdout is a character device
// A module importing any other WASI call doesn't load (lua_wasm.c).
//
// The calling convention is not ours to choose. wamrc knows WAMR's own
// libc-wasi signatures and compiles a native app's WASI imports as direct
// calls with them: pointer parameters ('*', '*~') checked and turned into
// native addresses by the app's code, the C ABI of libc_wasi_wrapper.c, no
// argument array and no attachment. So these are ordinary natives with
// exactly those signatures (any change breaks every app.aot the store has
// built: it would need a new AOT_TARGET). A '*' is checked for one byte
// only: each function checks the whole size it touches (lw_native). They go
// round lua_wasm.c's import wrapper, so each starts and ends with
// lw_import_in / lw_import_out (wasi_in charges the host step every import
// costs). app.wasm gets the same functions behind raw natives (below), which
// work in the emulator too. Output and random bytes cost host steps by
// length (lw_charge, lua_wasm.h), so they are bounded per callback.

#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "core/os_cloud.h"
#include "lua_wasm.h"

#define WASI_ESUCCESS   0
#define WASI_EBADF      8
#define WASI_EINVAL     28
#define WASI_ESPIPE     70

#define WASI_FILETYPE_CHARACTER_DEVICE  2
#define WASI_RIGHT_FD_WRITE             (1u << 6)

// lw_import_in, plus the step every import costs (lua_wasm.h): NULL when the
// app must stop.
static lw_t *wasi_in(void)
{
    lw_t *w = lw_import_in();
    if (w && !lw_charge(w, LW_IMPORT_STEPS)) {
        lw_import_out(w);
        return NULL;
    }
    return w;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);                       // the app's pointers needn't be aligned
}

static void put_u64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

// fd_write(fd, iovs, iovs_len, nwritten) -> errno
static int32_t fd_write(lw_t *w, uint32_t fd, uint8_t *iovs, uint32_t n, uint8_t *out)
{
    if (fd != 1 && fd != 2) {
        return WASI_EBADF;
    }
    if (n > 1024) {
        return WASI_EINVAL;
    }
    if ((n && !lw_native(w, "fd_write", iovs, n * 8)) || !lw_native(w, "fd_write", out, 4)) {
        return WASI_EINVAL;
    }
    // A short write past LW_WRITE_IOVS buffers or LW_WRITE_MAX bytes: what
    // POSIX allows and wasi-libc's stdio carries on from.
    uint32_t total = 0;
    for (uint32_t i = 0; i < n && i < LW_WRITE_IOVS && total < LW_WRITE_MAX; i++) {
        uint32_t buf, len;
        memcpy(&buf, iovs + i * 8, 4);
        memcpy(&len, iovs + i * 8 + 4, 4);
        const char *p = len ? lw_mem(w, "fd_write", buf, len) : "";
        if (!p) {
            return WASI_EINVAL;
        }
        if (len > LW_WRITE_MAX - total) {
            len = LW_WRITE_MAX - total;
        }
        if (!lw_charge(w, len * LW_OUT_STEPS)) {
            return WASI_EINVAL;             // the app stops (lw_charge)
        }
        lw_out(w, p, len);
        total += len;
    }
    put_u32(out, total);
    return WASI_ESUCCESS;
}

static int32_t w_fd_write(wasm_exec_env_t env, uint32_t fd, uint8_t *iovs, uint32_t n, uint8_t *out)
{
    (void)env;
    lw_t *w = wasi_in();
    int32_t r = w ? fd_write(w, fd, iovs, n, out) : WASI_EBADF;
    lw_import_out(w);
    return r;
}

// clock_time_get(id, precision, time) -> errno; time in ns
static int32_t clock_time_get(lw_t *w, uint32_t id, uint8_t *out)
{
    if (!lw_native(w, "clock_time_get", out, 8) || id > 3) {
        return WASI_EINVAL;
    }
    uint64_t ns;
    if (id == 0) {                          // realtime
        int64_t ms = os_cloud_time_ms();
        if (!ms) {
            return WASI_EINVAL;
        }
        ns = (uint64_t)ms * 1000000u;
    } else {                                // monotonic, process and thread time
        ns = (uint64_t)esp_timer_get_time() * 1000u;
    }
    put_u64(out, ns);
    return WASI_ESUCCESS;
}

static int32_t w_clock_time_get(wasm_exec_env_t env, uint32_t id, uint64_t precision, uint8_t *out)
{
    (void)env;
    (void)precision;
    lw_t *w = wasi_in();
    int32_t r = w ? clock_time_get(w, id, out) : WASI_EINVAL;
    lw_import_out(w);
    return r;
}

// random_get(buf, len) -> errno ('*~': the whole buffer is checked already).
// WASI has no short count here, so more than LW_RANDOM_MAX is refused
// (wasi-libc's getentropy asks for 256 at most).
static int32_t w_random_get(wasm_exec_env_t env, uint8_t *buf, uint32_t len)
{
    (void)env;
    lw_t *w = wasi_in();
    int32_t r = WASI_EINVAL;
    if (w && len <= LW_RANDOM_MAX && lw_charge(w, len * LW_RANDOM_STEPS)) {
        esp_fill_random(buf, len);
        r = WASI_ESUCCESS;
    }
    lw_import_out(w);
    return r;
}

// proc_exit(code): doesn't return
static void w_proc_exit(wasm_exec_env_t env, uint32_t code)
{
    (void)env;
    (void)code;
    lw_t *w = wasi_in();
    if (w) {
        lw_exit(w);
    }
    lw_import_out(w);
}

static int32_t w_fd_close(wasm_exec_env_t env, uint32_t fd)
{
    (void)env;
    lw_t *w = wasi_in();
    lw_import_out(w);
    return w && fd <= 2 ? WASI_ESUCCESS : WASI_EBADF;
}

// fd_seek(fd, offset, whence, newoffset) -> errno: the console doesn't seek
static int32_t w_fd_seek(wasm_exec_env_t env, uint32_t fd, int64_t offset, uint32_t whence, uint8_t *out)
{
    (void)env;
    (void)offset;
    (void)whence;
    (void)out;
    lw_t *w = wasi_in();
    lw_import_out(w);
    return w && fd <= 2 ? WASI_ESPIPE : WASI_EBADF;
}

// fd_fdstat_get(fd, stat) -> errno: 1 and 2 are a character device, write
// only, so wasi-libc line-buffers stdout as on a terminal.
static int32_t fd_fdstat_get(lw_t *w, uint32_t fd, uint8_t *p)
{
    if (fd != 1 && fd != 2) {
        return WASI_EBADF;
    }
    if (!lw_native(w, "fd_fdstat_get", p, 24)) {
        return WASI_EINVAL;
    }
    memset(p, 0, 24);
    p[0] = WASI_FILETYPE_CHARACTER_DEVICE;
    put_u64(p + 8, WASI_RIGHT_FD_WRITE);
    return WASI_ESUCCESS;
}

static int32_t w_fd_fdstat_get(wasm_exec_env_t env, uint32_t fd, uint8_t *stat)
{
    (void)env;
    lw_t *w = wasi_in();
    int32_t r = w ? fd_fdstat_get(w, fd, stat) : WASI_EBADF;
    lw_import_out(w);
    return r;
}

// environ_sizes_get / args_sizes_get(count, bytes) -> errno: none
static int32_t w_sizes_get(wasm_exec_env_t env, uint8_t *count, uint8_t *bytes)
{
    (void)env;
    lw_t *w = wasi_in();
    bool ok = w && lw_native(w, "sizes_get", count, 4) && lw_native(w, "sizes_get", bytes, 4);
    if (ok) {
        put_u32(count, 0);
        put_u32(bytes, 0);
    }
    lw_import_out(w);
    return ok ? WASI_ESUCCESS : WASI_EINVAL;
}

// environ_get / args_get(pointers, buffer) -> errno: nothing to write
static int32_t w_none_get(wasm_exec_env_t env, uint8_t *ptrs, uint8_t *buf)
{
    (void)env;
    (void)ptrs;
    (void)buf;
    lw_t *w = wasi_in();
    lw_import_out(w);
    return w ? WASI_ESUCCESS : WASI_EINVAL;
}

// libc_wasi_wrapper.c's signatures, character for character (see the top).
static const NativeSymbol s_wasi[] = {
    {"fd_write", w_fd_write, "(i*i*)i", NULL},
    {"clock_time_get", w_clock_time_get, "(iI*)i", NULL},
    {"random_get", w_random_get, "(*~)i", NULL},
    {"proc_exit", w_proc_exit, "(i)", NULL},
    {"fd_close", w_fd_close, "(i)i", NULL},
    {"fd_seek", w_fd_seek, "(iIi*)i", NULL},
    {"fd_fdstat_get", w_fd_fdstat_get, "(i*)i", NULL},
    {"environ_sizes_get", w_sizes_get, "(**)i", NULL},
    {"environ_get", w_none_get, "(**)i", NULL},
    {"args_sizes_get", w_sizes_get, "(**)i", NULL},
    {"args_get", w_none_get, "(**)i", NULL},
};

// ---------------------------------------------------------------------------
// The same calls as raw natives, for app.wasm: WAMR's interpreter calls a
// plain native through a generic trampoline that, in the emulator (itself
// WebAssembly), traps on any C signature it doesn't expect, while a raw
// native always has one. Each turns the app's offsets into native pointers
// as WAMR does for '*' (checked for one byte; the function checks the rest).

#define ARG_U32(i)  (*(uint32_t *)&a[i])
#define ARG_U64(i)  (*(uint64_t *)&a[i])
#define RET_I32(v)  (*(int32_t *)a = (int32_t)(v))

static uint8_t *ptr(uint32_t off, uint32_t len, const char *fn)
{
    lw_t *w = lw_cur;
    return w && w->inst ? lw_mem(w, fn, off, len) : NULL;
}

static void r_fd_write(wasm_exec_env_t env, uint64_t *a)
{
    uint8_t *iovs = ptr(ARG_U32(1), 1, "fd_write"), *out = ptr(ARG_U32(3), 1, "fd_write");
    RET_I32(w_fd_write(env, ARG_U32(0), iovs, ARG_U32(2), out));
}

static void r_clock_time_get(wasm_exec_env_t env, uint64_t *a)
{
    uint8_t *out = ptr(ARG_U32(2), 1, "clock_time_get");
    RET_I32(w_clock_time_get(env, ARG_U32(0), ARG_U64(1), out));
}

static void r_random_get(wasm_exec_env_t env, uint64_t *a)
{
    uint32_t len = ARG_U32(1);
    uint8_t *buf = ptr(ARG_U32(0), len, "random_get");
    RET_I32(buf ? w_random_get(env, buf, len) : WASI_EINVAL);
}

static void r_proc_exit(wasm_exec_env_t env, uint64_t *a)
{
    w_proc_exit(env, ARG_U32(0));
}

static void r_fd_close(wasm_exec_env_t env, uint64_t *a)
{
    RET_I32(w_fd_close(env, ARG_U32(0)));
}

static void r_fd_seek(wasm_exec_env_t env, uint64_t *a)
{
    uint8_t *out = ptr(ARG_U32(3), 1, "fd_seek");
    RET_I32(w_fd_seek(env, ARG_U32(0), (int64_t)ARG_U64(1), ARG_U32(2), out));
}

static void r_fd_fdstat_get(wasm_exec_env_t env, uint64_t *a)
{
    uint8_t *stat = ptr(ARG_U32(1), 1, "fd_fdstat_get");
    RET_I32(w_fd_fdstat_get(env, ARG_U32(0), stat));
}

static void r_sizes_get(wasm_exec_env_t env, uint64_t *a)
{
    uint8_t *count = ptr(ARG_U32(0), 1, "sizes_get"), *bytes = ptr(ARG_U32(1), 1, "sizes_get");
    RET_I32(w_sizes_get(env, count, bytes));
}

static void r_none_get(wasm_exec_env_t env, uint64_t *a)
{
    RET_I32(w_none_get(env, NULL, NULL));
}

static const NativeSymbol s_wasi_raw[] = {
    {"fd_write", r_fd_write, "(iiii)i", NULL},
    {"clock_time_get", r_clock_time_get, "(iIi)i", NULL},
    {"random_get", r_random_get, "(ii)i", NULL},
    {"proc_exit", r_proc_exit, "(i)", NULL},
    {"fd_close", r_fd_close, "(i)i", NULL},
    {"fd_seek", r_fd_seek, "(iIii)i", NULL},
    {"fd_fdstat_get", r_fd_fdstat_get, "(ii)i", NULL},
    {"environ_sizes_get", r_sizes_get, "(ii)i", NULL},
    {"environ_get", r_none_get, "(ii)i", NULL},
    {"args_sizes_get", r_sizes_get, "(ii)i", NULL},
    {"args_get", r_none_get, "(ii)i", NULL},
};

const NativeSymbol *lw_wasi_symbols(bool native, uint32_t *n)
{
    _Static_assert(sizeof(s_wasi) == sizeof(s_wasi_raw), "the two tables name the same calls");
    *n = sizeof(s_wasi) / sizeof(s_wasi[0]);
    return native ? s_wasi : s_wasi_raw;
}
