// Lua app host, see lua_host.h and docs/LUA_API.md.
//
// One app runs at a time, in a fresh lua_State on the "lua" task:
//   boot      load the code: the store's compiled code run in place from
//             flash (lua_code.c), else main.lua compiled from the card;
//             then open the sandboxed libraries (no lock)
//   run       lock LVGL, run the chunk, then loop: keys, API answers, mesh
//             messages, timers and the 30 ms on_tick, each a callback under
//             the lock with its own instruction budget
//   shutdown  the OS closes the app (HOME, CANCEL, sys.exit): on_exit runs
//             without the lock (UI calls do nothing then), storage is
//             written, the state is closed and the task ends
// An error stops the script at once: the error screen replaces its UI, and
// CANCEL goes back to the launcher.
//
// Lua 5.5 (vendor/LICENSE lists the patches). Libraries are lazy
// (vendor/lauxlib.c): their functions stay in flash until a script uses
// one, ~5 KB less per app than filling every library table up front.
//
// Numbers are 32-bit (LUA_32BITS in vendor/luaconf.h): integers and floats
// both. The C5 has no FPU, so floats are soft-float either way and single
// precision is the cheaper one, and 8-byte values instead of 16 shrink tables
// and stacks: with the sample apps the heap peaked about 20 % lower than with
// 64-bit numbers (snake 30.6 KB instead of 37.1 KB, towel-tycoon 28.2 KB
// instead of 35.3 KB). Integers wrap at ±2^31, which is fine for scores,
// !LAMPs and sys.time() seconds; bigger whole numbers in JSON (ms
// timestamps) arrive as floats.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "bsp.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_http.h"
#include "core/os_mem.h"
#include "vendor/lualib.h"
#include "lua_install.h"
#include "lua_priv.h"

static const char *TAG = "lua";

#define LH_STACK            (12 * 1024)
#define LH_STACK_LOW        4096        // left when Lua raises "C stack overflow"
#define LH_STACK_CRIT       2048        // left when even handling that fails
#define LH_PRIO             2           // below the mesh task, above idle
#define LH_HOOK_EVERY       1000        // instructions between budget checks
#define LH_QUEUE_LEN        16
#define LH_EXIT_WAIT_MS     2000
#define LH_MAX_WAIT_MS      200         // re-check the quit flag at least this often
#define LH_LOCK_POLL_MS     20
#define LH_PREWARM_MS       4000        // longest wait for the server connection at launch
#define LH_BUSY_MAX_MS      1000        // an app that never leaves the CPU idle still yields a tick this often
#define LH_SERVER_PERMS     (LUA_PERM_LAMPS | LUA_PERM_ITEMS | LUA_PERM_PROFILE | LUA_PERM_QUEST | LUA_PERM_HTTP | \
                             LUA_PERM_RPC | LUA_PERM_INVENTORY)

SemaphoreHandle_t lh_mx;
lh_t *lh_cur;
lh_stats_t lh_stats;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done;
static uint32_t s_gen;
static lua_pkg_t s_next;            // app to start on the next enter (under lh_mx)
static char s_title[21] = "Lua";
static bool s_busy;                 // a lua task exists (under lh_mx)
static char s_task_id[LUA_PKG_ID_MAX + 1];  // ... running this app (under lh_mx)

// Native C stack guard (vendor/luaconf.h luai_cstackfull): Lua asks before
// every new C level. The leaf work under a level (LVGL, printf, JSON, pattern
// matching) stays within LH_STACK_LOW. Two tasks run Lua: the app's "lua"
// task and the services' "svc" task (2.3.0), each with its own bounds; 0
// on any other task.
static struct {
    TaskHandle_t task;
    uintptr_t low, crit;
} s_stacks[2];

void lh_stack_enter(int slot, uintptr_t bottom)
{
    s_stacks[slot].low = bottom + LH_STACK_LOW;
    s_stacks[slot].crit = bottom + LH_STACK_CRIT;
    s_stacks[slot].task = xTaskGetCurrentTaskHandle();
}

void lh_stack_leave(int slot)
{
    s_stacks[slot].task = NULL;
    s_stacks[slot].low = s_stacks[slot].crit = 0;
}

static int stack_slot(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    return me == s_stacks[0].task ? 0 : me == s_stacks[1].task ? 1 : -1;
}

uintptr_t lh_stack_low(void)
{
    int i = stack_slot();
    return i < 0 ? 0 : s_stacks[i].low;
}

int lh_cstack_check(void)
{
    volatile char probe;
    uintptr_t sp = (uintptr_t)&probe;
    int i = stack_slot();
    if (i < 0 || !s_stacks[i].low) {
        return 0;
    }
    return sp < s_stacks[i].crit ? 2 : sp < s_stacks[i].low ? 1 : 0;
}

int64_t lh_now(void)
{
    return esp_timer_get_time() / 1000;
}

