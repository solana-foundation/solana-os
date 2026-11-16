// The badge's side of the AOT seam (lua_aot.h): running native C apps. What
// decides whether one may run (key, signature, target, slot) is portable:
// lua_aot_trust.c.
//
// Where the code runs. app.aot is built for execute-in-place (wamrc --xip:
// no relocations in the code, helpers called through a table), copied into
// a flash slot and mapped for instruction fetch (lua_xip.c, LUA_XIP_EXEC).
// Run-time mappings start above the PSRAM heap's region, which the PMP marks
// readable and writable but not executable; above it the C5's PMA lets the
// CPU execute (measured: PMP entry 10 ends at the start of the mapping
// window). So nothing in RAM ever becomes executable, no sdkconfig changes,
// and the flash driver's own cache invalidation after the slot is written is
// all the cache maintenance there is (write, then map). WAMR's loader is
// handed the mapping with wasm_binary_freeable false (lua_wasm.c): data
// segments and names keep pointing into it until wasm_runtime_unload, and
// the mapping goes after that. A non-XIP file would ask WAMR for executable
// memory (os_mmap with MMAP_PROT_EXEC), which lua_wasm_port.c refuses: it
// doesn't load.
//
// Stopping a stuck loop ("mechanism A" of the hardware study). An esp_timer
// (its task runs at priority 22, the app's "lua" task at 2, on the C5's one
// core) checks the running callback's native time every WATCH_MS. Past
// LW_AOT_CALLBACK_MS it sets kill (the bridge's imports check it on entry and
// return, the bulk helpers below between pieces), and if the app's task is
// preempted (Ready, not blocked) with its saved program counter inside the
// module's mapping, it rewrites that saved PC to aim at kill_trampoline and
// lets the task go on: the task "returns" from the interrupt into the
// trampoline, which longjmps to the innermost enter() of lua_wasm.c. When the
// PC is elsewhere (a soft-float helper, WAMR's runtime, the badge's own code)
// nothing is touched and the watchdog tries again after a random 0.25 to
// 1.25 ms (random, so it can't keep sampling a loop at the same point): a
// helper returns to the module within microseconds, and an import stops the
// app on its own meanwhile. The task is suspended only around reading and
// writing its saved context.
//
// The same timer keeps the ceiling (LW_CALLBACK_MAX_MS of wall-clock time per
// callback) for both forms: for the interpreter it sets kill and the
// remaining instruction count to 0, again at every pass until the app stops.
//
// Bulk memory. wamrc's XIP code calls memory.fill and memory.copy through the
// "memset", "memmove" and "memcpy" entries of the native symbol table
// (aot_reloc.h: WAMR's aot_memset/aot_memmove) and memory.init through
// aot_memory_init, which uses aot_memmove: a Solana OS change in
// aot_runtime.c sends all of them here (wamr/README.md). They run in pieces
// of BULK_PIECE bytes and look at kill before each: once it is set they leave
// through the innermost enter()'s setjmp, as the trampoline does. They are
// called straight from module code on the app's task, so the jump skips only
// the module's frames, WAMR's aot_memset/aot_memmove/aot_memory_init (which
// hold nothing: no shared memory, no locks) and WAMR's call path. Their time
// is native time: the clock runs while they do.

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "riscv/rvruntime-frames.h"
#include "wasm_export.h"
#include "aot_runtime.h"
#include "lua_aot.h"

static const char *TAG = "lua_aot";

#define WATCH_MS    10
#define RETRY_US    250             // + up to 1 ms, random: the next try once a stop is asked
#define BULK_PIECE  4096            // bytes per piece of a bulk memory operation (~50 us in PSRAM)

static esp_timer_handle_t s_timer;
static lw_clock_t *volatile s_clock;

static void watch(void *arg);

// The watchdog's timer is made the first time it's asked for and kept: without
// it a stuck native loop couldn't be stopped, so native code doesn't run.
bool lw_aot_usable(void)
{
    if (!s_timer) {
        const esp_timer_create_args_t args = { .callback = watch, .name = "aotdog" };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;
            ESP_LOGE(TAG, "no watchdog timer: app.wasm only");
        }
    }
    return s_timer != NULL;
}

// ---------------------------------------------------------------------------
// The watchdog

// Runs on the app's task in place of the native instruction it was
// preempted at: the stack below is the module's frames and WAMR's.
static void __attribute__((noreturn, noinline)) kill_trampoline(void)
{
    longjmp(*s_clock->jb, 1);
}

