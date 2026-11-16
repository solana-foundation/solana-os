#pragma once

// WAMR's platform header for Solana OS (lua/lua_wasm_port.c implements it).
// Portable: the badge and the emulator build the same file. WAMR runs only
// on the Lua host's task, one app at a time, with no threads of its own
// (WASM_ENABLE_THREAD_MGR 0), so the locks are empty and the "thread" is
// whatever task calls in.

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BH_PLATFORM_SOLANA_OS
#define BH_PLATFORM_SOLANA_OS
#endif

typedef void *korp_tid;
typedef int korp_mutex;
typedef int korp_cond;
typedef void *korp_thread;
typedef int korp_rwlock;
typedef int korp_sem;

#define OS_THREAD_MUTEX_INITIALIZER 0

#define BH_APPLET_PRESERVED_STACK_SIZE (2 * 1024)
#define BH_THREAD_DEFAULT_PRIORITY 5

typedef int os_file_handle;
typedef void *os_dir_stream;
typedef int os_raw_file_handle;

static inline int
os_getpagesize(void)
{
    return 4096;
}

static inline os_file_handle
os_get_invalid_handle(void)
{
    return -1;
}

#ifdef __cplusplus
}
#endif