bool lh_post(const lh_ev_t *ev)
{
    return s_q && xQueueSend(s_q, ev, 0) == pdTRUE;
}

bool lh_post_to(lh_t *s, const lh_ev_t *ev)
{
    return s && s->svc ? lh_svc_post(s, ev) : lh_post(ev);
}

void lh_need_perm(lua_State *L, uint32_t perm, const char *module)
{
    if (!(lh_get(L)->pkg.perms & perm)) {
        luaL_error(L, "%s: add the \"%s\" permission to app.json", module, module);
    }
}

// ---------------------------------------------------------------------------
// Limits: a capped allocator and an instruction budget per callback

size_t lua_host_heap_max(void)
{
    return os_mem_psram() ? LUA_HOST_HEAP_MAX : LUA_HOST_HEAP_MAX_SRAM;
}

void *lh_mem_realloc(lh_t *s, void *ptr, size_t osize, size_t nsize)
{
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        free(ptr);
        s->mem -= old;
        return NULL;
    }
    if (nsize > old && s->mem - old + nsize > s->heap_max) {
        s->mem_hit = true;
        return NULL;    // Lua collects garbage and retries, then fails with "not enough memory"
    }
    if (nsize > old) {
        // The badge's own share: no growth that would leave the heap the app
        // lives in (PSRAM, or SRAM without it) below its reserve (the OS task,
        // mesh and cloud allocate while an app runs). Lua collects garbage and
        // retries; if that's not enough the app stops with "The badge is low
        // on memory". Checked once per KB of growth while there is room (the
        // emulator's free-size call walks its heap), and at every growth once
        // a check has failed: the counter only resets when one passes, so the
        // retry and the small allocations after a refusal can't slip under
        // the reserve.
        s->grown += nsize - old;
        if (s->grown >= 1024) {
            size_t room = heap_caps_get_free_size(s->psram ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT);
            if (room < (s->psram ? LUA_HOST_PSRAM_RESERVE : LUA_HOST_SYS_RESERVE) + (nsize - old)) {
                return NULL;
            }
            s->grown = 0;
        }
    }
    // In PSRAM only: a full PSRAM fails here and Lua collects and retries,
    // rather than spilling into the SRAM the system needs.
    void *p = s->psram ? heap_caps_realloc(ptr, nsize, OS_MEM_PSRAM) : realloc(ptr, nsize);
    if (p) {
        s->mem = s->mem - old + nsize;
        if (s->mem > s->mem_peak) {
            s->mem_peak = s->mem;
        }
    }
    return p;
}

static void *lh_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    return lh_mem_realloc(ud, ptr, osize, nsize);
}

static void step_hook(lua_State *L, lua_Debug *ar);

// Over budget: from now on every thread fails at every instruction, so
// neither pcall() nor a coroutine can keep the script going; a callback that
// still returns normally is failed by lh_callback. level: where to report.
static int over_budget(lua_State *L, int level)
{
    lh_t *s = lh_get(L);
    s->killed = true;
    lua_sethook(L, step_hook, LUA_MASKCOUNT, 1);
    if (s->L && s->L != L) {
        lua_sethook(s->L, step_hook, LUA_MASKCOUNT, 1);
    }
    if (s->pkg.lang == LUA_PKG_LANG_C) {
        // A C app's own code counts in wasm instructions (lua_wasm.c); this is
        // the host work an API call did for it.
        lua_pushfstring(L, LH_HOST_STEPS_MSG, (int)s->steps_max);
    } else {
        luaL_where(L, level);
        if (s->svc) {
            lua_pushliteral(L, "Service took too long");
        } else {
            lua_pushliteral(L, "Script took too long");
        }
        lua_concat(L, 2);
    }
    if (!s->kill_msg[0]) {
        strlcpy(s->kill_msg, lua_tostring(L, -1), sizeof(s->kill_msg));
    }
    return lua_error(L);
}

static void step_hook(lua_State *L, lua_Debug *ar)
{
    lh_t *s = lh_get(L);
    s->steps += LH_HOOK_EVERY;
    // A service (2.3.0) also has a second of wall-clock time per callback:
    // work a VM instruction starts in C (string.rep of a big string, a long
    // concatenation) isn't counted as instructions.
    if (s->steps > s->steps_max || (s->svc && lh_now() - s->cb_t0 > LH_SVC_CALLBACK_MS)) {
        over_budget(L, 0);      // level 0: the script line running now
    }
}

// Work done in C (pattern matching, vendor/lstrlib.c) counts too.
void lh_charge_steps(lua_State *L, unsigned int n)
{
    lh_t *s = lh_get(L);
    s->steps += n;
    if (s->steps > s->steps_max) {
        over_budget(L, 1);      // level 1: the line that called string.find & co.
    }
}