// Native code: aim the app's task at the trampoline if it stands in the
// module's code. true: aimed.
static bool aim(lw_clock_t *c, uint32_t used)
{
    // Only a preempted task: suspending and resuming a blocked one would cut
    // its wait short. Native code never blocks; the badge's code may.
    if (eTaskGetState(c->task) != eReady || !c->jb) {
        return false;
    }
    vTaskSuspend(c->task);
    RvExcFrame *f = *(RvExcFrame **)c->task;    // pxTopOfStack: the context saved when it was preempted
    uint32_t pc = (uint32_t)f->mepc;
    if (c->open && pc >= (uint32_t)c->code_lo && pc < (uint32_t)c->code_hi) {
        f->mepc = (long)(uintptr_t)kill_trampoline;
        c->aimed_pc = pc;
        c->aimed_us = used;
        c->aimed = true;
    }
    vTaskResume(c->task);
    return c->aimed;
}

static void watch(void *arg)
{
    (void)arg;
    lw_clock_t *c = s_clock;
    bool soon = false;
    if (c && c->open && !c->aimed) {
        uint32_t now = (uint32_t)esp_timer_get_time();
        uint32_t used = c->used_us + (c->running ? now - c->since_us : 0);
        bool over = c->aot && used > (c->limit_ms ? c->limit_ms : LW_AOT_CALLBACK_MS) * 1000u;
        bool ceiling = now - c->cb_us > LW_CALLBACK_MAX_MS * 1000u;
        if (over || ceiling) {
            if (!c->kill) {
                c->ceiling = !over;         // the first reason is the one shown
            }
            c->kill = true;
            if (c->tries < 255) {
                c->tries++;
            }
            if (c->aot) {
                soon = !aim(c, used);
            } else {
                // The interpreter reads its count back after every import;
                // between them it may write its own over this: so every pass.
                wasm_runtime_set_instruction_count_limit((wasm_exec_env_t)c->env, 0);
                c->aimed_us = now - c->cb_us;
                soon = true;
            }
        }
    }
    // No logging here: the app's task may hold stdout's lock. lua_wasm.c
    // prints what happened once it has landed.
    if (c) {
        esp_timer_start_once(s_timer, soon ? RETRY_US + esp_random() % 1000 : WATCH_MS * 1000);
    }
}

void lw_aot_watch(lw_clock_t *c)
{
    if (lw_aot_usable()) {
        s_clock = c;
        esp_timer_start_once(s_timer, WATCH_MS * 1000);
    }
}

void lw_aot_unwatch(void)
{
    s_clock = NULL;
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
}

bool lw_aot_has_start(void *module)
{
    const AOTModule *m = module;
    return m && m->module_type == Wasm_Module_AoT && m->start_func_index != (uint32)-1;
}

// ---------------------------------------------------------------------------
// Bulk memory for native code (see the top)

// Leaves through the innermost enter() once the app must stop. Only ever
// called from module code (WAMR's aot_memset, aot_memmove, aot_memory_init).
static inline void bulk_check(void)
{
    lw_clock_t *c = s_clock;
    if (c && c->kill && c->jb) {
        if (!c->aimed) {
            c->aimed_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
            c->aimed_us = c->used_us + (c->running ? (uint32_t)esp_timer_get_time() - c->since_us : 0);
            c->aimed = true;
        }
        longjmp(*c->jb, 1);
    }
}

void *lw_aot_memset(void *s, int v, size_t n)
{
    uint8_t *p = s;
    do {
        bulk_check();
        size_t k = n < BULK_PIECE ? n : BULK_PIECE;
        memset(p, v, k);
        p += k;
        n -= k;
    } while (n);
    return s;
}

void *lw_aot_memmove(void *dest, const void *src, size_t n)
{
    uint8_t *d = dest;
    const uint8_t *sp = src;
    if (d == sp || !n) {
        bulk_check();
        return dest;
    }
    // Overlapping ranges move in the direction memmove would: from the end
    // when the destination is above the source.
    bool down = d > sp && d < sp + n;
    size_t done = 0;
    while (done < n) {
        bulk_check();
        size_t k = n - done < BULK_PIECE ? n - done : BULK_PIECE;
        size_t at = down ? n - done - k : done;
        memmove(d + at, sp + at, k);
        done += k;
    }
    return dest;
}
