#ifndef UI_MEMORY_H
#define UI_MEMORY_H

#include <Arduino.h>
#include <esp_heap_caps.h>

// --- SAFE MEMORY ALLOCATION ---
// Provides safe allocation with fallback strategies

// Allocation priority levels
typedef enum {
    ALLOC_CRITICAL,   // System cannot continue - restart on fail
    ALLOC_DEGRADED,   // Can continue with reduced features
    ALLOC_OPTIONAL    // Skip if unavailable
} AllocPriority;

// Memory health status
typedef enum {
    MEM_HEALTH_OK = 0,        // >2MB free, <50% fragmentation
    MEM_HEALTH_WARNING = 1,   // <2MB free OR >50% fragmentation
    MEM_HEALTH_CRITICAL = 2   // <1MB free OR <512KB largest block
} MemHealthStatus;

// --- Memory Allocation ---

// Safe allocation with priority-based fallback
// desc: description for logging (e.g., "LVGL buffer", "tile decode")
// Returns NULL only for ALLOC_OPTIONAL on failure
void* ui_alloc_safe(size_t size, uint32_t caps, AllocPriority priority, const char* desc);

// Safe PSRAM allocation (common case)
void* ui_alloc_psram(size_t size, AllocPriority priority, const char* desc);

// Safe internal RAM allocation (common case)
void* ui_alloc_internal(size_t size, AllocPriority priority, const char* desc);

// Safe DMA-capable allocation (for display buffers)
void* ui_alloc_dma(size_t size, AllocPriority priority, const char* desc);

// --- Memory Health Monitoring ---

// Check current memory health status
MemHealthStatus ui_check_memory_health();

// Get PSRAM statistics
size_t ui_get_free_psram();
size_t ui_get_largest_psram_block();
float ui_get_psram_fragmentation();

// Get internal RAM statistics
size_t ui_get_free_internal();
size_t ui_get_largest_internal_block();

// Print memory status to serial (for debugging)
void ui_print_memory_status();

// --- Memory Pressure Response ---

// Emergency cleanup when memory is critically low
// Clears caches and non-essential allocations
void ui_emergency_memory_cleanup();

// Check if we should defer heavy operations
bool ui_should_defer_allocation(size_t size);

#endif // UI_MEMORY_H
