// C apps (2.3.0, docs/C_API.md): a store app compiled to WebAssembly, run by
// WAMR (lua/wamr/, WAMR 2.4.5) inside the Lua host.
//
// The Lua host stays the host. A C app gets the usual Lua state with every
// API module, but its "main chunk" is the module: lw_boot() loads app.wasm
// (or the store's native app.aot, lua_aot.h) and pushes lw_main, which
// instantiates it and calls on_start. The globals the host calls by name
// (on_key, on_tick, on_exit, on_mesh, on_meet) are C closures that call the
// module's exports. Everything else (events, timers, sockets, Bluetooth,
// animations, permissions, the error screen) runs unchanged.
//
// The module's imports (module "badge", sdk/c/include/badge_abi.h) are a
// small, checked Lua C API over that state: a frame is the Lua C frame of
// the closure that called into the app, 1..64 values. Every index, pointer
// and length is checked; an app's mistake stops it with the error screen.
//
// Errors never longjmp across WAMR. An import that can raise a Lua error
// (anything that allocates, metamethods, the API itself) runs protected; a
// failure is noted (lw_t.fatal) and turned into a WAMR exception, the
// module unwinds to the closure that called in, and only there, with no
// WAMR frame left above it, is the Lua error raised. sos_call is lua_pcall:
// what an API function refuses comes back to the app, but out of memory,
// the time budget and a trap in a nested callback always stop it.
//
// One jump does cross WAMR, outward only: stopping stuck native code
// (lua_aot.h). Every call into the module (enter(): the top-level callbacks
// and each nested callback of an sos_push_fn closure) has its own setjmp,
// and the watchdog aims the app's task at the innermost one, only while the
// task's program counter is in the module's code. So the jump skips the
// module's frames and WAMR's call path (wasm_runtime_call_wasm ->
// aot_call_function -> wasm_runtime_invoke_native -> invokeNative), never a
// host function's frame or Lua's: with this build's settings (no AOT stack
// frames, no quick entries, no reference types, no hardware bound checks,
// no threads) that path takes no lock (WAMR's are empty anyway,
// lua_wasm_port.c), allocates nothing for our signatures, and leaves only
// exec-env fields every call sets again. From the landing the kill travels
// the normal way: the closure raises, the sos_call's lua_pcall returns, the
// bridge sets the WAMR exception so the outer native code unwinds by itself,
// and the host ends the app with the error screen ("App took too long");
// the instance is always destroyed after (lw_close). The shadow stack
// pointer (__stack_pointer) is left wherever the jump found it: one more
// reason the instance never runs again. The LVGL lock the Lua task holds
// around the callback is taken and released in lua_host.c, outside every
// enter(), so the jump can't skip it.
//
// Limits. WAMR's allocations (module, instance, linear memory, its stack) go
// to the app's counted heap (lua_wasm_port.c), so one cap covers the Lua
// state and the module, and sys.memory() reports both. CPU, interpreted:
// WAMR's instruction metering, LUA_HOST_WASM_STEPS_MAX per callback, shared
// by the nested calls of one callback (a "Solana OS" patch in
// wasm_interp_classic.c hands the count across natives), with bulk memory
// operations charged by length (another one). Native: time,
// LW_AOT_CALLBACK_MS of it per callback (lw_clock_t), counted while a call
// into the module is open and no import is, bulk memory helpers included
// (lua_aot.c), shared by nested calls. The imports' own work counts against
// the host's step budget, as a Lua app's API calls do: LW_IMPORT_STEPS each,
// and by length where an import copies, writes or fills (lw_charge,
// lua_wasm.h). Over all of it, the ceiling: LW_CALLBACK_MAX_MS of wall-clock
// time per callback (lua_aot.h), checked by every import and, on a badge, by
// the watchdog.
//
// Stack. Both forms run on the Lua task (12 KB). The interpreter doesn't
// recurse in C (the classic interpreter keeps its frames on its own stack,
// LW_WASM_STACK, in the app's heap); native code keeps its frames on the
// task's stack and checks them against the boundary set below. Each nesting
// through sos_call -> API -> callback costs a few C frames, and WAMR refuses
// to go on within LW_STACK_MARGIN of the task's low mark ("native stack
// overflow" on the error screen).

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os_data.h"
#include "core/os.h"
#include "core/os_keys.h"
#include "wasm.h"                       // WASMModule: whether app.wasm has a start function
#include "lua_aot.h"
#include "lua_wasm.h"
#include "lua_wasm_port.h"

static const char *TAG = "lua_wasm";

// Must match sdk/c/include/badge_abi.h (not included: it declares wasm
// imports, which a native compiler doesn't know).
#define LW_ABI          1
enum { SOS_NONE = -1, SOS_NIL, SOS_BOOL, SOS_INT, SOS_NUM, SOS_STR, SOS_TABLE, SOS_FN, SOS_OBJ };
#define SOS_MULTRET     (-1)

#define LW_REFS_MAX     1024
#define LW_SLACK        8           // stack slots past a full frame: a protected call's function and arguments
#define LW_WASM_STACK   (32 * 1024) // WAMR's own stack per app: wasm frames and operands (app heap)
#define LW_STACK_MARGIN 1024        // past lh_stack_low(): the interpreter's frame and an import's
#define LW_GLOBAL_MAX   96          // sos_global's dotted name
#define LW_LOG_MAX      1024        // bytes per sos_log

lw_t *lw_cur;

// The runtime's holder (2.3.0): WAMR's globals are one set for the badge and
// its locks are empty (lua_wasm_port.c), so a C app and a C service never
// run it at once (lua_svc.c pauses C services for a C app).
static portMUX_TYPE s_own_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_owned, s_owned_svc;

bool lw_busy(void)
{
    return s_owned;
}

bool lw_busy_svc(void)
{
    return s_owned && s_owned_svc;
}

static bool claim(bool svc)
{
    portENTER_CRITICAL(&s_own_mux);
    bool ok = !s_owned;
    if (ok) {
        s_owned = true;
        s_owned_svc = svc;
    }
    portEXIT_CRITICAL(&s_own_mux);
    return ok;
}

static void release(void)
{
    portENTER_CRITICAL(&s_own_mux);
    s_owned = s_owned_svc = false;
    portEXIT_CRITICAL(&s_own_mux);
}

// Per callback: an app's limits, or a service's (lua_aot.h).
static uint32_t wasm_steps_max(const lh_t *s)
{
    return s->svc ? LW_SVC_WASM_STEPS : LUA_HOST_WASM_STEPS_MAX;
}

// on_key's numbers: s_key_names' order in lua_host.c (SOS_KEY_* in badge_abi.h).
static const char *const s_keys[] = {"up", "down", "left", "right", "select", "back"};

// ---------------------------------------------------------------------------
// Stopping

void lw_trap(lw_t *w, uint8_t kind, const char *fmt, ...)
{
    if (!w->fatal) {
        w->fatal = true;
        w->kind = kind;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(w->msg, sizeof(w->msg), fmt, ap);
        va_end(ap);
    }
    if (w->inst) {
        wasm_runtime_set_exception(w->inst, "sos: stop");
    }
}

void lw_exit(lw_t *w)
{
    w->exiting = true;
    w->s->exit_req = true;
    if (w->inst) {
        wasm_runtime_set_exception(w->inst, "sos: exit");
    }
}

// The callback ran past its time: native code past LW_AOT_CALLBACK_MS (the
// watchdog set kill), or anything past the ceiling (noticed here or by the
// watchdog). Then the app stops.
static bool over_time(lw_t *w)
{
    lw_clock_t *c = &w->clk;
    if (!c->kill && c->open && (uint32_t)esp_timer_get_time() - c->cb_us > LW_CALLBACK_MAX_MS * 1000u) {
        c->ceiling = true;
        c->kill = true;
    }
    if (!c->kill) {
        return false;
    }
    if (!w->fatal) {
        if (c->ceiling) {
            lw_trap(w, LW_BUDGET, "The app ran over %d s in one callback", LW_CALLBACK_MAX_MS / 1000);
        } else {
            lw_trap(w, LW_BUDGET, "Native code ran over %d ms in one callback", (int)c->limit_ms);
        }
    }
    return true;
}

bool lw_charge(lw_t *w, uint32_t n)
{
    lh_t *s = w->s;
    s->steps += n;
    if (s->steps <= s->steps_max) {
        return true;
    }
    if (!w->fatal) {
        s->killed = true;
        lw_trap(w, LW_BUDGET, LH_HOST_STEPS_MSG, (int)s->steps_max);
    }
    return false;
}

// The app must stop now (or has exited): then an import does nothing more.
static bool stopping(lw_t *w)
{
    if (over_time(w) || w->fatal || w->exiting) {
        wasm_runtime_set_exception(w->inst, w->exiting ? "sos: exit" : "sos: stop");
        return true;
    }
    return false;
}

void *lw_mem(lw_t *w, const char *fn, uint32_t addr, uint32_t len)
{
    if (!wasm_runtime_validate_app_addr(w->inst, addr, len)) {
        lw_trap(w, LW_ERR, "%s: the pointer 0x%x (%u bytes) is outside the app's memory", fn, (unsigned)addr,
                (unsigned)len);
        return NULL;
    }
    return wasm_runtime_addr_app_to_native(w->inst, addr);
}