// Every coroutine counts against the same budget (the extra space, and so
// lh_get, is shared). Threads copy the creator's hook anyway; arming it here
// doesn't depend on that.
void lh_thread_new(lua_State *L1)
{
    lua_sethook(L1, step_hook, LUA_MASKCOUNT, LH_HOOK_EVERY);
}

// ---------------------------------------------------------------------------
// Error screen

// An app's text as one console line: control characters become spaces (l_print).
static void one_line(char *t)
{
    for (; *t; t++) {
        if ((unsigned char)*t < 0x20 || *t == 0x7f) {
            *t = ' ';
        }
    }
}

static void show_error(lh_t *s, const char *title, const char *msg)
{
    lh_ui_close_dialog(s);
    lh_ui_clear(s);
    lv_obj_t *a = s->area;
    lv_obj_set_style_bg_color(a, ui_col(COL_BG), 0);
    lv_obj_t *t = ui_label(a, &sol_font_20, COL_ERR, title);
    lv_obj_set_pos(t, 12, 14);
    lv_obj_t *n = ui_label(a, &sol_mono_12, COL_DIM, s->pkg.name);
    lv_obj_set_pos(n, 12, 42);
    lv_obj_t *p = ui_panel(a, 8, 62, OS_ROOT_W - 16, OS_ROOT_H - 62 - UI_HINT_H - 8);
    lv_obj_t *m = ui_label(p, &sol_mono_12, COL_TEXT, msg);
    lv_obj_set_width(m, OS_ROOT_W - 30);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    ui_hint(a, "CANCEL exit");
}

static int run_on_exit(lua_State *L)
{
    lua_getglobal(L, "on_exit");
    if (lua_isfunction(L, -1)) {
        lua_call(L, 0, 0);
    }
    return 0;
}

static void close_state(lh_t *s)
{
    if (s->L) {
        s->ui_off = true;
        lh_callback(s, run_on_exit, NULL);
        lh_ble_stop(s);                     // its callbacks go with the state
        lh_svc_app_closed(s);               // a service.start still waiting answers nobody
        lh_storage_flush(s, true);
        lh_storage_close(s);
        s->steps = 0;
        lua_close(s->L);
        s->L = NULL;
        s->gif_work = NULL;                 // it was a userdata
        if (s->pkg.perms & LUA_PERM_MESH) {
            lh_mesh_end_app();              // again: on_exit (or a finalizer) may have scanned or moved
        }
    }
    lh_fs_close(s);
    lh_files_close(s);
    lh_code_close(s);                       // nothing points into it now
    // A frame the panel may still be reading from the app's memory
    // (ui_frame), before that memory goes with WAMR.
    bsp_display_frame_end();
    lw_close(s);                            // a C app's module and WAMR, after the state that called it
}

// The script failed with rc and its error on the stack (or in msg_in).
// LVGL lock held.
static void fail(lh_t *s, int rc, const char *msg_in)
{
    lua_State *L = s->L;
    char msg[200];
    bool c = s->pkg.lang == LUA_PKG_LANG_C;
    const char *title = c ? "App error" : "Script error";
    if (s->mem_fatal) {
        rc = LUA_ERRMEM;                    // a C app's memory ran out (lua_wasm.c)
    }
    // A memory error that went through pcall (require, or the script's own)
    // comes back as a plain error with Lua's message.
    if (rc != LUA_ERRMEM && s->mem_hit && lua_type(L, -1) == LUA_TSTRING
            && !strcmp(lua_tostring(L, -1), "not enough memory")) {
        rc = LUA_ERRMEM;
    }
    if (rc == LUA_ERRMEM) {
        title = "Out of memory";
        if (!s->mem_hit) {
            strlcpy(msg, "The badge is low on memory.", sizeof(msg));
        } else if (s->heap_max >= 1024 * 1024) {
            snprintf(msg, sizeof(msg), "The app needs more than %d MB.", (int)(s->heap_max / (1024 * 1024)));
        } else {
            snprintf(msg, sizeof(msg), "The app needs more than %d KB.", (int)(s->heap_max / 1024));
        }
    } else {
        // Over budget: report where it ran out, not how the error travelled.
        if (s->killed && s->kill_msg[0]) {
            msg_in = s->kill_msg;
        }
        strlcpy(msg, msg_in ? msg_in : lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                     : "(error is not a string)", sizeof(msg));
        if (s->killed) {
            title = c ? "App took too long" : "Script took too long";
        }
    }
    lua_settop(L, 0);
    char line[sizeof(msg)];                 // the console's copy on one line (l_print)
    strlcpy(line, msg, sizeof(line));
    one_line(line);
    printf("[lua:%s] %s: %s\n", s->pkg.id, title, line);
    strlcpy(s->err, msg, sizeof(s->err));
    s->failed = true;
    lh_mesh_mute();
    lh_mesh_end_app();                      // no scan or app channel outlives the script (close_state repeats it after on_exit)
    lh_ws_stop(s);                          // give the badge its server connection back now
    lh_ble_stop(s);                         // and its Bluetooth heap
    show_error(s, title, msg);
    s->killed = false;
    s->mem_fatal = false;
    lua_sethook(L, step_hook, LUA_MASKCOUNT, LH_HOOK_EVERY);
    close_state(s);
    lh_led_off();
}

