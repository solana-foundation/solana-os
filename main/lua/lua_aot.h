#pragma once

// The seam between C apps and native code (WAMR's ahead-of-time form,
// app.aot). The store compiles each C app's wasm with wamrc for one target
// (sdk/c/toolchain/wamrcflags) and signs the result; a badge runs that native
// form only when the target is its own label, this seam says it can, and the
// signature verifies. Everything else about an app (the bridge, the limits,
// the error screen) is the same for app.aot and app.wasm.
//
// Two halves. What decides whether native code may run (the store's key,
// its cache, the signed message, the Ed25519 check, the target and slot
// checks, the launch's decision) is portable: lua_aot_trust.c, in both
// builds, so the emulator checks every signature too. What runs it is the
// badge's: lua_aot.c (SOLANA_OS_DEVICE_SRCS), with WAMR's AOT runtime
// compiled in (SOLANA_OS_WAMR_AOT_SRCS, WASM_ENABLE_AOT=1 in
// main/CMakeLists.txt) and the watchdog. The code runs in place from a flash
// slot (lua_xip.c, LUA_XIP_EXEC), mapped until the module is unloaded.
// Emulator: platform/lua_aot_emu.c says it can't run native code, and the
// emulator's WAMR has no AOT runtime: it checks the signature, never
// downloads app.aot, and always interprets the wasm.
//
// Trust. app.aot is native code and the card can be edited on a computer, so
// a matching SHA-256 in app.json isn't enough: the store signs
// "sos-aot1|<target>|<sha256 hex>" with Ed25519 (server/API.md "C apps"),
// app.json carries the signature, and the badge checks it against the
// store's public key (GET /api/device/aotkey, kept in NVS "cloud"/"aotkey",
// dropped with the server identity) at install and again at every launch,
// before a byte of the file is mapped. The slot's bytes are then hashed
// through the mapping (lua_xip.c), so what runs is what was signed.
//
// Stopping. Native code has no instruction count: each callback gets
// LW_AOT_CALLBACK_MS of native time (time in the badge's own functions, which
// have their own limits, doesn't count; the bulk memory helpers native code
// calls, lw_aot_memset/lw_aot_memmove, do). A watchdog (an esp_timer, every
// 10 ms) notices the overrun, sets lw_clock_t.kill (every import and every
// piece of a bulk memory operation checks it) and, when the app's task stands
// in the module's code, points its saved program counter at a trampoline
// that longjmps to the innermost call into the module (lua_wasm.c enter()).
// Nothing but the module's code and WAMR's own call frames lies between: see
// lua_wasm.c. When the task stands elsewhere (a soft-float helper), the
// watchdog tries again after a random 0.25 to 1.25 ms until it lands.
//
// The ceiling. Whatever a C app does, native or interpreted, one callback
// ends after LW_CALLBACK_MAX_MS of wall-clock time: the limits above stop
// honest overruns long before, so this is the backstop for anything they
// miss. The watchdog then asks for the stop every way that is safe: kill (for
// imports and the bulk helpers), the program counter (native code), and the
// interpreter's remaining instruction count set to 0 (it traps at its next
// check: after an import, which hands the count back). Imports check the
// ceiling themselves too (lua_wasm.c), which is all the emulator, with no
// watchdog, has.

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_pkg.h"

// This build's AOT target label (LW_AOT_TARGET, a compile definition): the
// store's manifest names the target its app.aot was compiled for ("aot":
// {"target": ...}), and only an exact match is taken. Both builds read it
// from AOT_TARGET in firmware/sdk/c/toolchain/versions (main/sources.cmake),
// the label of the wamrc flags the store uses, so it can't drift.
#ifndef LW_AOT_TARGET
#error "LW_AOT_TARGET comes from sdk/c/toolchain/versions (main/sources.cmake)"
#endif

// Native time per callback, shared by the nested calls of one callback. Wall
// time while native code runs, including any moment a higher-priority task
// (Wi-Fi, the timer task) preempts it: FreeRTOS keeps no per-task run time
// in this build (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is off), and those
// moments are short next to 200 ms.
#define LW_AOT_CALLBACK_MS  200
// Wall-clock time per callback of a C app, whatever it does (the ceiling).
#define LW_CALLBACK_MAX_MS  3000
// A C service's (2.3.0, C_API.md "service"): native time and interpreted
// instructions per callback, and what a minute of either counts as against
// LUA_SVC_STEPS_MIN (700 ms of native time a minute ~ 2 000 000).
#define LW_SVC_CALLBACK_MS  30
#define LW_SVC_WASM_STEPS   250000
#define LW_SVC_STEPS_PER_US 3
// What the store signs: "sos-aot1|" LW_AOT_TARGET "|" <64 lowercase hex>.
#define LW_AOT_SIG_PREFIX   "sos-aot1|"

