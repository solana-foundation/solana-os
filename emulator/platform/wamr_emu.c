// WAMR's invokeNative for the emulator. WAMR calls it only for natives
// registered with the plain (non-raw) API and for AOT code; Solana OS
// registers every import raw (lua_wasm.c, lua_wasi.c) and the emulator has no
// AOT, so it is never reached. The badge links WAMR's invokeNative_riscv.S
// (sources.cmake, SOLANA_OS_WAMR_AOT_SRCS) for its AOT runtime.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void invokeNative(void (*fn)(void), uint32_t *argv, uint32_t n);

void invokeNative(void (*fn)(void), uint32_t *argv, uint32_t n)
{
    (void)fn;
    (void)argv;
    (void)n;
    printf("wamr: invokeNative reached in the emulator\n");
    abort();
}