bool lh_callback(lh_t *s, lua_CFunction fn, void *arg)
{
    if (s->svc) {
        return lh_svc_callback(s, fn, arg);    // a background service (lua_svc.c)
    }
    lua_State *L = s->L;
    if (!L) {
        return false;
    }
    s->steps = 0;
    s->kill_msg[0] = 0;
    s->cb_seq++;
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, arg);
    int rc = lua_pcall(L, 1, 0, 0);
    if (s->steps > s->steps_peak) {
        s->steps_peak = s->steps;
    }
    if (rc == LUA_OK && s->killed) {
        // The budget ran out inside a coroutine or pcall that swallowed it.
        fail(s, LUA_ERRRUN, s->kill_msg);
        return false;
    }
    if (rc != LUA_OK) {
        if (s->ui_off) {
            // Already closing (on_exit or a finalizer failed): just log it, on one line (l_print).
            char line[160];
            strlcpy(line, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "error", sizeof(line));
            one_line(line);
            printf("[lua:%s] %s\n", s->pkg.id, line);
            lua_settop(L, 0);
            s->mem_fatal = false;
            return false;
        }
        fail(s, rc, NULL);
        return false;
    }
    lh_led_flush(s);
    return true;
}

// ---------------------------------------------------------------------------
// Boot: libraries, sandbox, storage, compile

typedef struct {
    FILE *f;
    char buf[LH_READ_CHUNK];
} reader_t;

static const char *read_chunk(lua_State *L, void *ud, size_t *size)
{
    reader_t *r = ud;
    *size = fread(r->buf, 1, sizeof(r->buf), r->f);
    return *size ? r->buf : NULL;
}

// What an app prints reaches the console, which the web's Run on badge and
// badge beacon panels read (`cloud`, `beacon`). So every line it makes starts
// with the app's tag and no control character passes (a CR would put the
// cursor back to the start): an app can't write a line, or a prompt, of the
// OS's own.
void lh_print_text(const char *id, const char *text, size_t len)
{
    char buf[96];
    size_t n = 0;
    printf("[%s] ", id);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (n + 1 >= sizeof(buf) || c == '\n') {
            fwrite(buf, 1, n, stdout);
            n = 0;
        }
        if (c == '\n') {
            printf("\n[%s] ", id);
        } else {
            buf[n++] = (c < 0x20 && c != '\t') || c == 0x7f ? '?' : (char)c;
        }
    }
    fwrite(buf, 1, n, stdout);
    putchar('\n');
}

static int l_print(lua_State *L)
{
    luaL_Buffer b;
    int n = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    size_t len;
    const char *text = lua_tolstring(L, -1, &len);
    const char *id = lh_get(L)->tag[0] ? lh_get(L)->tag : lh_get(L)->pkg.id;
    if (lh_get(L)->pkg.lang == LUA_PKG_LANG_C || lh_get(L)->svc) {
        // A C app's output is charged however it gets out (LH_OUT_STEPS),
        // before the console blocks on it; a service's too (2.3.0).
        lh_charge_steps(L, (unsigned)((len + 1 > UINT_MAX / LH_OUT_STEPS ? UINT_MAX / LH_OUT_STEPS : len + 1)
                                      * LH_OUT_STEPS));
    }
    luaL_Buffer o;
    luaL_buffinit(L, &o);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            luaL_addstring(&o, "\n[");
            luaL_addstring(&o, id);
            luaL_addstring(&o, "] ");
        } else {
            luaL_addchar(&o, (c < 0x20 && c != '\t') || c == 0x7f ? '?' : (char)c);
        }
    }
    luaL_pushresult(&o);
    printf("[%s] %s\n", id, lua_tostring(L, -1));
    return 0;
}

static int l_collectgarbage(lua_State *L)
{
    const char *opt = luaL_optstring(L, 1, "collect");
    luaL_argcheck(L, !strcmp(opt, "count"), 1, "only \"count\" is available");
    lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024);
    return 1;
}