// ---------------------------------------------------------------------------
// Badge only (lua_aot.c; the emulator's platform/lua_aot_emu.c says no)

// The badge can run app.aot: executable flash mappings and a way to stop a
// stuck native loop. While false the installer fetches app.wasm only and the
// host never loads app.aot.
bool lw_aot_usable(void);

// ---------------------------------------------------------------------------
// Portable (lua_aot_trust.c, both builds)

// The store's public key, cached in NVS ("cloud"/"aotkey", 64 hex digits).
// false when there is none.
bool lw_aot_key(uint8_t key[32]);
// Saves a key the server sent (64 hex digits); false if it isn't one.
bool lw_aot_key_save(const char *hex);
// An answer to GET /api/device/aotkey ({"key": "<64 hex>"}): caches the key.
// false (and the cached key, if any, kept) when it carries none. Small NVS
// write: fine on the OS task, where os_api's callbacks run.
// gen is os_cloud_server_gen() from when the request was sent: the key is
// the server's that was asked, so after `server <url>` an answer still on
// its way is dropped (false) instead of putting the old store's key back.
bool lw_aot_key_answer(int status, const char *body, uint32_t gen);

// The store's signature of app.aot with this SHA-256, for this build's
// target, verifies under key (Ed25519, verify_strict). About 58 ms on a badge.
bool lw_aot_sig_ok(const uint8_t key[32], const uint8_t sha[32], const uint8_t sig[64]);

// Whether an installed C app's app.aot may run now, checked in this order:
// the manifest names this build's target with a signature (pkg->aot), the
// badge can run native code, app.aot is on the card with the manifest's
// size, it fits a flash slot, the store's key is cached, the signature
// verifies. NULL: it may, with app.aot's full path in path, its size and the
// signature check's time (sig_us, may be NULL); else why not ("" when the
// store sent no native code for this badge: nothing to say). Reads the card
// and verifies (~58 ms): the app's task or the console, not the OS task.
const char *lw_aot_refusal(const lua_pkg_t *pkg, char *path, size_t n, long *size, uint32_t *sig_us);

// lw_aot_refusal's answer is about the key: none cached, or a signature it
// doesn't verify (the store may have rotated its key since it was cached).
bool lw_aot_refusal_is_key(const char *why);

// After such a launch (the app runs app.wasm meanwhile), the key is asked of
// the server again in the background: once per boot, only when online (an
// offline launch leaves the ask to a later one), never in a loop. The answer
// replaces the cached key, so the next launch can verify. Any task.
void lw_aot_key_refresh(void);
enum { LW_KEY_REFRESH_NONE, LW_KEY_REFRESH_ASKED, LW_KEY_REFRESH_NEW, LW_KEY_REFRESH_SAME, LW_KEY_REFRESH_FAILED };
// Where this boot's refresh stands (LW_KEY_REFRESH_*), and when the answer
// came (esp_timer us; at_us may be NULL), for `lua aot`.
uint8_t lw_aot_key_refresh_state(int64_t *at_us);

// The running callback's clock (lua_wasm.c keeps it; the watchdog reads it
// from its own task). Times are esp_timer microseconds, cut to 32 bits: only
// differences are used.
typedef struct {
    volatile uint32_t used_us;      // native time in this callback before since_us
    volatile uint32_t since_us;     // when native code last started running
    volatile uint32_t cb_us;        // when this callback first called into the module (the ceiling)
    volatile bool open;             // a call into the module is open (both forms)
    volatile bool running;          // in the module (not in an import), a callback open (native)
    volatile bool kill;             // over a limit: every import and bulk helper stops the app
    volatile bool ceiling;          // ... and it was the ceiling (set before kill)
    volatile bool aimed;            // the trampoline is set: nothing more to do
    volatile uint8_t tries;         // watchdog passes after the limit (for the log)
    volatile uint32_t aimed_pc, aimed_us;   // where the stop was aimed, and when (for the log)
    jmp_buf *volatile jb;           // the innermost call into the module's (native)
    const uint8_t *code_lo, *code_hi;   // the module's mapping (native)
    bool aot;                       // native code; else the interpreter, with env
    void *env;                      // the interpreter's wasm_exec_env_t
    TaskHandle_t task;              // the app's task
    uint32_t limit_ms;              // native time per callback: LW_AOT_CALLBACK_MS, a service's LW_SVC_CALLBACK_MS
} lw_clock_t;

// From loading the module to closing it (lua_wasm.c): the watchdog runs
// while armed, for both forms. Badge only (lua_aot.c): an esp_timer;
// emulator: nothing (it runs no native code, and its imports check the
// ceiling).
void lw_aot_watch(lw_clock_t *c);
void lw_aot_unwatch(void);

// A native module (app.aot, loaded) declares a start function. Badge only
// (lua_aot.c); the emulator loads no native code (false).
bool lw_aot_has_start(void *module);