// A trap WAMR raised (no import noted a reason): words for the error screen.
static void wamr_reason(lw_t *w)
{
    const char *e = wasm_runtime_get_exception(w->inst);
    e = e ? e : "unknown error";
    if (!strncmp(e, "Exception: ", 11)) {
        e += 11;
    }
    static const struct { const char *wamr, *say; uint8_t kind; } map[] = {
        {"instruction limit exceeded", "App took too long", LW_BUDGET},
        {"out of bounds memory access", "The app read or wrote outside its memory", LW_ERR},
        {"unreachable", "The app crashed (unreachable code: abort(), __builtin_trap())", LW_ERR},
        {"integer divide by zero", "The app divided by zero", LW_ERR},
        {"integer overflow", "Integer overflow in a division (INT_MIN / -1)", LW_ERR},
        {"invalid conversion to integer", "A float too big for an integer", LW_ERR},
        {"wasm operand stack overflow", "The app ran out of stack (recursion too deep)", LW_ERR},
        {"native stack overflow", "The app ran out of stack (recursion or callbacks nested too deeply)", LW_ERR},
        {"indirect call type mismatch", "The app called a function pointer of the wrong type", LW_ERR},
        {"undefined element", "The app called a bad function pointer", LW_ERR},
        {"uninitialized element", "The app called a bad function pointer", LW_ERR},
        {"allocate memory failed", "not enough memory", LW_MEM},
        {"out of memory", "not enough memory", LW_MEM},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strstr(e, map[i].wamr)) {
            if (map[i].kind == LW_ERR) {
                lw_trap(w, LW_ERR, "%s (%s)", map[i].say, map[i].wamr);
            } else if (map[i].kind == LW_BUDGET) {
                lw_trap(w, LW_BUDGET, "Over %d wasm instructions in one callback", (int)wasm_steps_max(w->s));
            } else {
                lw_trap(w, map[i].kind, "%s", map[i].say);
            }
            return;
        }
    }
    lw_trap(w, LW_ERR, "The app crashed: %s", e);
}

// In a closure, with no WAMR frame above: raise the reason the app stops.
static int raise(lua_State *L, lw_t *w)
{
    lh_t *s = w->s;
    if (w->kind == LW_MEM) {
        s->mem_fatal = true;
    } else if (w->kind == LW_BUDGET) {
        s->killed = true;
        if (!s->kill_msg[0]) {
            strlcpy(s->kill_msg, w->msg, sizeof(s->kill_msg));
        }
    }
    lua_pushstring(L, w->msg);
    return lua_error(L);
}

// After a protected call failed with rc and its error on top: note why, pop
// it, and make the interpreter unwind.
static void lua_failed(lw_t *w, lua_State *L, int rc)
{
    lh_t *s = w->s;
    const char *m = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(error is not a string)";
    uint8_t kind = rc == LUA_ERRMEM || (s->mem_hit && !strcmp(m, "not enough memory")) ? LW_MEM
                   : s->killed ? LW_BUDGET : LW_ERR;
    lw_trap(w, kind, "%s", kind == LW_BUDGET && s->kill_msg[0] ? s->kill_msg : m);
    lua_pop(L, 1);
}

