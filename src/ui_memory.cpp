#include "ui_memory.h"

// External function to clear tile cache
extern void tile_cache_clear();

// --- Memory Allocation ---

void* ui_alloc_safe(size_t size, uint32_t caps, AllocPriority priority, const char* desc) {
    // Try allocation
    void* ptr = heap_caps_malloc(size, caps);

    if (ptr) {
        return ptr;
    }

    // Allocation failed - handle based on priority
    Serial.printf("[UI_MEMORY] Allocation failed: %s (%zu bytes, caps=0x%x)\n",
                  desc ? desc : "unknown", size, caps);

    switch (priority) {
        case ALLOC_CRITICAL:
            // Try emergency cleanup and retry
            Serial.println("[UI_MEMORY] CRITICAL allocation - attempting cleanup...");
            ui_emergency_memory_cleanup();

            ptr = heap_caps_malloc(size, caps);
            if (ptr) {
                Serial.println("[UI_MEMORY] Allocation succeeded after cleanup");
                return ptr;
            }

            // Still failed - system cannot continue
            Serial.println("[UI_MEMORY] FATAL: Critical allocation failed, restarting...");
            Serial.flush();
            delay(100);
            ESP.restart();
            return NULL;  // Never reached

        case ALLOC_DEGRADED:
            // Try cleanup and retry
            Serial.println("[UI_MEMORY] DEGRADED allocation - attempting cleanup...");
            ui_emergency_memory_cleanup();

            ptr = heap_caps_malloc(size, caps);
            if (ptr) {
                Serial.println("[UI_MEMORY] Allocation succeeded after cleanup");
                return ptr;
            }

            // Return NULL - caller handles degraded mode
            Serial.printf("[UI_MEMORY] Degraded mode: %s unavailable\n", desc ? desc : "resource");
            return NULL;

        case ALLOC_OPTIONAL:
            // Just return NULL - operation is optional
            Serial.printf("[UI_MEMORY] Optional allocation skipped: %s\n", desc ? desc : "resource");
            return NULL;

        default:
            return NULL;
    }
}

void* ui_alloc_psram(size_t size, AllocPriority priority, const char* desc) {
    return ui_alloc_safe(size, MALLOC_CAP_SPIRAM, priority, desc);
}

void* ui_alloc_internal(size_t size, AllocPriority priority, const char* desc) {
    return ui_alloc_safe(size, MALLOC_CAP_INTERNAL, priority, desc);
}

void* ui_alloc_dma(size_t size, AllocPriority priority, const char* desc) {
    return ui_alloc_safe(size, MALLOC_CAP_DMA, priority, desc);
}

// --- Memory Health Monitoring ---

MemHealthStatus ui_check_memory_health() {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Avoid division by zero
    float fragmentation = 0.0f;
    if (free_psram > 0) {
        fragmentation = 100.0f * (1.0f - (float)largest_block / (float)free_psram);
    }

    // Critical: Less than 1MB free OR less than 512KB largest block
    if (free_psram < 1024 * 1024 || largest_block < 512 * 1024) {
        Serial.printf("[MEM_HEALTH] CRITICAL: %lu KB free, %lu KB largest (%.1f%% frag)\n",
                     free_psram / 1024, largest_block / 1024, fragmentation);
        return MEM_HEALTH_CRITICAL;
    }

    // Warning: Less than 2MB free OR more than 50% fragmentation
    if (free_psram < 2 * 1024 * 1024 || fragmentation > 50.0f) {
        Serial.printf("[MEM_HEALTH] WARNING: %lu KB free, %lu KB largest (%.1f%% frag)\n",
                     free_psram / 1024, largest_block / 1024, fragmentation);
        return MEM_HEALTH_WARNING;
    }

    return MEM_HEALTH_OK;
}

size_t ui_get_free_psram() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t ui_get_largest_psram_block() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

float ui_get_psram_fragmentation() {
    size_t free = ui_get_free_psram();
    size_t largest = ui_get_largest_psram_block();
    if (free == 0) return 0.0f;
    return 100.0f * (1.0f - (float)largest / (float)free);
}

size_t ui_get_free_internal() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t ui_get_largest_internal_block() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

void ui_print_memory_status() {
    size_t psram_free = ui_get_free_psram();
    size_t psram_largest = ui_get_largest_psram_block();
    float psram_frag = ui_get_psram_fragmentation();
    size_t internal_free = ui_get_free_internal();
    size_t internal_largest = ui_get_largest_internal_block();

    Serial.println("=== MEMORY STATUS ===");
    Serial.printf("PSRAM: %lu KB free, %lu KB largest block (%.1f%% frag)\n",
                  psram_free / 1024, psram_largest / 1024, psram_frag);
    Serial.printf("Internal: %lu KB free, %lu KB largest block\n",
                  internal_free / 1024, internal_largest / 1024);

    MemHealthStatus health = ui_check_memory_health();
    const char* health_str = "OK";
    if (health == MEM_HEALTH_WARNING) health_str = "WARNING";
    else if (health == MEM_HEALTH_CRITICAL) health_str = "CRITICAL";
    Serial.printf("Health: %s\n", health_str);
    Serial.println("=====================");
}

// --- Memory Pressure Response ---

void ui_emergency_memory_cleanup() {
    Serial.println("[UI_MEMORY] Emergency cleanup starting...");

    // Clear tile cache (biggest memory consumer)
    tile_cache_clear();

    // Force garbage collection hint (not available on ESP32, but good practice)
    // The heap will be compacted automatically on next alloc

    // Log result
    size_t psram_free = ui_get_free_psram();
    size_t psram_largest = ui_get_largest_psram_block();
    Serial.printf("[UI_MEMORY] After cleanup: %lu KB free, %lu KB largest block\n",
                  psram_free / 1024, psram_largest / 1024);
}

bool ui_should_defer_allocation(size_t size) {
    // Check if we have enough headroom for the allocation + safety margin
    const size_t UI_SAFETY_MARGIN = 2 * 1024 * 1024;  // 2MB reserved for UI

    size_t free_psram = ui_get_free_psram();
    size_t largest_block = ui_get_largest_psram_block();

    // Defer if:
    // 1. Allocation would leave less than safety margin
    // 2. Largest block is smaller than allocation (fragmentation issue)
    if (free_psram - size < UI_SAFETY_MARGIN) {
        Serial.printf("[UI_MEMORY] Deferring %zu byte allocation (would leave %lu KB)\n",
                      size, (free_psram - size) / 1024);
        return true;
    }

    if (largest_block < size) {
        Serial.printf("[UI_MEMORY] Deferring %zu byte allocation (largest block: %lu KB)\n",
                      size, largest_block / 1024);
        return true;
    }

    return false;
}
