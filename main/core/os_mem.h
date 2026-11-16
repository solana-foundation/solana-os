#pragma once

// Memory (2.0.0). Two heaps: the chip's SRAM ("internal": task stacks, DMA
// buffers, LVGL's objects, the Bluetooth controller, small allocations) and,
// on every badge so far, 8 MB of PSRAM in the ESP32-C5HR8's package (Wi-Fi and
// lwIP buffers, TLS, Lua apps' heaps, anything malloc'd over 16 KB). A chip
// without PSRAM still boots (CONFIG_SPIRAM_IGNORE_NOTFOUND) and gets 1.x's
// limits. PSRAM is slower than SRAM and unreachable while the flash is being
// written (the cache is off), like code in flash: nothing in an ISR may use it.
//
// Portable: the emulator's heap_caps shim answers for both heaps.

#include <stdbool.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#define OS_MEM_SRAM     (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define OS_MEM_PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
// The SRAM that malloc() (and so LVGL) can still take. OS_MEM_SRAM's figure,
// the console's "heap" line, also counts the 32 KB pool that
// CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL sets aside at boot for DMA and
// internal-only allocations (task stacks, drivers): malloc never uses it, and
// a small malloc that finds the rest full quietly lands in PSRAM instead. A
// floor that must leave the badge SRAM it can malloc measures this one
// (2.1.0; 2.0.1's UI check counted the pool, so it protected almost nothing).
#define OS_MEM_SRAM_MALLOC  (MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT)

// Read that figure through os_mem_sram_malloc_free()/_min(), not
// heap_caps_get_free_size(OS_MEM_SRAM_MALLOC). ESP-IDF master (2.2.0, esp-idf
// 90a275f29ac) gives the pool MALLOC_CAP_DEFAULT at its lowest priority:
// malloc() still reaches it only once PSRAM is full too, but the caps query
// now counts it, and no caps tell it from the rest of the SRAM. So the pool's
// size comes off. Nothing takes from the pool while the rest of the SRAM has
// room (explicit DMA and internal requests use it last as well), so this is
// exact until the SRAM is nearly full and errs low after, the safe side.
#if CONFIG_SPIRAM && defined(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL)
#define OS_MEM_SRAM_POOL    CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
#else
#define OS_MEM_SRAM_POOL    0
#endif

// PSRAM is in the heap.
static inline bool os_mem_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

// Where a buffer that nothing reads from an ISR or with the cache off should
// go: PSRAM when there is some, else wherever malloc puts it.
static inline unsigned os_mem_bulk_caps(void)
{
    return os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT;
}

// The heap that TLS allocates from (os_http.c sets mbedTLS's allocator): the
// guards before a handshake look at this one.
static inline unsigned os_mem_tls_caps(void)
{
    return os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT;
}

static inline size_t os_mem_sram_less_pool(size_t n)
{
    size_t pool = os_mem_psram() ? OS_MEM_SRAM_POOL : 0;
    return n > pool ? n - pool : 0;
}

static inline size_t os_mem_sram_malloc_free(void)
{
    return os_mem_sram_less_pool(heap_caps_get_free_size(OS_MEM_SRAM_MALLOC));
}

// The low-water mark of that figure can't be read exactly: the heap keeps one
// minimum for the SRAM and the pool together, and when the SRAM ran nearly
// full, DMA and internal requests may have dipped into the pool too. So this
// is a lower bound (the raw minimum less the whole pool); the truth lies
// between it and the raw minimum (heap_caps_get_minimum_free_size of
// OS_MEM_SRAM_MALLOC), which the console's `heap` prints as a range.
static inline size_t os_mem_sram_malloc_min(void)
{
    return os_mem_sram_less_pool(heap_caps_get_minimum_free_size(OS_MEM_SRAM_MALLOC));
}