// Runs f with the nargs values above it (pushed by the caller) protected,
// leaving nres results. false: the app stops.
static bool prot(lw_t *w, lua_State *L, int nargs, int nres)
{
    int rc = lua_pcall(L, nargs, nres, 0);
    if (rc != LUA_OK) {
        lua_failed(w, L, rc);
        return false;
    }
    if (w->fatal || w->exiting || w->s->killed) {
        stopping(w);
        if (!w->fatal && !w->exiting) {
            lw_trap(w, LW_BUDGET, "%s", w->s->kill_msg);
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frames

#define ARG_I32(i)  (*(int32_t *)&a[i])
#define ARG_U32(i)  (*(uint32_t *)&a[i])
#define ARG_F32(i)  (*(float *)&a[i])
#define RET_I32(v)  (*(int32_t *)a = (int32_t)(v))
#define RET_F32(v)  (*(float *)a = (v))

// Every import starts here: the running app and its frame's state, or NULL
// when the app is stopping (the import then does nothing).
static lw_t *begin(lua_State **L)
{
    lw_t *w = lw_cur;
    if (!w || !w->L || stopping(w)) {
        if (w && !w->L && !w->fatal) {
            lw_trap(w, LW_ERR, "the badge API is called outside a callback");
        }
        return NULL;
    }
    if (!lw_charge(w, LW_IMPORT_STEPS)) {
        return NULL;
    }
    *L = w->L;
    return w;
}

// idx as an absolute index of the frame, or 0 (and the app stops) when it is
// outside it.
static int frame_idx(lw_t *w, lua_State *L, const char *fn, int32_t idx)
{
    int top = lua_gettop(L);
    int i = idx > 0 ? idx : idx < 0 && -(int64_t)idx <= top ? top + idx + 1 : 0;
    if (i < 1 || i > top) {
        lw_trap(w, LW_ERR, "%s: index %d is outside the frame (%d values)", fn, (int)idx, top);
        return 0;
    }
    return i;
}

// Room for n more values in the frame.
static bool room(lw_t *w, lua_State *L, const char *fn, int n)
{
    if (lua_gettop(L) + n > LW_FRAME_MAX) {
        lw_trap(w, LW_ERR, "%s: more than %d values in a frame", fn, LW_FRAME_MAX);
        return false;
    }
    return true;
}

static int sos_type_of(lua_State *L, int i)
{
    switch (lua_type(L, i)) {
        case LUA_TNIL: return SOS_NIL;
        case LUA_TBOOLEAN: return SOS_BOOL;
        case LUA_TNUMBER: return lua_isinteger(L, i) ? SOS_INT : SOS_NUM;
        case LUA_TSTRING: return SOS_STR;
        case LUA_TTABLE: return SOS_TABLE;
        case LUA_TFUNCTION: return SOS_FN;
        case LUA_TNONE: return SOS_NONE;
        default: return SOS_OBJ;            // userdata (an API object), a coroutine
    }
}

// ---------------------------------------------------------------------------
// Protected operations: arguments 1.. as pushed by the import

static int op_pushstr(lua_State *L)
{
    lua_pushlstring(L, lua_touserdata(L, 1), (size_t)lua_tointeger(L, 2));
    return 1;
}

static int op_newtable(lua_State *L)
{
    lua_createtable(L, 0, 0);
    return 1;
}

static int fn_call(lua_State *L);

static int op_pushfn(lua_State *L)
{
    lua_pushcclosure(L, fn_call, 2);        // upvalues: the table index and ud
    return 1;
}

static int op_get(lua_State *L)             // 1 the value, 2 key pointer, 3 key length
{
    lua_pushlstring(L, lua_touserdata(L, 2), (size_t)lua_tointeger(L, 3));
    lua_gettable(L, 1);
    return 1;
}

static int op_set(lua_State *L)             // 1 table, 2 key pointer, 3 key length, 4 value
{
    lua_pushlstring(L, lua_touserdata(L, 2), (size_t)lua_tointeger(L, 3));
    lua_pushvalue(L, 4);
    lua_settable(L, 1);
    return 0;
}

static int op_geti(lua_State *L)            // 1 table, 2 i
{
    lua_geti(L, 1, lua_tointeger(L, 2));
    return 1;
}

static int op_seti(lua_State *L)            // 1 table, 2 i, 3 value
{
    lua_pushvalue(L, 3);
    lua_seti(L, 1, lua_tointeger(L, 2));
    return 0;
}

static int op_next(lua_State *L)            // 1 table, 2 key
{
    return lua_next(L, 1) ? 2 : 0;
}

static int op_global(lua_State *L)          // 1 name pointer, 2 its length: "ui.label"
{
    const char *p = lua_touserdata(L, 1), *end = p + lua_tointeger(L, 2);
    lua_pushglobaltable(L);
    while (p < end) {
        const char *dot = memchr(p, '.', (size_t)(end - p));
        size_t n = dot ? (size_t)(dot - p) : (size_t)(end - p);
        int t = lua_type(L, -1);
        if (t != LUA_TTABLE && t != LUA_TUSERDATA) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, p, n);
        lua_gettable(L, -2);
        lua_remove(L, -2);
        p += n + (dot ? 1 : 0);
    }
    return 1;
}

static int op_rawseti(lua_State *L)         // 1 table, 2 i, 3 value
{
    lua_pushvalue(L, 3);
    lua_rawseti(L, 1, lua_tointeger(L, 2));
    return 0;
}

static int op_errstr(lua_State *L)
{
    lua_pushliteral(L, "(error is not a string)");
    return 1;
}

// ---------------------------------------------------------------------------
// The imports (module "badge"): raw natives, args in 64-bit slots

static void n_top(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    RET_I32(w ? lua_gettop(L) : 0);
}

static void n_pop(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t n = ARG_I32(0);
    if (!w) {
        return;
    }
    if (n < 0 || n > lua_gettop(L)) {
        lw_trap(w, LW_ERR, "sos_pop: %d values, but the frame has %d", (int)n, lua_gettop(L));
        return;
    }
    lua_pop(L, n);
}

static void n_push_value(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_push_value", ARG_I32(0)) : 0;
    if (i && room(w, L, "sos_push_value", 1)) {
        lua_pushvalue(L, i);
    }
}

static void n_remove(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_remove", ARG_I32(0)) : 0;
    if (i) {
        lua_remove(L, i);
    }
}

static void n_push_nil(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    (void)a;
    lua_State *L;
    lw_t *w = begin(&L);
    if (w && room(w, L, "sos_push_nil", 1)) {
        lua_pushnil(L);
    }
}

static void n_push_bool(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    if (w && room(w, L, "sos_push_bool", 1)) {
        lua_pushboolean(L, ARG_I32(0) != 0);
    }
}

static void n_push_int(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    if (w && room(w, L, "sos_push_int", 1)) {
        lua_pushinteger(L, (lua_Integer)ARG_I32(0));
    }
}

static void n_push_num(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    if (w && room(w, L, "sos_push_num", 1)) {
        lua_pushnumber(L, (lua_Number)ARG_F32(0));
    }
}

static void n_push_str(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t len = ARG_U32(1);
    if (!w || !room(w, L, "sos_push_str", 1)) {
        return;
    }
    const char *p = len ? lw_mem(w, "sos_push_str", ARG_U32(0), len) : "";
    if (p && lw_charge(w, len / LW_COPY_BYTES)) {
        lua_pushcfunction(L, op_pushstr);
        lua_pushlightuserdata(L, (void *)p);
        lua_pushinteger(L, (lua_Integer)len);
        prot(w, L, 2, 1);
    }
}

static void n_new_table(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    (void)a;
    lua_State *L;
    lw_t *w = begin(&L);
    if (w && room(w, L, "sos_new_table", 1)) {
        lua_pushcfunction(L, op_newtable);
        prot(w, L, 0, 1);
    }
}

// The function at index fn of the app's function table, if it is an
// int32_t (*)(void *, int32_t): NULL (and why in `why`) otherwise.
static wasm_function_inst_t table_fn(lw_t *w, uint32_t fn, const char **why)
{
    wasm_table_inst_t t;
    if (!wasm_runtime_get_export_table_inst(w->inst, "__indirect_function_table", &t)) {
        *why = "the app exports no function table (link with --export-table)";
        return NULL;
    }
    wasm_function_inst_t f = fn && fn < t.cur_size ? wasm_table_get_func_inst(w->inst, &t, fn) : NULL;
    wasm_valkind_t p[2], r;
    if (!f || wasm_func_get_param_count(f, w->inst) != 2 || wasm_func_get_result_count(f, w->inst) != 1) {
        *why = f ? "the function isn't an sos_fn (int32_t f(void *, int32_t))" : "no function there";
        return NULL;
    }
    wasm_func_get_param_types(f, w->inst, p);
    wasm_func_get_result_types(f, w->inst, &r);
    if (p[0] != WASM_I32 || p[1] != WASM_I32 || r != WASM_I32) {
        *why = "the function isn't an sos_fn (int32_t f(void *, int32_t))";
        return NULL;
    }
    return f;
}

static void n_push_fn(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t fn = ARG_U32(0), ud = ARG_U32(1);
    const char *why;
    if (!w || !room(w, L, "sos_push_fn", 1)) {
        return;
    }
    if (!table_fn(w, fn, &why)) {
        lw_trap(w, LW_ERR, "sos_push_fn: %s (function %u)", why, (unsigned)fn);
        return;
    }
    lua_pushcfunction(L, op_pushfn);
    lua_pushinteger(L, (lua_Integer)fn);
    lua_pushinteger(L, (lua_Integer)ud);
    prot(w, L, 2, 1);
}

static void n_type(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t idx = ARG_I32(0);
    int top = w ? lua_gettop(L) : 0;
    int i = idx > 0 ? idx : idx < 0 && -(int64_t)idx <= top ? top + idx + 1 : 0;
    RET_I32(w && i >= 1 && i <= top ? sos_type_of(L, i) : SOS_NONE);
}

static void n_to_bool(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_to_bool", ARG_I32(0)) : 0;
    RET_I32(i ? lua_toboolean(L, i) : 0);
}

static int32_t to_i32(lua_State *L, int i)
{
    if (lua_type(L, i) != LUA_TNUMBER) {
        return 0;
    }
    if (lua_isinteger(L, i)) {
        return (int32_t)lua_tointeger(L, i);
    }
    lua_Number v = lua_tonumber(L, i);     // truncated, NaN as 0, clamped to the range
    return v != v ? 0 : v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t)v;
}

static void n_to_int(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_to_int", ARG_I32(0)) : 0;
    RET_I32(i ? to_i32(L, i) : 0);
}

static void n_to_num(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_to_num", ARG_I32(0)) : 0;
    RET_F32(i && lua_type(L, i) == LUA_TNUMBER ? (float)lua_tonumber(L, i) : 0.0f);
}

static void n_to_str(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t addr = ARG_U32(1), cap = ARG_U32(2);
    int i = w ? frame_idx(w, L, "sos_to_str", ARG_I32(0)) : 0;
    RET_I32(0);
    if (!i) {
        return;
    }
    char *buf = cap ? lw_mem(w, "sos_to_str", addr, cap) : NULL;
    if (cap && !buf) {
        return;
    }
    size_t len = 0;
    const char *str = lua_type(L, i) == LUA_TSTRING ? lua_tolstring(L, i, &len) : "";
    if (cap) {
        size_t n = len < cap - 1 ? len : cap - 1;
        if (!lw_charge(w, (uint32_t)(n / LW_COPY_BYTES))) {
            return;
        }
        memcpy(buf, str, n);
        buf[n] = 0;
    }
    RET_I32(len > UINT32_MAX ? UINT32_MAX : len);
}

static void n_len(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_len", ARG_I32(0)) : 0;
    int t = i ? lua_type(L, i) : LUA_TNIL;
    RET_I32(t == LUA_TSTRING || t == LUA_TTABLE ? lua_rawlen(L, i) : 0);
}

static void n_get(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t klen = ARG_U32(2);
    int i = w ? frame_idx(w, L, "sos_get", ARG_I32(0)) : 0;
    RET_I32(SOS_NIL);
    if (!i || !room(w, L, "sos_get", 1)) {
        return;
    }
    const char *k = klen ? lw_mem(w, "sos_get", ARG_U32(1), klen) : "";
    if (!k || !lw_charge(w, klen / LW_COPY_BYTES)) {
        return;
    }
    int t = lua_type(L, i);
    if (t != LUA_TTABLE && t != LUA_TUSERDATA) {
        lua_pushnil(L);                     // no fields
        return;
    }
    lua_pushcfunction(L, op_get);
    lua_pushvalue(L, i);
    lua_pushlightuserdata(L, (void *)k);
    lua_pushinteger(L, (lua_Integer)klen);
    if (prot(w, L, 3, 1)) {
        RET_I32(sos_type_of(L, -1));
    }
}

// The table at idx for a write (sos_set, sos_seti), with the value on top above it.
static int table_for_write(lw_t *w, lua_State *L, const char *fn, int32_t idx)
{
    int i = frame_idx(w, L, fn, idx);
    if (!i) {
        return 0;
    }
    if (lua_type(L, i) != LUA_TTABLE) {
        lw_trap(w, LW_ERR, "%s: the value at %d is not a table", fn, (int)idx);
        return 0;
    }
    if (lua_gettop(L) < 2) {
        lw_trap(w, LW_ERR, "%s: no value above the table to set", fn);
        return 0;
    }
    return i;
}

static void n_set(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t klen = ARG_U32(2);
    int i = w ? table_for_write(w, L, "sos_set", ARG_I32(0)) : 0;
    if (!i) {
        return;
    }
    const char *k = klen ? lw_mem(w, "sos_set", ARG_U32(1), klen) : "";
    if (!k || !lw_charge(w, klen / LW_COPY_BYTES)) {
        return;
    }
    lua_pushcfunction(L, op_set);
    lua_pushvalue(L, i);
    lua_pushlightuserdata(L, (void *)k);
    lua_pushinteger(L, (lua_Integer)klen);
    lua_pushvalue(L, -5);                   // the value: the frame's top before these four
    if (prot(w, L, 4, 0)) {
        lua_pop(L, 1);
    }
}

static void n_geti(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? frame_idx(w, L, "sos_geti", ARG_I32(0)) : 0;
    RET_I32(SOS_NIL);
    if (!i || !room(w, L, "sos_geti", 1)) {
        return;
    }
    if (lua_type(L, i) != LUA_TTABLE) {
        lua_pushnil(L);
        return;
    }
    lua_pushcfunction(L, op_geti);
    lua_pushvalue(L, i);
    lua_pushinteger(L, (lua_Integer)ARG_I32(1));
    if (prot(w, L, 2, 1)) {
        RET_I32(sos_type_of(L, -1));
    }
}

static void n_seti(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int i = w ? table_for_write(w, L, "sos_seti", ARG_I32(0)) : 0;
    if (!i) {
        return;
    }
    lua_pushcfunction(L, op_seti);
    lua_pushvalue(L, i);
    lua_pushinteger(L, (lua_Integer)ARG_I32(1));
    lua_pushvalue(L, -4);
    if (prot(w, L, 3, 0)) {
        lua_pop(L, 1);
    }
}

static void n_next(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t idx = ARG_I32(0);
    int i = w ? frame_idx(w, L, "sos_next", idx) : 0;
    RET_I32(0);
    if (!i || !room(w, L, "sos_next", 1)) {
        return;
    }
    int top = lua_gettop(L);
    if (lua_type(L, i) != LUA_TTABLE || top < 2 || i == top) {
        lw_trap(w, LW_ERR, "sos_next: needs a table at %d and the previous key on top", (int)idx);
        return;
    }
    lua_pushcfunction(L, op_next);
    lua_pushvalue(L, i);
    lua_pushvalue(L, top);
    if (prot(w, L, 2, LUA_MULTRET)) {
        bool more = lua_gettop(L) > top;
        lua_remove(L, top);                 // the previous key
        RET_I32(more);
    }
}

static void n_global(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t path = ARG_U32(0), len = ARG_U32(1);     // (args before the result: it overwrites a[0])
    RET_I32(SOS_NIL);
    if (!w || !room(w, L, "sos_global", 1)) {
        return;
    }
    const char *p = len && len <= LW_GLOBAL_MAX ? lw_mem(w, "sos_global", path, len) : NULL;
    if (!p) {
        if (!w->fatal) {
            lw_trap(w, LW_ERR, "sos_global: a name of 1 to %d characters", LW_GLOBAL_MAX);
        }
        return;
    }
    char name[LW_GLOBAL_MAX];
    memcpy(name, p, len);                   // a copy: nothing below may see memory move
    lua_pushcfunction(L, op_global);
    lua_pushlightuserdata(L, name);
    lua_pushinteger(L, (lua_Integer)len);
    if (prot(w, L, 2, 1)) {
        RET_I32(sos_type_of(L, -1));
    }
}

static void n_call(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t nargs = ARG_I32(0), nres = ARG_I32(1);
    RET_I32(0);
    if (!w) {
        return;
    }
    int top = lua_gettop(L);
    if (nargs < 0 || nargs >= top) {
        lw_trap(w, LW_ERR, "sos_call: %d arguments need %d values in the frame (it has %d)", (int)nargs,
                (int)nargs + 1, top);
        return;
    }
    // What stays below the function is 0..LW_FRAME_MAX; nres is the app's
    // number, so compare it with the room left, never add it first (with two
    // values below, INT32_MAX would wrap and pass, and lua_pcall checks no
    // count in this build).
    int below = top - nargs - 1;
    if (nres < SOS_MULTRET || (nres != SOS_MULTRET && nres > LW_FRAME_MAX - below)) {
        lw_trap(w, LW_ERR, "sos_call: %d results don't fit in a frame", (int)nres);
        return;
    }
    if (!lw_charge(w, LW_CALL_STEPS)) {
        return;
    }
    lh_t *s = w->s;
    int rc = lua_pcall(L, nargs, nres, 0);
    // What always stops the app, whatever the function's caller wanted.
    if (w->fatal || w->exiting) {
        stopping(w);
        return;
    }
    if (rc == LUA_ERRMEM || (rc != LUA_OK && (s->mem_fatal || s->killed))) {
        lua_failed(w, L, rc);
        return;
    }
    if (s->mem_fatal || s->killed) {        // the function swallowed it (a pcall of its own)
        lw_trap(w, s->killed ? LW_BUDGET : LW_MEM, "%s", s->killed && s->kill_msg[0] ? s->kill_msg
                : s->killed ? "App took too long" : "not enough memory");
        return;
    }
    if (rc != LUA_OK) {
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            lua_pushcfunction(L, op_errstr);
            if (!prot(w, L, 0, 1)) {
                return;
            }
        }
        RET_I32(rc);
        return;
    }
    if (lua_gettop(L) > LW_FRAME_MAX) {
        lw_trap(w, LW_ERR, "sos_call: the function returned %d values, more than a frame holds",
                (int)(lua_gettop(L) - (top - nargs - 1)));
    }
}

