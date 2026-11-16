// WAMR's platform layer for Solana OS (wamr_port/platform_internal.h, the
// functions of wamr/core/shared/platform/include/platform_api_vmcore.h).
// Portable: the badge and the emulator compile this same file.
//
// Memory. Everything WAMR allocates (the loaded module, the instance, its
// linear memory, the interpreter's stack) comes through here and goes to the
// running app's heap (lw_port_heap: lh_mem_realloc in lua_host.c), so it
// counts against the one cap the app's Lua heap has, in PSRAM, with the
// PSRAM reserve. A refusal is a NULL: WAMR fails the load or instantiation,
// or memory.grow answers -1 to the app (its malloc returns NULL).
// WAMR is started and stopped around each C app (lua_wasm.c), so nothing it
// holds outlives the app and nothing is counted to the wrong one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_api_vmcore.h"
#include "lua_wasm_port.h"

// Each block carries its size (os_free and os_munmap need it for the count)
// and starts 64-aligned: WAMR asks 8 of linear memory (AOT code assumes it),
// and a frame the panel's DMA reads straight from the app's memory
// (ui_frame, bsp_display_frame) needs whole 64-byte cache lines, so an app's
// 64-aligned buffer is 64-aligned on the badge too. The badge's heap gives
// 4: up to 60 bytes of padding, then the header { u32 size, u32 offset from
// the allocation }, then the block.
#define HDR     8
#define ALIGN   64
#define SLACK   (HDR + ALIGN - 4)

static lw_port_realloc_fn s_fn;
static void *s_ud;

void lw_port_heap(lw_port_realloc_fn fn, void *ud)
{
    s_fn = fn;
    s_ud = ud;
}

static uint8_t *align_up(uint8_t *p)
{
    return (uint8_t *)(((uintptr_t)p + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));
}

static void *blk_realloc(void *ptr, size_t nsize)
{
    uint32_t *h = ptr ? (uint32_t *)ptr - 2 : NULL;
    size_t old = h ? h[0] : 0, off = h ? h[1] : 0;
    uint8_t *base = h ? (uint8_t *)ptr - off : NULL;
    if (!s_fn || nsize > UINT32_MAX - SLACK) {
        return NULL;
    }
    uint8_t *p = s_fn(s_ud, base, base ? old + SLACK : 0, nsize + SLACK);
    if (!p) {
        return NULL;
    }
    uint8_t *data = align_up(p + HDR);
    if (base && (size_t)(data - p) != off) {
        memmove(data, p + off, old < nsize ? old : nsize);     // the allocator moved it to another alignment
    }
    h = (uint32_t *)data - 2;
    h[0] = (uint32_t)nsize;
    h[1] = (uint32_t)(data - p);
    return data;
}

static void blk_free(void *ptr)
{
    if (ptr && s_fn) {
        uint32_t *h = (uint32_t *)ptr - 2;
        s_fn(s_ud, (uint8_t *)ptr - h[1], h[0] + SLACK, 0);
    }
}

int bh_platform_init(void)
{
    return 0;
}

void bh_platform_destroy(void)
{
}

void *os_malloc(unsigned size)
{
    return blk_realloc(NULL, size ? size : 1);
}

void *os_realloc(void *ptr, unsigned size)
{
    return blk_realloc(ptr, size ? size : 1);
}

void os_free(void *ptr)
{
    blk_free(ptr);
}

int os_dumps_proc_mem_info(char *out, unsigned int size)
{
    (void)out;
    (void)size;
    return -1;
}

int os_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vprintf(format, ap);
    va_end(ap);
    return n;
}

int os_vprintf(const char *format, va_list ap)
{
    return vprintf(format, ap);
}

uint64 os_time_get_boot_us(void)
{
    return (uint64)esp_timer_get_time();
}

uint64 os_time_thread_cputime_us(void)
{
    return (uint64)esp_timer_get_time();
}

korp_tid os_self_thread(void)
{
    return (korp_tid)xTaskGetCurrentTaskHandle();
}

// NULL: WAMR doesn't derive a boundary itself; lua_wasm.c sets the Lua task's
// (wasm_runtime_set_native_stack_boundary) before every call into the app.
uint8 *os_thread_get_stack_boundary(void)
{
    return NULL;
}

void os_thread_jit_write_protect_np(bool enabled)
{
    (void)enabled;
}

// One task uses WAMR at a time (the Lua task), so the locks have nothing to do.
int os_mutex_init(korp_mutex *mutex)
{
    *mutex = 0;
    return BHT_OK;
}

int os_mutex_destroy(korp_mutex *mutex)
{
    (void)mutex;
    return BHT_OK;
}

int os_mutex_lock(korp_mutex *mutex)
{
    (void)mutex;
    return BHT_OK;
}

int os_mutex_unlock(korp_mutex *mutex)
{
    (void)mutex;
    return BHT_OK;
}

// Linear memory and the AOT loader's data sections. Never executable
// memory: the badge runs native code only in place, from a flash slot
// (lua_aot.c), so a file that would need code copied into RAM (not built
// with --xip) gets NULL here and doesn't load.
void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint;
    (void)flags;
    (void)file;
    if (prot & MMAP_PROT_EXEC) {
        return NULL;
    }
    void *p = blk_realloc(NULL, size ? size : 1);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

// Growing in place where the heap can (memory.grow): no second copy of a
// big linear memory at the peak. The new part reads as zeros.
void *os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    if (!old_addr) {
        return os_mremap_slow(old_addr, old_size, new_size);
    }
    if (!lw_port_slow(true)) {
        return NULL;
    }
    uint8_t *p = blk_realloc(old_addr, new_size ? new_size : 1);
    if (p && new_size > old_size) {
        memset(p + old_size, 0, new_size - old_size);
    }
    lw_port_slow(false);
    return p;
}

void os_munmap(void *addr, size_t size)
{
    (void)size;
    blk_free(addr);
}

int os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    (void)prot;
    return 0;
}

void os_dcache_flush(void)
{
}

void os_icache_flush(void *start, size_t len)
{
    (void)start;
    (void)len;
}

// WAMR's runtime calls this only for imports registered through its C API
// (wasm_c_api.c), which Solana OS doesn't build: every import is a raw
// native. Here so the runtime links without the C API.
#include "wasm_c_api.h"

void wasm_trap_delete(wasm_trap_t *trap)
{
    (void)trap;
}
