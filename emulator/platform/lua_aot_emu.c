// The emulator's side of the AOT seam (lua/lua_aot.h): no native code here.
// The emulator's WAMR has no AOT runtime (WASM_ENABLE_AOT 0), so every C app
// runs its app.wasm in the interpreter, and the installer never downloads
// app.aot. The trust checks (the store's key, its cache, the signature) are
// portable (lua/lua_aot_trust.c) and run here as on a badge. No watchdog
// either (tasks are cooperative here): the interpreter's instruction count,
// the bulk memory charge and the imports' own ceiling check stop an app.

#include "lua/lua_aot.h"

bool lw_aot_usable(void)
{
    return false;
}

void lw_aot_watch(lw_clock_t *c)
{
    (void)c;
}

void lw_aot_unwatch(void)
{
}

bool lw_aot_has_start(void *module)
{
    (void)module;
    return false;
}
