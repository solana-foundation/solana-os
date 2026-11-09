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

// Lua heap per app (LUA_API.md "Limits"; 48 KB before 0.5.13), and the system
// heap an app may never take: the OS, mesh and cloud tasks' share while it
// runs. Measured on 0.5.13 with Wi-Fi joined: an app with one label reaches
// ~103 KB of Lua heap before the reserve stops it (tests/t-heap).
#define LUA_HOST_HEAP_MAX    (96 * 1024)
#define LUA_HOST_SYS_RESERVE (32 * 1024)
#define LUA_HOST_STEPS_MAX  200000          // VM instructions per callback

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
