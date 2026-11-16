#pragma once

// Internals of the C app bridge, shared by lua_wasm.c (the "badge" imports)
// and lua_wasi.c (the few "wasi_snapshot_preview1" imports wasi-libc needs).
// The host's side is lw_boot()/lw_close() in lua_priv.h.

#include <stdbool.h>
#include <stdint.h>
#include "wasm_export.h"
#include "lua_aot.h"
#include "lua_priv.h"

#define LW_FRAME_MAX    64          // values a frame holds (badge_abi.h)
#define LW_LINE_MAX     200         // stdout/stderr bytes kept until their newline

// Host steps the bridge charges (LUA_HOST_STEPS_MAX per callback, the budget
// the Lua API's own work counts against too), so what an import does for the
// app is bounded per callback like the app's own code. A step is about what
// a Lua VM instruction takes on a badge (~0.33 us). Measured on a badge
// (Solana OS 2.3.0): memmove in PSRAM 8 KB/ms, esp_fill_random 520 B/ms, the
// console (UART, 115200 baud, blocking) 11 B/ms.
#define LW_IMPORT_STEPS 1           // every import: an import loop costs as a Lua loop does
#define LW_CALL_STEPS   4           // sos_call on top: a Lua call's own instructions
#define LW_COPY_BYTES   4           // bytes copied in or out per step (strings, keys): ~0.5 us
#define LW_OUT_STEPS    LH_OUT_STEPS    // steps per byte written to the console (lua_priv.h)
#define LW_RANDOM_STEPS 6           // steps per random byte (random_get): ~1.9 us a byte
#define LW_WRITE_IOVS   4           // fd_write takes at most this many buffers a call ...
#define LW_WRITE_MAX    1024        // ... and this many bytes (wasi-libc buffers 1 KB): libc writes the rest with another call
#define LW_RANDOM_MAX   4096        // random_get: more in one call is refused (EINVAL)

enum { LW_ERR, LW_MEM, LW_BUDGET };     // why the app stops (lw_t.kind)

typedef struct lw {
    lh_t *s;
    uint8_t *bin;                   // app.wasm / app.aot as loaded (WAMR points into it)
    size_t bin_size;
    bool aot;                       // native: bin is xip's mapping, not the app's heap
    lua_xip_t xip;                  // app.aot's flash slot, mapped until WAMR unloads it
    lw_clock_t clk;                 // native time in this callback (lua_aot.h)
    int imports;                    // imports now open (native: the clock stops in them)
    bool rt;                        // WAMR is up
    bool claimed;                   // this one holds the runtime (lw_busy)
    wasm_module_t mod;
    wasm_module_inst_t inst;
    wasm_exec_env_t env;
    // The running call: the Lua state whose frame the imports work on.
    lua_State *L;
    int depth;                      // calls into the app now open (nested through sos_call)
    uint32_t seq;                   // the callback (lh_t.cb_seq) the budget below belongs to
    int left;                       // wasm instructions left in it
    // Stopping: the app can't go on (fatal, with kind and msg), or proc_exit
    // ended it like sys.exit() (exiting). Either way every call into the app
    // unwinds and no import does anything more.
    bool fatal, exiting;
    uint8_t kind;
    char msg[160];
    // sos_ref: a table in the registry, slots 1..LW_REFS_MAX, and which are live.
    int refs_ref;
    int nrefs;
    uint32_t refs_live[1024 / 32];
    // stdout and stderr, a line at a time
    char line[LW_LINE_MAX];
    size_t line_len;
    uint32_t frames, frames_direct; // ui_frame calls that drew, and how many went without a copy
    NativeSymbol *natives;          // the import tables WAMR holds (and sorts) while it runs
    uint32_t n_natives;
    // Exports the host calls (NULL: the app didn't define it)
    wasm_function_inst_t f_start, f_key, f_tick, f_exit, f_mesh, f_meet, f_mail;
} lw_t;

// The running C app or C service (one at a time: one WebAssembly runtime
// on the badge, 2.3.0), NULL between them.
extern lw_t *lw_cur;

// Who holds the runtime (any task): anyone, a service. lw_boot claims it and
// raises when it's taken; lw_close gives it back.
bool lw_busy(void);
bool lw_busy_svc(void);

// LH_EV_CALL for a C app or service (lua_svc.c): the export `fn`, (void) ->
// void, with the value on top as its frame's argument 1. Nothing when the
// module doesn't export it.
void lw_call_named(lua_State *L, lh_t *s, const char *fn);

// Stops the app: msg goes to the error screen (kind LW_ERR/MEM/BUDGET),
// and a WAMR exception makes the interpreter unwind. The first reason wins.
void lw_trap(lw_t *w, uint8_t kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
// proc_exit: the app ends as with sys.exit().
void lw_exit(lw_t *w);
// The app's memory at addr, len bytes, or NULL (and the app stops) when that
// isn't all inside it. fn names the import for the message.
void *lw_mem(lw_t *w, const char *fn, uint32_t addr, uint32_t len);
// The same check for a native pointer WAMR converted from a '*' parameter
// (checked for one byte only): p, or NULL and the app stops.
void *lw_native(lw_t *w, const char *fn, void *p, uint32_t len);
// Around an import WAMR calls straight (lua_wasi.c): the running app, or NULL
// when it must stop (the import then does nothing); out takes what in gave.
// Native time stops in between, and past its limit the app stops.
lw_t *lw_import_in(void);
void lw_import_out(lw_t *w);
// Charges n host steps to the running callback. false: over the budget, and
// the app stops (lw_trap: "Over 200000 host steps in one callback ...").
bool lw_charge(lw_t *w, uint32_t n);
// stdout / stderr bytes; lines go to the console as print's do.
void lw_out(lw_t *w, const char *p, size_t len);
void lw_out_flush(lw_t *w);

// lua_wasi.c: its import table (const: lua_wasm.c registers a copy). native:
// plain natives with WAMR's libc-wasi signatures, which app.aot calls
// straight; else raw natives for the interpreter (see lua_wasi.c).
const NativeSymbol *lw_wasi_symbols(bool native, uint32_t *n);
