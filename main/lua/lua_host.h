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

// Registers the "lua" console command. Call before console_start().
void lua_host_init(void);
