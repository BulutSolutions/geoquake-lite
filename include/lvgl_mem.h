#pragma once

#include <esp_heap_caps.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL 9.x TLSF pool allocator — strictly requires PSRAM (384 KB cannot fit in free internal SRAM)
static inline void *lvgl_pool_alloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        printf("[LVGL] ❌ FATAL: Failed to allocate %u bytes for LVGL TLSF pool from PSRAM!\n",
               (unsigned int)size);
        abort();
    }
    return ptr;
}

#ifdef __cplusplus
}
#endif