static int boot(lua_State *L)
{
    // The code first: compiling needs no library, and the parser's garbage
    // is gone before the libraries take their share (the launch peaks ~6 KB
    // lower). The store's compiled code loads in place, with no parser.
    lh_t *s = lh_get(L);
    int rc = LUA_OK;
    if (s->pkg.lang == LUA_PKG_LANG_C) {
        lw_boot(L, s);                      // the main chunk instantiates the module
    } else if ((rc = lh_code_load(L, s, "main.lua")) == LH_CODE_NONE) {
        reader_t *rd = lua_touserdata(L, 1);
        if (!rd->f) {
            luaL_error(L, "main.lua is missing");
        }
        rc = lua_load(L, read_chunk, rd, "=main.lua", "t");
    }
    if (rc != LUA_OK) {
        lua_error(L);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_gc(L, LUA_GCCOLLECT);

    lh_open_base(L);
    lh_open_ui(L);
    lh_open_sys(L);
    lh_open_json(L);
    lh_open_net(L);
    lh_open_ws(L);
    lh_open_ble(L);
    lh_open_fs(L);
    lh_open_files(L);
    lh_open_crypto(L);
    lh_open_mail(L);
    lh_open_gossip(L);
    lh_open_service(L);
    lh_storage_load(s);

    // Then collect generationally: apps churn short-lived strings and
    // tables every tick, and young collections keep the heap near the live
    // data (mesh-wave peaked at 31 KB instead of 45 KB with the incremental
    // collector). A young collection every 10 % of growth instead of 20 %
    // trims the peaks another 0.5-1.7 KB. The cap is a ceiling; every KB a
    // script holds is gone from the badge's heap, and TLS needs ~30 KB of it
    // for each server call.
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCGEN);
    lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMUL, 10);
    return 0;
}

void lh_open_base(lua_State *L)
{
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    // Straight into the globals: there's no package.loaded to list them in.
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        lua_pushcfunction(L, libs[i].func);
        lua_call(L, 0, 1);
        if (i == 0) {
            lua_pop(L, 1);                  // the base library fills _G itself
        } else {
            lua_setglobal(L, libs[i].name);
        }
    }
    // LUA_API.md "Standard library": no loading code, no GC control, no
    // dump (vendor/lstrlib.c leaves string.dump out).
    static const char *const drop[] = {"dofile", "loadfile", "load", "warn"};
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, drop[i]);
    }
    lua_register(L, "collectgarbage", l_collectgarbage);
    lua_register(L, "print", l_print);
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)(esp_random() >> 1));
    lua_pushinteger(L, (lua_Integer)(esp_random() >> 1));
    lua_call(L, 2, 0);
    lua_pop(L, 1);
}

static int run_main(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_call(L, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Events

static const char *const s_key_names[] = {"up", "down", "left", "right", "select", "back"};

static int key_cb(lua_State *L)
{
    lh_ev_t *ev = lua_touserdata(L, 1);
    lh_t *s = lh_get(L);
    if (lh_ui_key(L, s, ev->key, ev->repeat)) {
        return 0;
    }
    bool handled = false;
    lua_getglobal(L, "on_key");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, s_key_names[ev->key]);
        lua_pushboolean(L, ev->repeat);
        lua_call(L, 2, 1);
        handled = lua_toboolean(L, -1);
    }
    if (!handled && ev->key == OS_KEY_BACK && !ev->repeat) {
        s->exit_req = true;
    }
    return 0;
}

static int tick_cb(lua_State *L)
{
    lua_getglobal(L, "on_tick");
    if (lua_isfunction(L, -1)) {
        lua_pushinteger(L, (lua_Integer)(intptr_t)lua_touserdata(L, 1));
        lua_call(L, 1, 0);
    }
    return 0;
}

// Takes the LVGL lock unless the OS is closing the app.
static bool lock(lh_t *s)
{
    while (!lvgl_port_lock(LH_LOCK_POLL_MS)) {
        if (s->quit) {
            return false;
        }
    }
    if (s->quit) {
        lvgl_port_unlock();
        return false;
    }
    return true;
}

static void run_events(lh_t *s, const lh_ev_t *ev)
{
    if (ev) {
        if (ev->kind == LH_EV_KEY && ev->key < sizeof(s_key_names) / sizeof(s_key_names[0])) {
            lh_callback(s, key_cb, (void *)ev);
        } else if (ev->kind == LH_EV_MESH) {
            lh_mesh_event(s, ev->data);
        } else if (ev->kind == LH_EV_MEET) {
            lh_meet_event(s, ev->data);
        } else if (ev->kind == LH_EV_MAIL) {
            lh_mail_event(s, ev->data);
        } else if (ev->kind == LH_EV_CALL || ev->kind == LH_EV_SVC_START) {
            lh_svc_app_event(s, ev);        // the app's service, lua_host_call (2.3.0)
        }                                   // LH_EV_SCAN only wakes us: lh_mesh_scan_run below
    }
    lh_calls_run(s);
    lh_ws_run(s);
    lh_ble_run(s);
    lh_mesh_scan_run(s);
    if (s->L && !s->failed) {
        lh_meet_run(s);
    }
    int64_t now = lh_now();
    if (s->L && !s->failed && now >= s->next_tick) {
        int64_t ms = now - s->next_tick + s->tick_ms;
        s->next_tick = now + s->tick_ms;
        lh_callback(s, tick_cb, (void *)(intptr_t)ms);
    }
    if (s->L && !s->failed) {
        lh_timers_run(s);
    }
    if (s->L && !s->failed) {
        lh_anims_run(s);
    }
    if (s->exit_req && !s->failed) {
        s->exit_req = false;
        os_launch(&os_launcher);
    }
}