// sos_ref's slots: 1..LW_REFS_MAX of a table only the bridge reaches.
static void n_ref(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    RET_I32(0);
    if (!w) {
        return;
    }
    if (lua_gettop(L) < 1) {
        lw_trap(w, LW_ERR, "sos_ref: the frame is empty");
        return;
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int id = 0;
    for (int i = 0; i < LW_REFS_MAX && !id; i++) {
        if (!(w->refs_live[i / 32] & (1u << (i % 32)))) {
            id = i + 1;
        }
    }
    if (!id) {
        lw_trap(w, LW_ERR, "sos_ref: more than %d values kept (sos_unref the ones you're done with)", LW_REFS_MAX);
        return;
    }
    lua_pushcfunction(L, op_rawseti);
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->refs_ref);
    lua_pushinteger(L, id);
    lua_pushvalue(L, -4);
    if (prot(w, L, 3, 0)) {
        lua_pop(L, 1);
        w->refs_live[(id - 1) / 32] |= 1u << ((id - 1) % 32);
        w->nrefs++;
        RET_I32(id);
    }
}

static bool live_ref(lw_t *w, int32_t ref)
{
    return ref >= 1 && ref <= LW_REFS_MAX && (w->refs_live[(ref - 1) / 32] & (1u << ((ref - 1) % 32)));
}

static void n_unref(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t ref = ARG_I32(0);
    if (!w || ref == 0) {
        return;
    }
    if (!live_ref(w, ref)) {
        lw_trap(w, LW_ERR, "sos_unref: %d is not a kept value", (int)ref);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->refs_ref);
    lua_pushnil(L);
    lua_rawseti(L, -2, ref);                // an existing slot: no allocation
    lua_pop(L, 1);
    w->refs_live[(ref - 1) / 32] &= ~(1u << ((ref - 1) % 32));
    w->nrefs--;
}

static void n_push_ref(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t ref = ARG_I32(0);
    RET_I32(SOS_NIL);
    if (!w || !room(w, L, "sos_push_ref", 1)) {
        return;
    }
    if (ref == 0) {
        lua_pushnil(L);
        return;
    }
    if (!live_ref(w, ref)) {
        lw_trap(w, LW_ERR, "sos_push_ref: %d is not a kept value", (int)ref);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->refs_ref);
    lua_rawgeti(L, -1, ref);
    lua_remove(L, -2);
    RET_I32(sos_type_of(L, -1));
}

