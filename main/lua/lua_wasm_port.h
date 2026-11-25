#pragma once

// lua_wasm_port.c's one setting: where WAMR's memory comes from. The running
// C app's counted heap; realloc semantics (nsize 0 frees, NULL refuses).

#include <stdbool.h>
#include <stddef.h>

typedef void *(*lw_port_realloc_fn)(void *ud, void *ptr, size_t osize, size_t nsize);

// Before wasm_runtime_full_init; NULL after wasm_runtime_destroy.
void lw_port_heap(lw_port_realloc_fn fn, void *ud);
// lua_wasm.c: around WAMR's slow work for the app (growing linear memory,
// which may copy megabytes): the badge's time, not native time. begin
// answers false once the app must stop (a limit or the ceiling passed): the
// growth is refused then (memory.grow answers -1), so a loop of growths
// can't outlast the stop.
bool lw_port_slow(bool begin);
