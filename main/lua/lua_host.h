#pragma once

// Lua app host: runs one installed store app (docs/LUA_API.md) in a sandbox.
//
// The script gets a fresh lua_State per launch with a capped allocator, an
// instruction budget per callback and only the globals LUA_API.md lists. It
// runs on its own "lua" task, not the OS task: Lua and the LVGL calls under
// it need more stack than the OS task has, and a stuck script must not stall
// the OS. The task takes the LVGL lock around every callback, so callbacks
// never overlap with each other or with the OS.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/os.h"

// Lua heap per app (LUA_API.md "Limits"). With PSRAM (2.0.0; every badge so
// far has 8 MB, core/os_mem.h) an app's whole Lua heap lives there, up to
// LUA_HOST_HEAP_MAX, and never takes PSRAM below LUA_HOST_PSRAM_RESERVE: the
// system's share while it runs (TLS, Wi-Fi and lwIP buffers, LVGL's big
// layers, downloads; ~60 KB at peak, measured). The cap counts what Lua asks
// for; the allocator's own overhead on many small blocks (~25 %) comes out of
// the rest. Its other memory (12 KB task stack, host state, LVGL objects)
// stays in SRAM, bounded by the object and text caps (lua_priv.h).
//
// Without PSRAM it's 1.x's (0.5.13): 96 KB in SRAM, never taking the SRAM
// below LUA_HOST_SYS_RESERVE; then which limit an app meets first depends on
// its UI (tests/t-heap).
#define LUA_HOST_HEAP_MAX       (6 * 1024 * 1024)
#define LUA_HOST_PSRAM_RESERVE  (512 * 1024)
#define LUA_HOST_HEAP_MAX_SRAM  (96 * 1024)
#define LUA_HOST_SYS_RESERVE    (32 * 1024)
#define LUA_HOST_STEPS_MAX      200000      // VM instructions per callback
// A C app's budget per callback in WAMR's interpreter (2.3.0): wasm
// instructions, counted across nested calls (a callback the app gets inside
// sos_call shares it). On a badge the classic interpreter runs 2 200 to
// 2 900 of them a millisecond, so this is under a second: room for the
// emulator to run what a badge runs natively at 15 frames a second. Native
// code (app.aot) is limited by time instead: LW_AOT_CALLBACK_MS (lua_aot.h).
// Host work an API call does (fs reads, crypto, canvas drawing) still counts
// against LUA_HOST_STEPS_MAX, as for a Lua app.
#define LUA_HOST_WASM_STEPS_MAX 2000000

// This badge's cap: LUA_HOST_HEAP_MAX with PSRAM, else LUA_HOST_HEAP_MAX_SRAM.
size_t lua_host_heap_max(void);

// Hidden system app; its title is the running script's name.
extern const os_app_t lua_host_app;

// Start an installed app (any task). When it can't start (no card, not
// installed, expired, the previous script still shutting down) it toasts
// why and returns false.
bool lua_host_launch(const char *id);

// Id of the running app, or NULL.
const char *lua_host_running(void);

// A Lua task for app <id> exists: running, showing its error screen or still
// closing. Its files are in use until this turns false. Any task.
bool lua_host_active(const char *id);

// Mailbox (2.3.0, game/mailbox.h): mail `id` for app `app` arrived. When that
// app is running (and has the "mail" permission) its on_mail gets it. Any task.
void lua_host_mail(const char *app, uint32_t id);
// Launch `app` from the Mailbox on its mail `id`: on_mail gets it once the
// top-level chunk ran. false (and a toast) when it can't start. OS task.
bool lua_host_open_mail(const char *app, uint32_t id);
// ... and for the app's running service (2.3.0, lua_svc_call): its on_mail
// gets the mail as mail.get gives it, if the app asks for "mail". Any task.
void lua_host_mail_service(const char *app, uint32_t id);

// Registers the "lua" console command and the app services' provider
// (os_service_set_provider). Call before os_service_boot().
void lua_host_init(void);

// ---------------------------------------------------------------------------
// Background services (2.3.0, docs/LUA_API.md "service", lua_svc.c): an app
// with the `background` permission leaves service.lua (a C app: a second
// instance at on_service_start) running in a state of its own on the "svc"
// task after it closes. The owner sees and stops them in Services
// (core/os_service.h: "app:<app id>").

#define LUA_SVC_MAX         3               // app services running at once
#define LUA_SVC_HEAP_MAX    (1024 * 1024)   // each one's heap, in PSRAM
#define LUA_SVC_STEPS_MAX   100000          // VM instructions per callback
#define LUA_SVC_STEPS_MIN   2000000         // ... and a minute, all callbacks together
#define LUA_SVC_MSG_MAX     1024            // service.send / lua_host_call JSON

// The hook for built-in code (Mailbox's on_mail, ...). From any task: queue
// a call of the global `fn` (^on_[a-z0-9_]{1,24}$) with one argument, the
// decoded `json` (a JSON value, ≤ LUA_SVC_MSG_MAX bytes; NULL = nil), for the
// running service of app `app` and for the app itself if it's on screen.
// Each side gets it as its own event (dropped when its queue is full) and
// calls fn only if it defines it; a C app or service needs an export of
// that name, (void) -> void, which reads the value as badge_arg(1). true if
// at least one side queued it. Nothing waits: the caller's task never runs
// Lua.
bool lua_host_call(const char *app, const char *fn, const char *json);
// The same for the app's running service only, and only when its app.json
// asks for every permission in need_perms (lua_perm_t bits; 0 = none). For a
// caller that reaches the app on screen its own way (Mailbox: lua_host_mail).
bool lua_svc_call(const char *app, const char *fn, const char *json, uint32_t need_perms);

// A service state of app <id> exists (starting, running or stopping): its
// files are in use. Any task.
bool lua_svc_active(const char *id);

// The installer, around swapping an app's folder (worker task, may block up
// to ~2 s): pause stops the app's service and waits until its state is gone
// (true if it was running); resume starts it again if it was, the new
// version still asks for `background` and the owner allowed it.
bool lua_svc_pause(const char *id);
void lua_svc_resume(const char *id, bool was_running);

// The app is being removed (or its quest app expired): stop its service,
// wait until its state is gone (≤ ~2 s), and forget the owner's answer.
// Worker tasks only (blocks); a no-op for apps without a service.
void lua_svc_forget(const char *id);