static void n_blit(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    int32_t idx = ARG_I32(0), x = ARG_I32(2), y = ARG_I32(3), bw = ARG_I32(4), bh = ARG_I32(5);
    uint32_t addr = ARG_U32(1);
    RET_I32(-1);
    int i = w ? frame_idx(w, L, "sos_blit", idx) : 0;
    if (!i) {
        return;
    }
    if (bw <= 0 || bh <= 0) {
        RET_I32(0);
        return;
    }
    if (bw > 4096 || bh > 4096) {
        lw_trap(w, LW_ERR, "sos_blit: %d x %d pixels, at most 4096 a side", (int)bw, (int)bh);
        return;
    }
    const uint8_t *px = lw_mem(w, "sos_blit", addr, (uint32_t)(bw * bh * 2));
    if (!px) {
        return;
    }
    // Far off any canvas is off it: the clip maths stays in range.
    lh_blit_t b = { .px = px, .w = bw, .h = bh, .x = x < -8192 ? -8192 : x > 8192 ? 8192 : x,
                    .y = y < -8192 ? -8192 : y > 8192 ? 8192 : y };
    lua_pushcfunction(L, lh_cv_blit);
    lua_pushvalue(L, i);
    lua_pushlightuserdata(L, &b);
    if (prot(w, L, 2, 1)) {
        RET_I32(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
}

// ui_frame (C_API.md "Frames straight to the screen"): w x h RGB565 pixels of
// the app's memory to the panel past LVGL, scaled to dw x dh at (x, y) of the
// app area (bsp_display_frame). 1 shown, 0 not (a dialog, the keyboard or the
// OS's panel is over the app: LVGL has the screen), -1 a frame outside the
// app area. A toast keeps its rows: the frame's part above it is drawn.
#define LW_FRAME_SRC_MAX    1024        // a side of the app's frame
#define LW_FRAME_BE         1           // SOS_FRAME_BE: big-endian pixels
#define LW_FRAME_PX_STEP    16          // host steps: 1 per pixel copied, as a blit
#define LW_FRAME_DMA_STEP   256         // ... 1 per pixel the DMA reads where it is

static void n_frame(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t addr = ARG_U32(0);
    int32_t fw = ARG_I32(1), fh = ARG_I32(2), x = ARG_I32(3), y = ARG_I32(4), dw = ARG_I32(5), dh = ARG_I32(6);
    uint32_t flags = ARG_U32(7);
    RET_I32(-1);
    if (!w) {
        return;
    }
    if (fw <= 0 || fh <= 0 || fw > LW_FRAME_SRC_MAX || fh > LW_FRAME_SRC_MAX) {
        lw_trap(w, LW_ERR, "sos_frame: %d x %d pixels, 1 to %d a side", (int)fw, (int)fh, LW_FRAME_SRC_MAX);
        return;
    }
    if (flags & ~(uint32_t)LW_FRAME_BE) {
        lw_trap(w, LW_ERR, "sos_frame: unknown flags 0x%x", (unsigned)flags);
        return;
    }
    const uint16_t *px = lw_mem(w, "sos_frame", addr, (uint32_t)(fw * fh * 2));
    if (!px) {
        return;
    }
    lh_t *s = w->s;
    if (s->svc || !s->area || x < 0 || y < 0 || dw <= 0 || dh <= 0 || (int64_t)x + dw > OS_ROOT_W ||
        (int64_t)y + dh > OS_ROOT_H) {
        return;                             // -1: not inside the app area
    }
    if (s->modal || s->kb || os_confirm_open() || os_overlay() || s->failed) {
        RET_I32(0);
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(s->area, &area);
    int sx = area.x1 + x, sy = area.y1 + y, h = fh, dhh = dh;
    int t0, t1;
    if (os_toast_rows(&t0, &t1) && t0 < sy + dh && t1 >= sy) {
        dhh = t0 - sy;                      // the rows above the toast
        if (dhh <= 0) {
            RET_I32(0);
            return;
        }
        h = (int)((int64_t)dhh * fh / dh);
        if (h <= 0) {
            RET_I32(0);
            return;
        }
    }
    bool be = flags & LW_FRAME_BE;
    // The cheap rate only for the frame the DMA reads where it is (whole, unscaled,
    // aligned); anything bsp_display_frame copies pays per pixel copied.
    bool direct = bsp_display_frame_no_copy(px, fw, fw, h, sx, sy, dw, dhh, be);
    if (!lw_charge(w, direct ? (uint32_t)(fw * h) / LW_FRAME_DMA_STEP : (uint32_t)(dw * dhh) / LW_FRAME_PX_STEP)) {
        return;
    }
    if (bsp_display_frame(px, fw, fw, h, sx, sy, dw, dhh, be) != ESP_OK) {
        lw_trap(w, LW_MEM, "not enough memory");
        return;
    }
    w->frames++;
    w->frames_direct += bsp_display_frame_direct();
    RET_I32(1);
}

// files_read (C_API.md "files"): up to len bytes of a file of the app's
// folder from off, straight into the app's memory. The bytes read (0 at the
// end), or -1 when there's no such file or the card failed.
static void n_file_read(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t np = ARG_U32(0), nl = ARG_U32(1), off = ARG_U32(2), bp = ARG_U32(3), len = ARG_U32(4);
    RET_I32(-1);
    if (!w) {
        return;
    }
    if (!(w->s->pkg.perms & LUA_PERM_FILES)) {
        lw_trap(w, LW_ERR, "files_read: add the \"files\" permission to app.json");
        return;
    }
    if (len > LH_FILES_READ_MAX) {
        lw_trap(w, LW_ERR, "files_read: %u bytes, at most %d per read", (unsigned)len, LH_FILES_READ_MAX);
        return;
    }
    if (nl > LH_FILES_NAME_MAX + 5) {
        RET_I32(-1);
        return;
    }
    // Every pointer checked before use: a bad one has trapped and is NULL (a NULL name
    // with len 0 reached memcpy and restarted the badge, a C service on every boot).
    const char *name = lw_mem(w, "files_read", np, nl);
    if (!name) {
        return;
    }
    void *dst = len ? lw_mem(w, "files_read", bp, len) : NULL;
    if (len && !dst) {
        return;
    }
    char copy[LH_FILES_NAME_MAX + 6];
    memcpy(copy, name, nl);                 // the app can't change it under the check
    unsigned steps = 0;
    const char *err;
    long n = lh_files_read_into(w->s, copy, nl, off, dst, len, &steps, &err);
    if (!lw_charge(w, steps)) {
        return;
    }
    RET_I32(n);
}

// sys.keys()'s set as bits: nothing while a dialog has the keys, and never in a
// service (C_API.md "service"): the owner is using another app, maybe typing a
// Wi-Fi password, and a service has no screen to read keys for.
static void n_keys(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t bits = 0;
    if (w && !w->s->svc && !w->s->modal && !w->s->kb && !os_confirm_open() && !os_overlay()) {
        uint32_t pressed = bsp_buttons_get();
        for (int k = 0; k < (int)(sizeof(s_keys) / sizeof(s_keys[0])); k++) {
            bits |= pressed & os_key_button(k) ? 1u << k : 0;
        }
    }
    RET_I32(bits);
}

static void n_millis(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    RET_I32(w ? (uint32_t)(lh_now() - w->s->t0) : 0);
}

static void n_log(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t len = ARG_U32(1);
    const char *p = w && len ? lw_mem(w, "sos_log", ARG_U32(0), len) : "";
    uint32_t n = len < LW_LOG_MAX ? len : LW_LOG_MAX;
    if (w && p && lw_charge(w, (n + 1) * LW_OUT_STEPS)) {   // + the newline
        lw_out_flush(w);
        lh_print_text(w->s->pkg.id, p, n);
    }
}

static void n_fail(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    lua_State *L;
    lw_t *w = begin(&L);
    uint32_t len = ARG_U32(1);
    if (!w) {
        return;
    }
    const char *p = len ? lw_mem(w, "sos_fail", ARG_U32(0), len) : "";
    if (p) {
        char msg[sizeof(w->msg)];
        size_t n = 0;
        for (uint32_t i = 0; i < len && n + 1 < sizeof(msg); i++) {
            unsigned char c = (unsigned char)p[i];
            msg[n++] = c == '\n' || (c >= 0x20 && c != 0x7f) ? (char)c : '?';
        }
        msg[n] = 0;
        lw_trap(w, LW_ERR, "%s", n ? msg : "The app stopped");
    }
}

static void n_host_abi(wasm_exec_env_t env, uint64_t *a)
{
    (void)env;
    RET_I32(LW_ABI);
}

// const (flash): WAMR sorts the tables it's given, so the app's heap gets a
// copy while it runs (lw_t.natives), and every badge keeps its SRAM.
static const NativeSymbol s_badge[] = {
    {"top", n_top, "()i", NULL},
    {"pop", n_pop, "(i)", NULL},
    {"push_value", n_push_value, "(i)", NULL},
    {"remove", n_remove, "(i)", NULL},
    {"push_nil", n_push_nil, "()", NULL},
    {"push_bool", n_push_bool, "(i)", NULL},
    {"push_int", n_push_int, "(i)", NULL},
    {"push_num", n_push_num, "(f)", NULL},
    {"push_str", n_push_str, "(ii)", NULL},
    {"new_table", n_new_table, "()", NULL},
    {"push_fn", n_push_fn, "(ii)", NULL},
    {"type", n_type, "(i)i", NULL},
    {"to_bool", n_to_bool, "(i)i", NULL},
    {"to_int", n_to_int, "(i)i", NULL},
    {"to_num", n_to_num, "(i)f", NULL},
    {"to_str", n_to_str, "(iii)i", NULL},
    {"len", n_len, "(i)i", NULL},
    {"get", n_get, "(iii)i", NULL},
    {"set", n_set, "(iii)", NULL},
    {"geti", n_geti, "(ii)i", NULL},
    {"seti", n_seti, "(ii)", NULL},
    {"next", n_next, "(i)i", NULL},
    {"global", n_global, "(ii)i", NULL},
    {"call", n_call, "(ii)i", NULL},
    {"ref", n_ref, "()i", NULL},
    {"unref", n_unref, "(i)", NULL},
    {"push_ref", n_push_ref, "(i)i", NULL},
    {"blit", n_blit, "(iiiiii)i", NULL},
    {"frame", n_frame, "(iiiiiiii)i", NULL},
    {"file_read", n_file_read, "(iiiii)i", NULL},
    {"keys", n_keys, "()i", NULL},
    {"millis", n_millis, "()i", NULL},
    {"log", n_log, "(ii)", NULL},
    {"fail", n_fail, "(ii)", NULL},
    {"host_abi", n_host_abi, "()i", NULL},
};

// ---------------------------------------------------------------------------
// Native time (app.aot): the clock runs while a call into the module is open
// and no import is (lua_aot.h). The watchdog reads it from its own task, so
// the order of the writes matters: since_us before running goes up, running
// down before used_us moves.

static void clk_set(lw_t *w)
{
    lw_clock_t *c = &w->clk;
    bool run = w->depth > w->imports;
    if (c->running && !run) {
        c->running = false;
        c->used_us += (uint32_t)esp_timer_get_time() - c->since_us;
    } else if (!c->running && run) {
        c->since_us = (uint32_t)esp_timer_get_time();
        c->running = true;
    }
}

lw_t *lw_import_in(void)
{
    lw_t *w = lw_cur;
    if (!w) {
        return NULL;
    }
    if (over_time(w) || w->fatal || w->exiting) {
        stopping(w);
        return NULL;
    }
    if (w->aot) {
        w->imports++;
        clk_set(w);
    }
    return w;
}

void lw_import_out(lw_t *w)
{
    if (w && w->aot) {
        w->imports--;
        clk_set(w);
        over_time(w);
    }
}

bool lw_port_slow(bool begin)
{
    lw_t *w = lw_cur;
    if (begin) {
        // memory.grow may move the linear memory: a frame the panel still
        // reads from it (ui_frame without a copy) finishes first.
        bsp_display_frame_wait();
    }
    if (begin && w && w->clk.kill) {
        return false;                       // stopping: memory.grow answers -1, and the app stops at once
    }
    if (w && w->aot) {
        w->imports += begin ? 1 : -1;
        clk_set(w);
    }
    return true;
}

typedef void (*lw_raw_fn)(wasm_exec_env_t env, uint64_t *a);

// The "badge" imports of a native app go through here (lw_boot puts the real
// one in the attachment: wamrc doesn't know them, so native code calls them
// through aot_invoke_native, which hands it over), for the clock and the stop.
static void n_native(wasm_exec_env_t env, uint64_t *a)
{
    lw_raw_fn fn = (lw_raw_fn)wasm_runtime_get_function_attachment(env);
    lw_t *w = lw_import_in();
    if (w) {
        fn(env, a);
    }
    lw_import_out(w);
}

void *lw_native(lw_t *w, const char *fn, void *p, uint32_t len)
{
    if (!wasm_runtime_validate_native_addr(w->inst, p, len)) {
        uint64_t at = p ? wasm_runtime_addr_native_to_app(w->inst, p) : 0;
        lw_trap(w, LW_ERR, "%s: the pointer 0x%x (%u bytes) is outside the app's memory", fn, (unsigned)at,
                (unsigned)len);
        return NULL;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Calling into the app

static void update_stats(lw_t *w)
{
    wasm_memory_inst_t m = wasm_runtime_get_default_memory(w->inst);
    uint64_t size = m ? wasm_memory_get_cur_page_count(m) * wasm_memory_get_bytes_per_page(m) : 0;
    w->s->wasm_stat.mem_size = (uint32_t)size;
    w->s->wasm_stat.frames = w->frames;
    w->s->wasm_stat.frames_direct = w->frames_direct;
    if (size > w->s->wasm_stat.mem_peak) {
        w->s->wasm_stat.mem_peak = (uint32_t)size;
    }
}

// Calls fn with argv (results come back in it) from a closure whose Lua
// frame becomes the app's. true: it returned; false: the app exited
// (proc_exit, like sys.exit()). Raises when the app must stop; no WAMR frame
// is above this one then.
static bool enter(lua_State *L, lw_t *w, wasm_function_inst_t fn, uint32_t argc, uint32_t *argv)
{
    lh_t *s = w->s;
    if (w->exiting) {
        return false;
    }
    if (w->fatal) {
        return raise(L, w);
    }
    if (!lua_checkstack(L, LW_FRAME_MAX + LW_SLACK)) {
        lw_trap(w, LW_MEM, "not enough memory");
        return raise(L, w);
    }
    if (lua_gettop(L) > LW_FRAME_MAX) {
        lua_settop(L, LW_FRAME_MAX);
    }
    if (w->depth == 0) {
        // A new callback (lh_callback) gets a fresh budget; more calls in the
        // same callback (_initialize then on_start, a list's select then
        // on_key) share what's left of it.
        if (w->seq != s->cb_seq) {
            w->seq = s->cb_seq;
            w->left = (int)wasm_steps_max(s);
            w->clk.used_us = 0;
            w->clk.tries = 0;
            w->clk.cb_us = (uint32_t)esp_timer_get_time();
        }
        if (!w->aot) {
            wasm_runtime_set_instruction_count_limit(w->env, w->left);
        }
        uintptr_t low = lh_stack_low();
        wasm_runtime_set_native_stack_boundary(w->env, low ? (uint8_t *)(low + LW_STACK_MARGIN) : NULL);
    }
    // The innermost call's landing for the watchdog's jump (native code only
    // ever gets there: see the top of this file).
    jmp_buf jb;
    jmp_buf *outer = w->clk.jb;
    lua_State *saved = w->L;
    volatile bool ok = false, killed = false;
    w->L = L;
    w->depth++;
    w->clk.open = true;
    if (w->aot) {
        w->clk.jb = &jb;
        clk_set(w);
    }
    if (!w->aot) {
        ok = wasm_runtime_call_wasm(w->env, fn, argc, argv);    // interpreted: no jump ever comes
    } else if (setjmp(jb) == 0) {
        ok = wasm_runtime_call_wasm(w->env, fn, argc, argv);
    } else {
        killed = true;                      // from lua_aot.c's trampoline
    }
    w->depth--;
    w->L = saved;
    w->clk.open = w->depth > 0;
    if (w->aot) {
        w->clk.jb = outer;
        clk_set(w);
    }
    if (killed) {
        // Landed from the trampoline (aimed at module code) or from a bulk
        // memory helper (aimed_pc: the helper's caller in WAMR).
        bool in_module = w->clk.aimed_pc >= (uint32_t)(uintptr_t)w->clk.code_lo
                         && w->clk.aimed_pc < (uint32_t)(uintptr_t)w->clk.code_hi;
        ESP_LOGW(TAG, "%s: %s 0x%08lx stopped after %lu us of native time (watchdog pass %u%s)", s->pkg.id,
                 in_module ? "stuck native code at" : "a bulk memory operation via", (unsigned long)w->clk.aimed_pc,
                 (unsigned long)w->clk.aimed_us, (unsigned)w->clk.tries, w->clk.ceiling ? ", the ceiling" : "");
        over_time(w);
    }
    if (w->depth == 0) {
        if (w->aot) {
            if (w->clk.used_us > s->wasm_stat.native_peak_us) {
                s->wasm_stat.native_peak_us = w->clk.used_us;
            }
            s->wasm_cpu = w->clk.used_us * LW_SVC_STEPS_PER_US;   // a service's minute (lua_svc.c)
        } else {
            int left = wasm_runtime_get_instruction_count_limit(w->env);
            w->left = left < 0 ? 0 : left;
            uint32_t used = wasm_steps_max(s) - (uint32_t)w->left;
            if (used > s->wasm_stat.steps_peak) {
                s->wasm_stat.steps_peak = used;
            }
            s->wasm_cpu = used;
        }
        update_stats(w);
    }
    if (!ok) {
        // The ceiling stops the interpreter through its instruction count:
        // over_time says so before the count's own words can.
        if (!w->fatal && !w->exiting && !over_time(w)) {
            wamr_reason(w);
        }
        wasm_runtime_clear_exception(w->inst);
        if (w->fatal) {
            return raise(L, w);
        }
        return false;
    }
    return !w->exiting;
}

// A function value the app made (sos_push_fn): calls fn(ud, nargs) with the
// arguments as its frame; it returns how many results it left on top.
static int fn_call(lua_State *L)
{
    lh_t *s = lh_get(L);
    lw_t *w = s->wasm;
    if (!w || !w->inst) {
        return 0;                           // the app is gone (a late callback)
    }
    uint32_t fn = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
    const char *why;
    wasm_function_inst_t f = table_fn(w, fn, &why);
    if (!f) {
        lw_trap(w, LW_ERR, "a callback: %s (function %u)", why, (unsigned)fn);
        return raise(L, w);
    }
    if (lua_gettop(L) > LW_FRAME_MAX) {
        lua_settop(L, LW_FRAME_MAX);
    }
    uint32_t argv[2] = { (uint32_t)lua_tointeger(L, lua_upvalueindex(2)), (uint32_t)lua_gettop(L) };
    if (!enter(L, w, f, 2, argv)) {
        return 0;
    }
    int32_t n = (int32_t)argv[0];
    if (n < 0 || n > lua_gettop(L)) {
        lw_trap(w, LW_ERR, "a callback returned %d results, but its frame has %d values", (int)n, lua_gettop(L));
        return raise(L, w);
    }
    return n;
}

// The globals the host calls (lua_host.c, lua_net.c, lua_meet.c).
static int g_on_key(lua_State *L)
{
    lw_t *w = lh_get(L)->wasm;
    const char *k = lua_tostring(L, 1);
    uint32_t argv[2] = { 0, (uint32_t)lua_toboolean(L, 2) };
    for (uint32_t i = 0; k && i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        if (!strcmp(k, s_keys[i])) {
            argv[0] = i;
        }
    }
    lua_settop(L, 0);                       // on_key's frame starts empty: the key is an argument
    bool handled = !enter(L, w, w->f_key, 2, argv) || argv[0] != 0;
    lua_settop(L, 0);
    lua_pushboolean(L, handled);
    return 1;
}

static int g_on_tick(lua_State *L)
{
    lw_t *w = lh_get(L)->wasm;
    uint32_t argv[1] = { (uint32_t)lua_tointeger(L, 1) };
    lua_settop(L, 0);
    enter(L, w, w->f_tick, 1, argv);
    return 0;
}

static int g_on_exit(lua_State *L)
{
    lw_t *w = lh_get(L)->wasm;
    uint32_t argv[1];
    if (w->fatal) {
        return 0;                           // stopped on an error: the app's state can't be trusted
    }
    lua_settop(L, 0);
    enter(L, w, w->f_exit, 0, argv);
    return 0;
}

static int g_on_mesh(lua_State *L)          // frame: 1 the sender's id, 2 the message
{
    lw_t *w = lh_get(L)->wasm;
    uint32_t argv[1];
    lua_settop(L, 2);
    enter(L, w, w->f_mesh, 0, argv);
    return 0;
}

static int g_on_meet(lua_State *L)          // frame: 1 who asks, 2 their data; a table on top to sign
{
    lw_t *w = lh_get(L)->wasm;
    uint32_t argv[1] = { 0 };
    lua_settop(L, 2);
    if (!enter(L, w, w->f_meet, 0, argv) || argv[0] != 1 || lua_gettop(L) < 1 || !lua_istable(L, -1)) {
        return 0;
    }
    return 1;
}

static int g_on_mail(lua_State *L)          // frame: 1 the mail (Mailbox, 2.3.0)
{
    lw_t *w = lh_get(L)->wasm;
    uint32_t argv[1];
    lua_settop(L, 1);
    enter(L, w, w->f_mail, 0, argv);
    return 0;
}

static void call_export(lua_State *L, lw_t *w, const char *name)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(w->inst, name);
    uint32_t argv[1] = { 0 };
    if (f) {
        lua_settop(L, 0);
        enter(L, w, f, 0, argv);
    }
}

void lw_call_named(lua_State *L, lh_t *s, const char *fn)
{
    lw_t *w = s->wasm;
    if (!w || !w->inst || w->fatal || w->exiting) {
        return;
    }
    wasm_function_inst_t f = wasm_runtime_lookup_function(w->inst, fn);
    if (!f || wasm_func_get_param_count(f, w->inst) != 0 || wasm_func_get_result_count(f, w->inst) != 0) {
        return;                             // not exported, or not (void) -> void: not a callback
    }
    uint32_t argv[1];
    enter(L, w, f, 0, argv);
}

// The main chunk: instantiate, check the ABI, run the C library's
// constructors (_initialize, wasi-libc's reactor start), then on_start (a
// service instance: on_service_start, 2.3.0).
static int lw_main(lua_State *L)
{
    lh_t *s = lh_get(L);
    lw_t *w = s->wasm;
    char err[128];
    w->inst = wasm_runtime_instantiate(w->mod, LW_WASM_STACK, 0, err, sizeof(err));
    w->env = w->inst ? wasm_runtime_create_exec_env(w->inst, LW_WASM_STACK) : NULL;
    w->clk.env = w->env;                    // the watchdog's way to stop the interpreter (lua_aot.h)
    if (!w->env) {
        if (s->mem_hit || strstr(err, "allocate memory failed") || w->inst) {
            s->mem_fatal = true;
            return luaL_error(L, "not enough memory");
        }
        return luaL_error(L, "The app can't start: %s", err);
    }
    update_stats(w);
    const char *entry = s->svc ? "on_service_start" : "on_start";
    w->f_start = wasm_runtime_lookup_function(w->inst, entry);
    w->f_key = wasm_runtime_lookup_function(w->inst, "on_key");
    w->f_tick = wasm_runtime_lookup_function(w->inst, "on_tick");
    w->f_exit = wasm_runtime_lookup_function(w->inst, "on_exit");
    w->f_mesh = wasm_runtime_lookup_function(w->inst, "on_mesh");
    w->f_meet = wasm_runtime_lookup_function(w->inst, "on_meet");
    w->f_mail = wasm_runtime_lookup_function(w->inst, "on_mail");

    lua_newtable(L);
    w->refs_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    static const struct { const char *name; lua_CFunction fn; } globals[] = {
        {"on_key", g_on_key}, {"on_tick", g_on_tick}, {"on_exit", g_on_exit},
        {"on_mesh", g_on_mesh}, {"on_meet", g_on_meet}, {"on_mail", g_on_mail},
    };
    wasm_function_inst_t *fns[] = { &w->f_key, &w->f_tick, &w->f_exit, &w->f_mesh, &w->f_meet, &w->f_mail };
    for (size_t i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
        if (*fns[i]) {
            lua_register(L, globals[i].name, globals[i].fn);
        }
    }

    wasm_function_inst_t abi = wasm_runtime_lookup_function(w->inst, "badge_abi");
    if (abi) {
        uint32_t argv[1] = { 0 };
        lua_settop(L, 0);
        if (!enter(L, w, abi, 0, argv)) {
            return 0;
        }
        if ((int32_t)argv[0] > LW_ABI) {
            return luaL_error(L, "This app needs a newer Solana OS (its C ABI is %d, this badge's %d). "
                              "Update in Settings > Software update.", (int)(int32_t)argv[0], LW_ABI);
        }
    }
    call_export(L, w, "_initialize");
    if (!w->exiting) {
        call_export(L, w, entry);
    }
    lua_settop(L, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Boot and close

static void *port_realloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    return lh_mem_realloc(ud, ptr, osize, nsize);
}

static bool file_size(const char *id, const char *file, char *path, size_t n, long *size)
{
    char rel[64];
    struct stat st;
    lua_pkg_rel(rel, sizeof(rel), id, file);
    os_data_path(path, n, rel);
    *size = stat(path, &st) == 0 ? (long)st.st_size : -1;
    return *size >= 0;
}

// The file into the app's heap. NULL: too big, unreadable or no memory (*mem).
static uint8_t *read_file(lh_t *s, const char *path, long size, bool *mem)
{
    *mem = false;
    if (size <= 0 || size > LUA_PKG_CODE_MAX) {
        return NULL;
    }
    uint8_t *buf = lh_mem_realloc(s, NULL, 0, (size_t)size);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    bool ok = f && fread(buf, 1, (size_t)size, f) == (size_t)size;
    if (f) {
        fclose(f);
    }
    if (!ok && buf) {
        lh_mem_realloc(s, buf, (size_t)size, 0);
    }
    *mem = !buf;
    return ok ? buf : NULL;
}

// What the module imports: only the badge's own (linked at load). Anything
// else, or one of ours with another type, and the app doesn't load.
static void check_imports(lua_State *L, lw_t *w)
{
    int32_t n = wasm_runtime_get_import_count(w->mod);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(w->mod, i, &im);
        char name[80];
        snprintf(name, sizeof(name), "%.30s.%.40s", im.module_name ? im.module_name : "?", im.name ? im.name : "?");
        lua_pkg_plain(name, sizeof(name), name);
        if (im.kind != WASM_IMPORT_EXPORT_KIND_FUNC) {
            luaL_error(L, "The app imports %s, which isn't a function: the badge gives only functions", name);
        }
        if (!im.linked) {
            bool ours = im.module_name && (!strcmp(im.module_name, "badge")
                                           || !strcmp(im.module_name, "wasi_snapshot_preview1"));
            luaL_error(L, ours ? "The app imports %s with a type the badge doesn't have (rebuild it with this SDK)"
                               : "The app imports %s, which the badge doesn't have", name);
        }
    }
}

// The exports the badge calls, with the types badge_abi.h gives them. And
// none WAMR would run by itself while it instantiates the module, on an exec
// env of its own with no instruction limit (wasm_runtime.c and aot_runtime.c
// execute_post_instantiate_functions): the SDK never exports them (its
// constructors run from _initialize, which the badge calls under the budget).
static void check_exports(lua_State *L, lw_t *w)
{
    static const struct { const char *name; uint8_t params, results; } want[] = {
        {"on_start", 0, 0}, {"on_service_start", 0, 0}, {"on_key", 2, 1}, {"on_tick", 1, 0}, {"on_exit", 0, 0},
        {"on_mesh", 0, 0}, {"on_meet", 0, 1}, {"on_mail", 0, 0}, {"badge_abi", 0, 1}, {"_initialize", 0, 0},
        {"on_service", 0, 0}, {"on_message", 0, 0}, {"on_stop", 0, 0},
    };
    size_t entry = w->s->svc ? 1 : 0;       // the instance's on_start: a service's is on_service_start
    bool start = false;
    int32_t n = wasm_runtime_get_export_count(w->mod);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t ex;
        wasm_runtime_get_export_type(w->mod, i, &ex);
        if (!strcmp(ex.name, "__wasm_call_ctors") || !strcmp(ex.name, "__post_instantiate")) {
            luaL_error(L, "The app exports %s, which the badge doesn't run (rebuild it with the SDK)", ex.name);
        }
        for (size_t k = 0; ex.kind == WASM_IMPORT_EXPORT_KIND_FUNC && k < sizeof(want) / sizeof(want[0]); k++) {
            if (strcmp(ex.name, want[k].name)) {
                continue;
            }
            uint32_t np = wasm_func_type_get_param_count(ex.u.func_type);
            uint32_t nr = wasm_func_type_get_result_count(ex.u.func_type);
            bool ok = np == want[k].params && nr == want[k].results;
            for (uint32_t j = 0; ok && j < np; j++) {
                ok = wasm_func_type_get_param_valkind(ex.u.func_type, j) == WASM_I32;
            }
            for (uint32_t j = 0; ok && j < nr; j++) {
                ok = wasm_func_type_get_result_valkind(ex.u.func_type, j) == WASM_I32;
            }
            if (!ok) {
                luaL_error(L, "The app's %s has the wrong type (see badge_abi.h)", want[k].name);
            }
            start = start || k == entry;
        }
    }
    if (!start) {
        luaL_error(L, entry ? "The app has no on_service_start (a C app's service needs it)" : "The app has no on_start");
    }
}

// A start function would run while the module is instantiated, outside any
// limit (WAMR gives it an exec env of its own with no instruction count).
// The SDK never emits one and the store refuses one, but a sideload or an
// edited card never passed the store: the badge refuses it too.
static bool has_start(lw_t *w)
{
    const WASMModule *m = (const WASMModule *)w->mod;
    if (m->module_type == Wasm_Module_Bytecode) {
        return m->start_function != (uint32)-1;
    }
    return lw_aot_has_start(w->mod);
}

// WAMR's loader picks the format from the first bytes, and the badge's WAMR
// has the AOT runtime: so each file must be the format its name promises
// before the loader sees it. app.wasm that is native code (an AOT file
// under the wrong name, sideloaded or edited on the card) would otherwise
// load unsigned.
static const uint8_t s_wasm_magic[4] = {0x00, 'a', 's', 'm'};
static const uint8_t s_aot_magic[4] = {0x00, 'a', 'o', 't'};

void lw_boot(lua_State *L, lh_t *s)
{
    char path[160];
    long size = 0;
    const char *id = s->pkg.id;
    lh_wasm_stat_t st = { .c = true, .slot = -1 };
    // The store's native form, if it may run (lua_aot.h): this build's target,
    // a badge that can, the file the manifest describes and the store's
    // signature over its SHA-256, all before a byte of the file is mapped.
    const char *no_aot = lw_aot_refusal(&s->pkg, path, sizeof(path), &size, &st.sig_us);
    lw_t *w = lh_mem_realloc(s, NULL, 0, sizeof(*w));
    if (!w) {
        s->mem_fatal = true;
        luaL_error(L, "not enough memory");
    }
    memset(w, 0, sizeof(*w));
    if (!claim(s->svc != NULL)) {
        lh_mem_realloc(s, w, sizeof(*w), 0);
        luaL_error(L, s->svc ? "A C app is using the badge's WebAssembly runtime"
                             : "A C service is using the badge's WebAssembly runtime: try again in a moment");
    }
    w->claimed = true;
    w->s = s;
    w->refs_ref = LUA_NOREF;
    w->xip.slot = -1;
    s->wasm = w;
    lw_cur = w;
    if (!no_aot) {
        // Into a flash slot (copied unless it's there), mapped for the CPU,
        // and its bytes hashed through the mapping against the signed SHA-256.
        int64_t t = esp_timer_get_time();
        bool mapped = lua_xip_map(path, s->pkg.aot_sha, (size_t)size, LUA_XIP_EXEC, &w->xip);
        st.map_us = (uint32_t)(esp_timer_get_time() - t);
        no_aot = mapped ? NULL : "its flash slot doesn't hold the signed code";
    }
    w->aot = !no_aot;
    bool wasm = !w->aot && file_size(id, "app.wasm", path, sizeof(path), &size);
    if (no_aot && no_aot[0]) {
        ESP_LOGW(TAG, "%s: native code not used (%s)%s", id, no_aot, wasm ? ": running app.wasm" : "");
    }
    if (no_aot && lw_aot_refusal_is_key(no_aot)) {
        lw_aot_key_refresh();               // the store may have a new key: the next launch can use it
    }
    if (!w->aot && !wasm) {
        luaL_error(L, no_aot && no_aot[0] ? "The app can't start: %s, and it has no app.wasm. Install it again "
                   "from the App Store" : "app.wasm is missing", no_aot);
    }
    st.aot = w->aot;
    st.slot = w->xip.slot;
    st.copied = w->xip.copied;
    s->wasm_stat = st;
    if (w->aot) {
        w->bin = (uint8_t *)w->xip.data;    // WAMR reads the mapping in place, until it unloads
        w->clk.code_lo = w->xip.data;
        w->clk.code_hi = w->xip.data + w->xip.size;
    } else {
        bool mem;
        w->bin = read_file(s, path, size, &mem);
        if (!w->bin) {
            s->mem_fatal = mem;
            luaL_error(L, mem ? "not enough memory" : size > LUA_PKG_CODE_MAX ? "app.wasm is too big (%d KB at most)"
                       : "Can't read app.wasm", LUA_PKG_CODE_MAX / 1024);
        }
    }
    w->bin_size = (size_t)size;
    s->wasm_stat.module_size = (uint32_t)size;
    if (w->bin_size < 8 || memcmp(w->bin, w->aot ? s_aot_magic : s_wasm_magic, 4)) {
        luaL_error(L, w->aot ? "app.aot isn't native code for the badge" : !memcmp(w->bin, s_aot_magic, 4)
                   ? "app.wasm is native code, not WebAssembly: only the store's signed app.aot runs natively"
                   : "app.wasm isn't WebAssembly");
    }

    lw_port_heap(port_realloc, s);
    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) {
        s->mem_fatal = true;
        luaL_error(L, "not enough memory");
    }
    w->rt = true;
    uint32_t n_badge = sizeof(s_badge) / sizeof(s_badge[0]), n_wasi;
    const NativeSymbol *wasi = lw_wasi_symbols(w->aot, &n_wasi);
    w->n_natives = n_badge + n_wasi;
    w->natives = lh_mem_realloc(s, NULL, 0, w->n_natives * sizeof(NativeSymbol));
    if (w->natives) {
        memcpy(w->natives, s_badge, sizeof(s_badge));
        memcpy(w->natives + n_badge, wasi, n_wasi * sizeof(NativeSymbol));
        for (uint32_t i = 0; w->aot && i < n_badge; i++) {
            w->natives[i].attachment = w->natives[i].func_ptr;     // through n_native: the clock and the stop
            w->natives[i].func_ptr = (void *)n_native;
        }
    }
    if (!w->natives || !wasm_runtime_register_natives_raw("badge", w->natives, n_badge)
            || !(w->aot ? wasm_runtime_register_natives : wasm_runtime_register_natives_raw)(
                   "wasi_snapshot_preview1", w->natives + n_badge, n_wasi)) {
        s->mem_fatal = true;
        luaL_error(L, "not enough memory");
    }
    char err[128];
    // Not freeable: WAMR keeps pointing into the file (the flash mapping for
    // app.aot) until wasm_runtime_unload, and never writes to it.
    LoadArgs args = { .name = "", .wasm_binary_freeable = false };
    w->mod = wasm_runtime_load_ex(w->bin, (uint32_t)w->bin_size, &args, err, sizeof(err));
    if (!w->mod) {
        if (s->mem_hit || strstr(err, "allocate memory failed")) {
            s->mem_fatal = true;
            luaL_error(L, "not enough memory");
        }
        lua_pkg_plain(err, sizeof(err), err);
        luaL_error(L, "%s doesn't load: %s", w->aot ? "app.aot" : "app.wasm", err);
    }
    if (has_start(w)) {
        luaL_error(L, "The app has a start function, which the badge doesn't run (rebuild it with the SDK)");
    }
    check_imports(L, w);
    check_exports(L, w);
    lua_pushcfunction(L, lw_main);
    w->clk.aot = w->aot;
    w->clk.task = xTaskGetCurrentTaskHandle();
    w->clk.limit_ms = s->svc ? LW_SVC_CALLBACK_MS : LW_AOT_CALLBACK_MS;
    lw_aot_watch(&w->clk);                  // both forms: the ceiling (and native time)
    if (w->aot) {
        ESP_LOGI(TAG, "%s: app.aot, %ld bytes, native from flash slot %d (%s; signature %lu us, slot %lu us)", id,
                 size, w->xip.slot, w->xip.copied ? "copied in" : "already there", (unsigned long)st.sig_us,
                 (unsigned long)st.map_us);
    } else {
        ESP_LOGI(TAG, "%s: app.wasm, %ld bytes, interpreted", id, size);
    }
}

void lw_close(lh_t *s)
{
    lw_t *w = s->wasm;
    if (!w) {
        return;
    }
    lw_aot_unwatch();                       // before anything it reads goes
    if (w->inst) {
        update_stats(w);
    }
    lw_out_flush(w);
    if (w->env) {
        wasm_runtime_destroy_exec_env(w->env);
    }
    if (w->inst) {
        wasm_runtime_deinstantiate(w->inst);
    }
    if (w->mod) {
        wasm_runtime_unload(w->mod);
    }
    if (w->rt) {
        wasm_runtime_destroy();
    }
    lw_port_heap(NULL, NULL);
    if (w->natives) {
        lh_mem_realloc(s, w->natives, w->n_natives * sizeof(NativeSymbol), 0);
    }
    if (w->aot) {
        lua_xip_unmap(&w->xip);             // after wasm_runtime_unload: the module pointed into it
    } else if (w->bin) {
        lh_mem_realloc(s, w->bin, w->bin_size, 0);
    }
    lw_cur = NULL;
    s->wasm = NULL;
    bool claimed = w->claimed;
    lh_mem_realloc(s, w, sizeof(*w), 0);
    if (claimed) {
        release();                          // after WAMR is down: the next C program may boot
    }
}

// ---------------------------------------------------------------------------
// stdout / stderr (lua_wasi.c's fd_write): whole lines, as print makes them

void lw_out_flush(lw_t *w)
{
    if (w->line_len) {
        lh_print_text(w->s->pkg.id, w->line, w->line_len);
        w->line_len = 0;
    }
}

void lw_out(lw_t *w, const char *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            if (!w->line_len) {
                lh_print_text(w->s->pkg.id, "", 0);
            }
            lw_out_flush(w);
            continue;
        }
        if (w->line_len == sizeof(w->line)) {
            lw_out_flush(w);
        }
        w->line[w->line_len++] = p[i];
    }
}