// sys.tick: the RTOS sleeps in whole 10 ms ticks, so a 16 ms period would
// come out as 20. An app that set its period is woken by this one-shot timer
// at the right millisecond instead (the queue wait, rounded up, is the
// fallback). Made at the first sys.tick app and kept.
static esp_timer_handle_t s_wake;
static volatile uint32_t s_wake_gen;

static void wake_fire(void *arg)
{
    (void)arg;
    lh_ev_t ev = { .kind = LH_EV_WAKE, .gen = s_wake_gen };
    lh_post(&ev);
}

static void wake_at(lh_t *s, int64_t ms)
{
    if (!s_wake) {
        return;                             // made at boot (lua_host_init)
    }
    s_wake_gen = s->gen;
    esp_timer_stop(s_wake);
    esp_timer_start_once(s_wake, (uint64_t)ms * 1000);
}

static TickType_t wait_ticks(lh_t *s)
{
    int64_t wake = s->next_tick, t = lh_timers_next(s), now = lh_now();
    if (t < wake) {
        wake = t;
    }
    t = lh_anims_next(s);
    if (t < wake) {
        wake = t;
    }
    if (s->store_dirty && s->store_saved + LH_STORAGE_SAVE_MS < wake) {
        wake = s->store_saved + LH_STORAGE_SAVE_MS;
    }
    int64_t ms = wake - now;
    if (ms <= 0) {
        return 0;
    }
    if (ms > LH_MAX_WAIT_MS) {
        ms = LH_MAX_WAIT_MS;
    }
    if (s->tick_set && wake == s->next_tick && ms % portTICK_PERIOD_MS) {
        wake_at(s, ms);
    }
    return (ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// The task

static void task_end(lh_t *s)
{
    lh_stack_leave(0);
    lh_stats = (lh_stats_t){
        .mem_peak = s->mem_peak, .mem_end = s->mem, .steps_peak = s->steps_peak,
        .stack_free = uxTaskGetStackHighWaterMark(NULL), .heap_before = s->heap_before,
        .code_size = s->code_stat.size, .code_slot = s->code_stat.slot, .code_copied = s->code_stat.copied,
        .wasm = s->wasm_stat,
    };
    strlcpy(lh_stats.id, s->pkg.id, sizeof(lh_stats.id));
    strlcpy(lh_stats.err, s->err, sizeof(lh_stats.err));
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s->finished = true;
    bool orphan = s->orphan;
    s_busy = false;
    s_task_id[0] = 0;
    xSemaphoreGive(lh_mx);
    if (orphan) {
        // The OS gave up waiting and left the session to us.
        lh_calls_free(s);
        free(s);
    } else {
        xSemaphoreGive(s_done);
    }
    vTaskDelete(NULL);
}

// An app that talks to the server: open the server connection before the
// script allocates, so the TLS handshake (~35 KB, much of it in one block)
// gets the big free blocks. Later calls reuse the connection.
static void prewarm(lh_t *s)
{
    if (!(s->pkg.perms & LH_SERVER_PERMS) || !os_api_online() || os_http_connected()) {
        return;
    }
    // The prewarm is a hello from the cloud task: wait for it to start and
    // finish (or for the connection), at most LH_PREWARM_MS.
    os_api_prewarm();
    os_cloud_info_t in;
    bool seen = false;
    for (int ms = 0; ms < LH_PREWARM_MS && !s->quit && !os_http_connected(); ms += 50) {
        os_cloud_get_info(&in);
        if (in.busy) {
            seen = true;
        } else if (seen || ms >= 500) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

lua_State *lh_new_state(lh_t *s)
{
    lua_State *L = lua_newstate(lh_alloc, s, esp_random());
    if (L) {
        *(lh_t **)lua_getextraspace(L) = s;
        lua_sethook(L, step_hook, LUA_MASKCOUNT, LH_HOOK_EVERY);
    }
    return L;
}

static void lua_task(void *arg)
{
    lh_t *s = arg;
    volatile char top;
    // The stack ends about LH_STACK below this frame (the frames above it put
    // the real end a few hundred bytes higher; the margins absorb that).
    lh_stack_enter(0, (uintptr_t)&top - LH_STACK);
    char rel[64], path[160];
    prewarm(s);
    if (s->pkg.lang == LUA_PKG_LANG_C) {
        // One WebAssembly runtime on the badge: a C service holding it
        // pauses first (lua_svc.c), at most a second or two.
        lh_svc_wamr_yield();
        for (int ms = 0; lh_svc_wamr_busy() && ms < 3000 && !s->quit; ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    // Store installs bring compiled code, run in place (lua_code.c);
    // otherwise main.lua is compiled from the card.
    bool xip = lh_code_open(s);
    s->code_stat = (lh_code_stat_t){ .size = xip ? (uint32_t)s->code.size : 0, .slot = s->code.slot,
                                     .copied = s->code.copied };
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "main.lua");
    os_data_path(path, sizeof(path), rel);
    reader_t *rd = malloc(sizeof(*rd));
    int rc = LUA_ERRMEM;
    s->L = rd ? lh_new_state(s) : NULL;
    if (s->L) {
        rd->f = xip ? NULL : fopen(path, "r");
        lua_pushcfunction(s->L, boot);
        lua_pushlightuserdata(s->L, rd);
        rc = lua_pcall(s->L, 1, 0, 0);
        if (rd->f) {
            fclose(rd->f);
        }
    }
    free(rd);
    lh_files_prepare(s);
    // How the code runs: a C app's app.aot (native, from a flash slot) or
    // app.wasm (WAMR's interpreter); a Lua app's code.bin (run in place) or
    // main.lua compiled on the badge.
    bool c = s->pkg.lang == LUA_PKG_LANG_C;
    ESP_LOGI(TAG, "%s: %s (%u bytes of %s heap, %s)", s->pkg.id, rc == LUA_OK ? "booted" : "didn't boot",
             (unsigned)s->mem, c ? "app" : "Lua",
             c ? (s->wasm_stat.aot ? "app.aot, native code run in place" : "app.wasm, interpreted")
             : xip ? "code.bin, store code run in place" : "main.lua, compiled here");

    if (lock(s)) {
        lv_obj_delete(s->loading);
        s->loading = NULL;
        if (!s->L) {
            s->failed = true;
            show_error(s, "Out of memory", "Not enough memory to start the app.");
        } else if (rc != LUA_OK) {
            fail(s, rc, NULL);
        } else {
            s->next_tick = lh_now() + s->tick_ms;
            lh_callback(s, run_main, NULL);
            if (s->L && !s->failed) {
                lh_mail_opened(s);      // opened from Mailbox on a message: on_mail gets it
            }
            run_events(s, NULL);
        }
        lvgl_port_unlock();
    }

    int64_t blocked = lh_now();
    while (s->L && !s->failed && !s->quit) {
        lh_ev_t ev;
        // A frame that takes longer than LH_TICK_MS (a C app drawing the
        // whole screen) leaves no wait between ticks: then the idle task
        // still gets one tick a second, or the task watchdog complains (seen
        // on a badge, 2.3.0).
        TickType_t wait = wait_ticks(s);
        int64_t now = lh_now();
        if (wait || now - blocked >= LH_BUSY_MAX_MS) {
            wait = wait ? wait : 1;
            blocked = now;
        }
        bool got = xQueueReceive(s_q, &ev, wait) == pdTRUE;
        if (got && ev.gen != s->gen) {
            free(ev.data);
            continue;
        }
        if (got && ev.kind == LH_EV_QUIT) {
            break;
        }
        if (lock(s)) {
            run_events(s, got ? &ev : NULL);
            lvgl_port_unlock();
        }
        if (got) {
            free(ev.data);
        }
        if (s->L) {
            lh_storage_flush(s, false);
        }
    }
    close_state(s);
    if (s->pkg.lang == LUA_PKG_LANG_C) {
        lh_svc_wamr_released();             // the runtime is free: C services it paused start again
    }
    if (s->led_used) {
        lh_led_off();
    }
    ESP_LOGI(TAG, "%s: closed (peak %u bytes, %u steps)", s->pkg.id, (unsigned)s->mem_peak,
             (unsigned)s->steps_peak);
    task_end(s);
}

// ---------------------------------------------------------------------------
// The host app (OS task)

static void host_enter(lv_obj_t *root)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lua_pkg_t pkg = s_next;
    s_next.id[0] = 0;
    bool busy = s_busy;
    xSemaphoreGive(lh_mx);

    lv_obj_t *area = lv_obj_create(root);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, OS_ROOT_W, OS_ROOT_H);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(area, ui_col(COL_BG), 0);
    lv_obj_set_scrollable(area, false);

    lh_t *s = pkg.id[0] && !busy ? calloc(1, sizeof(*s)) : NULL;
    if (!s) {
        lv_obj_t *l = ui_label(area, &sol_font_14, COL_DIM,
                               !pkg.id[0] ? "Open apps from the Apps library." : busy ? "The last app is still closing."
                               : "Not enough memory to start the app.");
        lv_obj_center(l);
        return;
    }
    s->pkg = pkg;
    s->gen = ++s_gen;
    s->t0 = lh_now();
    s->area = area;
    s->modal_ref = s->store_ref = s->scan_ref = s->svc_start_ref = LUA_NOREF;
    s->steps_max = LUA_HOST_STEPS_MAX;
    strlcpy(s->tag, pkg.id, sizeof(s->tag));
    s->tick_ms = LH_TICK_MS;
    lh_ble_init(s);
    s->loading = ui_label(area, &sol_font_14, COL_DIM, "Starting...");
    lv_obj_center(s->loading);
    s->heap_before = heap_caps_get_free_size(OS_MEM_SRAM);
    s->psram = os_mem_psram();
    s->heap_max = lua_host_heap_max();
    xSemaphoreTake(s_done, 0);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_cur = s;
    s_busy = true;
    strlcpy(s_task_id, pkg.id, sizeof(s_task_id));
    xSemaphoreGive(lh_mx);
    if (pkg.perms & LUA_PERM_MESH) {
        lh_mesh_start(s);
    }
    if (xTaskCreate(lua_task, "lua", LH_STACK, s, LH_PRIO, &s->task) != pdPASS) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        s_busy = false;
        s_task_id[0] = 0;
        s->finished = true;
        xSemaphoreGive(lh_mx);
        s->failed = true;
        lv_obj_delete(s->loading);
        s->loading = NULL;
        show_error(s, "Out of memory", "Not enough memory to start the app.");
    }
}

static void host_exit(void)
{
    lh_t *s = lh_cur;
    if (!s) {
        return;
    }
    s->quit = true;
    lh_ev_t ev = { .kind = LH_EV_QUIT, .gen = s->gen };
    xQueueSendToFront(s_q, &ev, pdMS_TO_TICKS(50));
    bool done = s->finished || xSemaphoreTake(s_done, pdMS_TO_TICKS(LH_EXIT_WAIT_MS)) == pdTRUE;
    lh_net_stop(s);
    lh_ble_stop(s);
    lh_ui_release(s);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_cur = NULL;
    done = done || s->finished;
    if (!done) {
        s->orphan = true;
        ESP_LOGW(TAG, "%s: still running after %d ms; it will clean up when it stops", s->pkg.id, LH_EXIT_WAIT_MS);
    }
    xSemaphoreGive(lh_mx);
    if (done) {
        lh_calls_free(s);
        free(s);
    }
    lh_led_off();
}

static bool host_key(os_key_t key, bool repeat)
{
    lh_t *s = lh_cur;
    if (!s || s->failed || s->finished) {
        return key != OS_KEY_BACK;      // error screen: CANCEL exits
    }
    lh_ev_t ev = { .kind = LH_EV_KEY, .key = key, .repeat = repeat, .gen = s->gen };
    lh_post(&ev);
    return true;
}

const os_app_t lua_host_app = {
    .id = "lua",
    .name = s_title,
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_LPURPLE,
    .on_enter = host_enter,
    .on_exit = host_exit,
    .on_key = host_key,
};

bool lua_host_launch(const char *id)
{
    lua_pkg_t pkg;
    const char *why = NULL;
    if (!os_data_ready()) {
        why = "Insert a microSD card";
    } else if (lua_install_busy(id)) {
        why = "That app is still installing";
    } else if (!lua_pkg_load(id, &pkg)) {
        why = "That app isn't installed";
    } else if (lua_pkg_expired(&pkg)) {
        why = "This app has expired";
    }
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    if (!why && s_busy && !lh_cur) {
        why = "The last app is still closing";
    }
    if (!why) {
        s_next = pkg;
        strlcpy(s_title, pkg.name, sizeof(s_title));
    }
    xSemaphoreGive(lh_mx);
    if (why) {
        os_toast("%s", why);
        printf("lua: %s: %s\n", id, why);
        return false;
    }
    if (os_current() == &lua_host_app) {
        os_launch(&os_launcher);        // re-entering the host needs a switch
    }
    os_launch(&lua_host_app);
    return true;
}

const char *lua_host_running(void)
{
    lh_t *s = lh_cur;
    return s && !s->failed ? s->pkg.id : NULL;
}

bool lua_host_active(const char *id)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    bool active = s_busy && id && !strcmp(s_task_id, id);
    xSemaphoreGive(lh_mx);
    return active;
}

void lua_host_init(void)
{
    lh_mx = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_q = xQueueCreate(LH_QUEUE_LEN, sizeof(lh_ev_t));
    // sys.tick's wake-up timer: made here, not at the first app that asks, so it
    // never lands in the middle of the heap an app left behind (CLAUDE.md rule 7).
    const esp_timer_create_args_t wake = { .callback = wake_fire, .name = "lhwake" };
    if (esp_timer_create(&wake, &s_wake) != ESP_OK) {
        s_wake = NULL;
    }
    lua_install_init();
    lh_cmd_register();
    lua_svc_init();
    if (lvgl_port_lock(0)) {
        lh_img_init();
        lvgl_port_unlock();
    }
}
