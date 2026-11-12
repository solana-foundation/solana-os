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

#define OS_MEM_SRAM     (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define OS_MEM_PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// PSRAM is in the heap.
static inline bool os_mem_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

// The heap that TLS allocates from (os_http.c sets mbedTLS's allocator): the
// guards before a handshake look at this one.
static inline unsigned os_mem_tls_caps(void)
{
    return os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT;
}
