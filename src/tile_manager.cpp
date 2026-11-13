#include "tile_manager.h"
#include "sd_card.h"
#include "lv_fs_flash.h"
#include "config.h"
#include "board_profile.h"
#include "config_constants.h"
#include "qoi_decoder.h"
#include "map_projection.h"
#include "perf_probe.h"
#include "tile_pak.h"
#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <rom/cache.h>
#include <atomic>  // FIX #4: For atomic cache_dirty flag

// --- Configuration ---
// TILE_SIZE and MAX_ZOOM_LEVEL now defined in config_constants.h

// Zoom level transition thresholds (1.4x)
static const float ZOOM_THRESHOLDS[] = {1.0f, 1.4f, 2.0f, 2.8f, 4.0f, 5.6f, 10.0f};

static bool manager_initialized = false;

// Where the tiles are (map_tiles_refresh_availability -> tile_manager_set_tile_source). Changed only while the
// tile cache is down (no decode task runs), so the decode tasks read these without a lock.
static bool core_pak_ready = false;           // s_core_index describes TILE_PAK_PATH on that filesystem
static bool core_pak_on_sd = false;           // the map pack is on the SD card (else in LittleFS)
static uint8_t core_pak_levels = 0;           // highest level in the pack (4..6), 0 without a pack
static uint8_t max_tile_level = 4;            // the pack's highest level, or 5/6 from loose detail tiles on the SD card
// Tile offsets in the map pack: one PSRAM block (43.7 KB), allocated at the first pack and never freed or moved,
// because the decode tasks read it without a lock.
static tile_pak::Index* s_core_index = nullptr;

// --- Tile Cache ---
static TileCache g_tile_cache;
static bool cache_initialized = false;
static SemaphoreHandle_t cache_mutex = NULL;
static std::atomic<bool> cache_shutdown_requested{false};  // set under cache_mutex by tile_cache_deinit()
static std::atomic<bool> drain_requested{false};  // set under cache_mutex by tile_cache_drain()

// Current map view state owned by map_tiles/map_renderer.
extern uint8_t current_z;
extern float view_center_lat;
extern float view_center_lon;

// === TELEMETRY: Zoom-level decode timing stats ===
// Always declared to allow stats retrieval via tile_cache_get_decode_stats()
static uint32_t decode_time_sum[7] = {0};  // z0-z6
static uint32_t decode_count[7] = {0};

bool tile_manager_set_tile_source(const tile_pak::Index* core, bool core_on_sd, uint8_t sd_detail_level) {
    // The decode tasks read the source without a lock: it may only change while they are not running.
    if (cache_initialized) {
        Serial.println("[TILE] Tile source unchanged: the tile cache is in use");
        return false;
    }
    // Only a fully parsed index (levels set by tile_pak::parse) may become the source.
    if (core && (core->levels < tile_pak::kMinLevels || core->levels > tile_pak::kMaxLevels)) {
        Serial.println("[TILE] Tile source unchanged: the pack index is not valid");
        return false;
    }
    if (core && !s_core_index) {
        s_core_index = tile_pak_alloc_index();
        if (!s_core_index) {
            Serial.println("[TILE] Tile source unchanged: no memory for the pack index");
            return false;
        }
    }
    core_pak_ready = false;  // nothing reads the index while it is replaced (no decode task runs here)
    if (core) memcpy(s_core_index, core, sizeof(*core));
    core_pak_levels = core ? core->levels : 0;
    core_pak_on_sd = core != nullptr && core_on_sd;
    // Loose SD detail tiles only add levels above the pack's own.
    max_tile_level = !core ? 4 : (sd_detail_level > core_pak_levels ? sd_detail_level : core_pak_levels);
    if (max_tile_level > tile_pak::kMaxLevels) max_tile_level = tile_pak::kMaxLevels;
    core_pak_ready = core != nullptr;
    Serial.printf("[TILE] Tile source: %s (z1-z%u), highest level z%u\n",
                  !core_pak_ready ? "none" : (core_on_sd ? "map pack on the SD card" : "map pack in flash"),
                  (unsigned)core_pak_levels, (unsigned)max_tile_level);
    return true;
}

bool tile_manager_using_flash_fallback() {
    return core_pak_ready && !core_pak_on_sd;
}

uint8_t tile_manager_get_max_tile_level() {
    return max_tile_level;
}

// ============================================================================
// FIX #6: PREALLOCATED TILE BUFFER POOL
// ============================================================================
// Preallocating all tile buffers at init time prevents PSRAM fragmentation
// from repeated alloc/free cycles during pan/zoom operations.
// Pool size: 15 tiles * 128KB = 1.92MB (matches TILE_CACHE_MAX)

// TILE_BUFFER_SIZE now defined in config_constants.h (value: 256*256*2 = 128KB)

static uint8_t* tile_pool[TILE_CACHE_MAX];       // Preallocated buffers
static bool tile_pool_used[TILE_CACHE_MAX];      // Track buffer usage
static bool tile_pool_initialized = false;
static SemaphoreHandle_t pool_mutex = NULL;      // Thread-safe pool access
#define QOI_SCRATCH_BUF_SIZE (64 * 1024)
static uint8_t* s_qoi_scratch_buf[NUM_DECODE_TASKS] = {NULL};

// Initialize the tile buffer pool (call from tile_cache_init)
static bool tile_pool_init() {
    if (tile_pool_initialized) return true;

    // Create mutex for thread-safe pool access (multi-core decode tasks)
    if (!pool_mutex) {
        pool_mutex = xSemaphoreCreateMutex();
    }

    #if TILE_DEBUG_LEVEL >= 2
    Serial.println("[POOL] Preallocating tile buffer pool...");
    #endif
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    int allocated = 0;
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        // No zero-fill: a buffer is only ever published after a successful decode,
        // which writes every pixel (qoi_decode_from_memory), or a NODATA black fill.
        tile_pool[i] = (uint8_t*)heap_caps_malloc(TILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        tile_pool_used[i] = false;

        if (!tile_pool[i]) {
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[POOL] WARNING: Could only allocate %d of %d buffers\n", i, TILE_CACHE_MAX);
            #endif
            // Partial allocation is OK - we'll use what we have
            break;
        }
        allocated++;
    }

    // Allocate persistent scratch buffers for QOI decodes (zero-allocation worker)
    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (!s_qoi_scratch_buf[i]) {
            s_qoi_scratch_buf[i] = (uint8_t*)heap_caps_malloc(QOI_SCRATCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
        }
    }

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    #if TILE_DEBUG_LEVEL >= 2
    Serial.printf("[POOL] Allocated %d buffers (%.1f MB), PSRAM: %d KB -> %d KB\n",
                  allocated, (allocated * TILE_BUFFER_SIZE) / (1024.0f * 1024.0f),
                  free_before / 1024, free_after / 1024);
    #endif

    tile_pool_initialized = true;
    return (allocated > 0);
}

static void tile_pool_deinit() {
    if (pool_mutex) xSemaphoreTake(pool_mutex, portMAX_DELAY);

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (tile_pool[i]) {
            heap_caps_free(tile_pool[i]);
            tile_pool[i] = NULL;
        }
        tile_pool_used[i] = false;
    }

    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (s_qoi_scratch_buf[i]) {
            heap_caps_free(s_qoi_scratch_buf[i]);
            s_qoi_scratch_buf[i] = NULL;
        }
    }
    tile_pool_initialized = false;

    if (pool_mutex) xSemaphoreGive(pool_mutex);
}

// Acquire a buffer from the pool (returns NULL if pool exhausted)
// Thread-safe: protected by pool_mutex for multi-core decode tasks
static uint8_t* tile_pool_acquire() {
    if (pool_mutex) xSemaphoreTake(pool_mutex, portMAX_DELAY);

    bool have_pool = false;
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (!tile_pool[i]) continue;
        have_pool = true;
        if (!tile_pool_used[i]) {
            tile_pool_used[i] = true;  // contents are stale; the decoder overwrites all of it
            if (pool_mutex) xSemaphoreGive(pool_mutex);
            return tile_pool[i];
        }
    }

    if (pool_mutex) xSemaphoreGive(pool_mutex);

    // Pool exhausted. With a pool in place that means over-subscription or a
    // leak: fail closed so the decoder's evict-and-retry path runs, instead of
    // PSRAM growing by 128 KB per call. Only when no pool buffer could be
    // allocated at init do tiles come from the heap (released after use).
    if (have_pool) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.println("[POOL] Pool exhausted");
        #endif
        return NULL;
    }
    return (uint8_t*)heap_caps_malloc(TILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
}

// Release a buffer back to the pool
// Thread-safe: protected by pool_mutex for multi-core decode tasks
static void tile_pool_release(uint8_t* buffer) {
    if (!buffer) return;

    if (pool_mutex) xSemaphoreTake(pool_mutex, portMAX_DELAY);

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (tile_pool[i] == buffer) {
            tile_pool_used[i] = false;
            if (pool_mutex) xSemaphoreGive(pool_mutex);
            return;
        }
    }

    if (pool_mutex) xSemaphoreGive(pool_mutex);

    // Not a pool buffer - must be a heap fallback, free it
    heap_caps_free(buffer);
}

// Check how many pool buffers are free
// Thread-safe: protected by pool_mutex for multi-core decode tasks
static int tile_pool_free_count() {
    if (pool_mutex) xSemaphoreTake(pool_mutex, portMAX_DELAY);

    int count = 0;
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (tile_pool[i] && !tile_pool_used[i]) count++;
    }

    if (pool_mutex) xSemaphoreGive(pool_mutex);
    return count;
}

// ============================================================================
// FIX #7: ADAPTIVE CACHE SIZING + MEMORY TELEMETRY
// ============================================================================

// Priority zoom level (don't evict tiles from this level)
// Moved here for use in tile_cache_can_load_more()
static int8_t priority_z = -1;

// FIX #1: Mutex contention telemetry counter (declared early for log function)
static uint32_t cache_lock_contentions = 0;

// Log memory stats for diagnostics (call periodically or on demand)
void tile_cache_log_memory_stats() {
    #if TILE_DEBUG_LEVEL >= 2
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    float fragmentation = 1.0f - ((float)largest_block / (float)free_psram);

    int pool_free = tile_pool_free_count();
    int pool_used = TILE_CACHE_MAX - pool_free;

    Serial.println("\n[TILE] === MEMORY TELEMETRY ===");
    Serial.printf("[TILE]   PSRAM Free: %u KB\n", free_psram / 1024);
    Serial.printf("[TILE]   Largest Block: %u KB\n", largest_block / 1024);
    Serial.printf("[TILE]   Fragmentation: %.1f%%\n", fragmentation * 100);
    Serial.printf("[TILE]   Pool Usage: %d/%d buffers\n", pool_used, TILE_CACHE_MAX);
    Serial.printf("[TILE]   Cache Count: %d tiles\n", g_tile_cache.count);
    Serial.printf("[TILE]   Hit Rate: %d/%d (%.1f%%)\n",
                  g_tile_cache.hits,
                  g_tile_cache.hits + g_tile_cache.misses,
                  g_tile_cache.hits + g_tile_cache.misses > 0
                      ? (100.0f * g_tile_cache.hits / (g_tile_cache.hits + g_tile_cache.misses))
                      : 0.0f);
    // FIX #1: Report mutex contention count for pan lag diagnosis
    Serial.printf("[TILE]   Mutex Contentions: %lu\n", cache_lock_contentions);
    Serial.println("[TILE] ===========================\n");
    #endif
}

// === TELEMETRY: Dump full cache state for diagnostics ===
void tile_cache_dump_state() {
#if TILE_TELEMETRY_LEVEL >= 2
    cache_lock();
    Serial.println("\n[CACHE] === STATE DUMP ===");

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if (t->state == TILE_EMPTY) {
            Serial.printf("[CACHE] Slot %2d: EMPTY\n", i);
        } else {
            const char* state_str = "?";
            switch(t->state) {
                case TILE_LOADING: state_str = "LOADING"; break;
                case TILE_READY: state_str = "READY  "; break;
                case TILE_DISPLAYED: state_str = "DISPLAY"; break;
                case TILE_RETRY_PENDING: state_str = "RETRY  "; break;
                case TILE_FAILED: state_str = "FAILED "; break;
                case TILE_NODATA: state_str = "NODATA "; break;
                default: state_str = "UNKNOWN"; break;
            }
            uint32_t age = millis() - t->last_used;
            Serial.printf("[CACHE] Slot %2d: z%d/%d_%d %s age=%5lums ref=%d",
                          i, t->coord.z, t->coord.x, t->coord.y,
                          state_str, age, t->refcount);
            if (t->failure_count > 0) {
                Serial.printf(" attempts=%d", t->failure_count);
            }
            if (t->refcount > 0) {
                Serial.print(" (pinned)");
            }
            Serial.println();
        }
    }

    // Queue status
    int vis_len = (visible_queue_tail - visible_queue_head + VISIBLE_QUEUE_SIZE) % VISIBLE_QUEUE_SIZE;
    int pre_len = (prefetch_queue_tail - prefetch_queue_head + PREFETCH_QUEUE_SIZE) % PREFETCH_QUEUE_SIZE;
    Serial.printf("[CACHE] Queue: visible=%d/%d prefetch=%d/%d\n",
                  vis_len, VISIBLE_QUEUE_SIZE, pre_len, PREFETCH_QUEUE_SIZE);

    // Pool status
    int pool_free = tile_pool_free_count();
    Serial.printf("[CACHE] Pool: %d/%d used\n", TILE_CACHE_MAX - pool_free, TILE_CACHE_MAX);

    // Cache stats
    int total_requests = g_tile_cache.hits + g_tile_cache.misses;
    float hit_rate = (total_requests > 0) ? (100.0f * g_tile_cache.hits / total_requests) : 0.0f;
    Serial.printf("[CACHE] Stats: %d cached, %d hits, %d misses (%.1f%% hit rate)\n",
                  g_tile_cache.count, g_tile_cache.hits, g_tile_cache.misses, hit_rate);
    Serial.println("[CACHE] ====================\n");

    cache_unlock();
#endif
}

// === TELEMETRY: Get decode timing stats for a zoom level ===
void tile_cache_get_decode_stats(uint8_t z, uint32_t* avg_ms, uint32_t* count) {
    if (z > 6) {
        *avg_ms = 0;
        *count = 0;
        return;
    }
    *count = decode_count[z];
    *avg_ms = (decode_count[z] > 0) ? (decode_time_sum[z] / decode_count[z]) : 0;
}

// === PHASE 4.4: ADAPTIVE TILE CACHE SIZING ===
// Dynamically size cache based on available PSRAM using formula instead of hardcoded thresholds
// Formula: max_tiles = (free_psram - ui_reserved) / tile_size
// This ensures UI always has enough memory while maximizing cache when possible

// Configuration constants for adaptive sizing
#define UI_RESERVED_MEMORY (2.5f * 1024 * 1024)  // 2.5MB safety margin for UI components
#define TILE_MEMORY_SIZE (128 * 1024)            // ~128KB per decoded tile (256x256 RGB565)

// Track initial calibration for telemetry
static int initial_effective_max = -1;

// Get effective max cache size based on available memory
// Returns reduced count when memory is critically low
static int tile_cache_get_effective_max() {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Calculate maximum tiles that would fit while preserving UI safety margin
    // Use free_psram for total calculation, but check largest_block for fragmentation
    int formula_max = 0;
    if (free_psram > UI_RESERVED_MEMORY) {
        formula_max = (int)((free_psram - UI_RESERVED_MEMORY) / TILE_MEMORY_SIZE);
    }

    // Also check fragmentation - if largest block is small, cache may fail to allocate
    // even with "enough" total free memory
    int fragmentation_max = (largest_block > UI_RESERVED_MEMORY)
        ? (int)((largest_block - 512 * 1024) / TILE_MEMORY_SIZE)  // 512KB margin for largest block
        : 0;

    // Take the more conservative of the two calculations
    int effective = (formula_max < fragmentation_max) ? formula_max : fragmentation_max;

    // Clamp to valid range [1, TILE_CACHE_MAX]
    if (effective < 1) effective = 1;
    if (effective > TILE_CACHE_MAX) effective = TILE_CACHE_MAX;

    // Telemetry: Log when effective max changes significantly
    #if TILE_DEBUG_LEVEL >= 2
    static int last_logged_max = -1;
    if (effective != last_logged_max) {
        Serial.printf("[CACHE] Adaptive sizing: %d tiles (free=%dKB, largest=%dKB, formula=%d, frag=%d)\n",
                      effective, free_psram / 1024, largest_block / 1024, formula_max, fragmentation_max);
        last_logged_max = effective;
    }
    #endif

    return effective;
}

// Check if we should allow loading more tiles (respects adaptive sizing)
// FIX 4.2: Now checks tiles at current zoom level to prevent old zoom tiles
// from blocking center tile loads after zoom changes
bool tile_cache_can_load_more() {
    int effective_max = tile_cache_get_effective_max();
    int pool_used = TILE_CACHE_MAX - tile_pool_free_count();

    // FIX 4.2: Count tiles at CURRENT zoom level (tracked via priority_z)
    // This ensures we can always load tiles for the current view even when
    // the cache is full of old zoom level tiles
    if (priority_z >= 0) {
        int current_z_count = 0;
        for (int i = 0; i < TILE_CACHE_MAX; i++) {
            if (g_tile_cache.tiles[i].state != TILE_EMPTY &&
                g_tile_cache.tiles[i].coord.z == priority_z) {
                current_z_count++;
            }
        }

        // Always allow loading if current zoom has room for 3x3 grid (9 tiles)
        // This prevents old zoom tiles from blocking essential center tiles
        if (current_z_count < 9) {
            return true;
        }
    }

    if (pool_used >= effective_max) {
        #if TILE_DEBUG_LEVEL >= 2
        Serial.printf("[CACHE] Adaptive limit reached: %d/%d (effective max: %d)\n",
                      pool_used, TILE_CACHE_MAX, effective_max);
        #endif
        return false;
    }
    return true;
}
// ============================================================================

// Parallel decode tasks (2 tasks for best throughput/contention tradeoff)
// NUM_DECODE_TASKS is defined in config_constants.h; 3 tasks contended on the SD mutex.
static TaskHandle_t decode_task_handles[NUM_DECODE_TASKS] = {};

// FIX 2: Priority queues - visible tiles load before prefetch
// Visible queue: tiles requested during render (high priority)
// Prefetch queue: tiles requested during predictive preloading (low priority)
#define VISIBLE_QUEUE_SIZE 16
#define PREFETCH_QUEUE_SIZE 48
static TileCoord visible_queue[VISIBLE_QUEUE_SIZE];
static uint8_t visible_queue_head = 0;
static uint8_t visible_queue_tail = 0;
static TileCoord prefetch_queue[PREFETCH_QUEUE_SIZE];
static uint8_t prefetch_queue_head = 0;
static uint8_t prefetch_queue_tail = 0;

// Render-thread fallback queue for visible misses observed while cache_mutex is busy.
// It is protected by its own zero-timeout mutex so tile_cache_get_tile() stays non-blocking.
#define DEFERRED_VISIBLE_QUEUE_SIZE 16
static TileCoord deferred_visible_queue[DEFERRED_VISIBLE_QUEUE_SIZE];
static uint8_t deferred_visible_head = 0;
static uint8_t deferred_visible_tail = 0;
static SemaphoreHandle_t deferred_visible_mutex = NULL;

// Legacy alias for queue length checks (combined)
#define LOAD_QUEUE_SIZE (VISIBLE_QUEUE_SIZE + PREFETCH_QUEUE_SIZE)

// FIX #4: Atomic cache dirty flag to prevent race conditions between
// decode tasks (set) and main thread (read/clear)
static std::atomic<bool> cache_dirty{false};

// Decodes claimed by a worker and not yet finished. A cancelled slot can go
// EMPTY while its worker still owns a pool buffer, so shutdown/drain must wait
// for this to reach zero, not just for LOADING slots to disappear.
static std::atomic<int> s_decodes_in_flight{0};
static std::atomic<int> s_decode_tasks_alive{0};  // decode tasks running; tile_cache_deinit() waits for 0
// Reset a slot to EMPTY under cache lock without ever reusing a generation:
// a stale worker compares generations, so a reset to 0 could let it publish
// into a slot that now belongs to a different tile (ABA).
static void slot_reset_locked(CachedTile* t) {
    const uint32_t next_generation = t->generation + 1;
    memset(t, 0, sizeof(CachedTile));
    t->generation = next_generation;
    t->state = TILE_EMPTY;
}

// Note: priority_z declared earlier in the file (needed by tile_cache_can_load_more)

static void tile_cache_evict_lru_locked();
static bool load_tile_to_slot(TileCoord coord, int slot, int task_id, uint32_t claim_generation);
static void build_tile_path(char* out_path, size_t out_size, TileCoord coord, const char* ext);
static void queue_tile_load_priority(TileCoord coord, bool is_visible);
static void drain_deferred_visible_locked();

// Centralized state transition helpers (must be called under cache lock)

// Atomically publish decoded result to tile slot
// Assumes img_dsc was already populated by decode_tile_to_psram()
static void tile_publish_locked(CachedTile* t) {
    t->state = TILE_READY;
    t->last_used = millis();
}

// Atomically clear tile buffer and reset state
static void tile_clear_locked(CachedTile* t, TileState next_state) {
    if (t->decoded_data) {
        tile_pool_release(t->decoded_data);
        t->decoded_data = NULL;
    }
    t->decoded_size = 0;
    memset(&t->img_dsc, 0, sizeof(t->img_dsc));
    t->state = next_state;
}

static void cache_lock() {
    if (cache_mutex) xSemaphoreTake(cache_mutex, portMAX_DELAY);
}

static void cache_unlock() {
    if (cache_mutex) xSemaphoreGive(cache_mutex);
}

// FIX #1: Non-blocking try-lock with configurable timeout
// Returns true if lock acquired, false if timeout (caller should skip frame/show gap)
static bool cache_try_lock(uint32_t timeout_ms = 10) {
    if (!cache_mutex) return true;  // No mutex = always "locked"
    bool acquired = xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!acquired) {
        cache_lock_contentions++;
        #if TILE_DEBUG_LEVEL >= 1
        // Log every 10th contention to avoid spam
        if (cache_lock_contentions % 10 == 1) {
            Serial.printf("[CACHE] Mutex contention #%lu (timeout %lu ms)\n",
                          cache_lock_contentions, timeout_ms);
        }
        #endif
    }
    return acquired;
}

// === BLANK TILE DETECTION ===
// Detect tiles that are mostly white (potential "No Data" placeholder images).
// Samples multiple regions to avoid false positives from legitimate white areas.
// Returns true if tile appears to be a blank/placeholder (fill with black instead).
static bool is_tile_blank_placeholder(uint8_t* data, int w, int h) {
    uint16_t* pixels = (uint16_t*)data;
    int total = w * h;
    int white_count = 0;

    // Sample 5 regions: 4 corners + center (20 pixels each = 100 samples)
    // This catches placeholder tiles while avoiding false positives on snow/ice
    int sample_points[] = {
        0,                      // Top-left corner
        w - 10,                 // Top-right corner
        (h / 2) * w + w / 2 - 5, // Center
        (h - 1) * w,            // Bottom-left corner
        (h - 1) * w + w - 10    // Bottom-right corner
    };

    for (int r = 0; r < 5; r++) {
        for (int i = 0; i < 20; i++) {
            int idx = sample_points[r] + i;
            if (idx >= 0 && idx < total) {
                // Check for near-white pixels (RGB565: 0xFFFF = pure white)
                // Threshold 0xF7BE allows for slight off-white (~248/252/248 RGB)
                if (pixels[idx] >= 0xF7BE) {
                    white_count++;
                }
            }
        }
    }

    // If >80% of samples are white, treat as placeholder
    return (white_count > 80);
}

// Worker-owned decode result. It is never written into a shared CachedTile slot
// directly; load_tile_to_slot() publishes it under cache_mutex only if the slot
// still belongs to this decode, and otherwise releases the buffer.
struct DecodedTile {
    uint8_t* data;
    uint32_t size;
    lv_image_dsc_t dsc;
};

// True when the map pack holds this tile's level; the levels above it come from loose files on the SD card.
static bool tile_is_in_pack(TileCoord coord) {
    return coord.z >= 1 && coord.z <= core_pak_levels;
}

// Reads one tile's QOI bytes into buf: every level the map pack has from the pack (one open, seek, read), levels
// above it from the SD card's loose detail files (/GeoQuakeLite/tiles/z{z}/{x}/{y}.qoi). The filesystem lock is
// held only for the open/read/close; the source is read once here (it only changes while no decode task runs, see
// tile_manager_set_tile_source).
static bool read_tile_bytes(TileCoord coord, uint8_t* buf, size_t buf_size, size_t* out_size, bool* from_sd) {
    *out_size = 0;
    if (!core_pak_ready || coord.z > max_tile_level) return false;
    char path[80];
    uint32_t offset = 0;
    size_t length = 0;
    bool on_sd = true;
    const bool in_pack = tile_is_in_pack(coord);
    if (in_pack) {
        const int slot = tile_pak::tile_slot(coord.z, coord.x, coord.y, core_pak_levels);
        if (slot < 0) return false;
        offset = s_core_index->entry[slot].offset;
        length = s_core_index->entry[slot].length;
        on_sd = core_pak_on_sd;
        snprintf(path, sizeof(path), "%s", TILE_PAK_PATH);
    } else {
        build_tile_path(path, sizeof(path), coord, "qoi");
    }
    *from_sd = on_sd;

    if (on_sd) {
        sd_lock();
    } else {
        flash_lock();
        if (lv_fs_flash_is_quiesced()) {
            flash_unlock();
            return false;
        }
    }
    File file = on_sd ? SD.open(path, FILE_READ) : LittleFS.open(path, FILE_READ);
    bool ok = false;
    if (file) {
        if (!in_pack) length = file.size();  // a loose file is one whole tile
        if (length > 0 && length <= buf_size && (!in_pack || file.seek(offset))) {
            ok = file.read(buf, length) == length;
        }
        file.close();
    }
    if (on_sd) {
        sd_unlock();
    } else {
        flash_unlock();
    }
    if (ok) *out_size = length;
    return ok;
}

// === QOI DECODER WRAPPER ===
// Read-then-decode pattern: the filesystem lock is held only during the read (~5-15ms),
// decode runs from RAM without lock, allowing parallel decode across the decode tasks.
// On success *out owns a pool buffer; on failure out->data is NULL.
static bool decode_tile_to_psram(TileCoord coord, DecodedTile* out, int expected_width, int expected_height, int task_id) {
    PERF_PROBE_SCOPE(dec, (task_id == 1) ? PP_DECODE_T1 : PP_DECODE_T0);
    out->data = NULL;
    out->size = 0;
    unsigned long t_start = micros();

    // QOI worst case: w*h*5 (QOI_OP_RGBA = 5 bytes/pixel) + 14 header + 8 end
    // Derive limit from expected dimensions to avoid silently dropping high-entropy tiles
    size_t max_qoi_size = (size_t)expected_width * expected_height * 5 + 22;  // QOI_OP_RGBA = 5 bytes/pixel worst case

    int buf_idx = (task_id >= 0 && task_id < NUM_DECODE_TASKS) ? task_id : 0;
    uint8_t* file_buf = s_qoi_scratch_buf[buf_idx];
    if (!file_buf) {
        return false;
    }

    // === Phase 1: read the tile under the filesystem lock (no malloc in the critical section) ===
    size_t file_size = 0;
    bool from_sd = false;
    const uint32_t read_started_us = micros();
    const size_t limit = max_qoi_size < QOI_SCRATCH_BUF_SIZE ? max_qoi_size : QOI_SCRATCH_BUF_SIZE;
    if (!read_tile_bytes(coord, file_buf, limit, &file_size, &from_sd)) {
        return false;
    }
    const uint32_t read_elapsed_us = micros() - read_started_us;
    if (from_sd) {
        const uint32_t read_kib_per_second =
            read_elapsed_us
                ? static_cast<uint32_t>(
                      (file_size * 1000000ULL) /
                      (read_elapsed_us * 1024ULL))
                : 0;
        Serial.printf(
            "[PERF] operation=tile_sd_read bytes=%u elapsed_us=%lu "
            "sd_read_kib_s=%lu\n",
            static_cast<unsigned>(file_size),
            static_cast<unsigned long>(read_elapsed_us),
            static_cast<unsigned long>(read_kib_per_second));
    }

    // === Phase 2: Validate QOI header before acquiring pool buffer ===
    // Early reject saves a pool slot for files that would fail decode anyway
    if (file_size < 22 ||
        file_buf[0] != 'q' || file_buf[1] != 'o' || file_buf[2] != 'i' || file_buf[3] != 'f') {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[QOI] Invalid header magic: z%d/%d_%d\n", coord.z, coord.x, coord.y);
        #endif
        return false;
    }
    {
        uint32_t hdr_w = ((uint32_t)file_buf[4] << 24) | ((uint32_t)file_buf[5] << 16) | ((uint32_t)file_buf[6] << 8) | file_buf[7];
        uint32_t hdr_h = ((uint32_t)file_buf[8] << 24) | ((uint32_t)file_buf[9] << 16) | ((uint32_t)file_buf[10] << 8) | file_buf[11];
        if ((int)hdr_w != expected_width || (int)hdr_h != expected_height) {
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[QOI] Dimension mismatch: z%d/%d_%d (got %ux%u, expected %dx%d)\n",
                          coord.z, coord.x, coord.y, hdr_w, hdr_h, expected_width, expected_height);
            #endif
            return false;
        }
    }

    // === Phase 3: Acquire pool buffer (NO SD lock) ===
    const size_t buffer_size = expected_width * expected_height * 2;
    PERF_PROBE_BEGIN(acq);
    uint8_t* buf = tile_pool_acquire();
    PERF_PROBE_END(acq, (task_id == 1) ? PP_ACQUIRE_T1 : PP_ACQUIRE_T0);
    if (!buf) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[QOI] Pool exhausted, attempting eviction recovery...\n");
        #endif
        for (int retry = 0; retry < 2 && !buf; retry++) {
            tile_cache_evict_lru();
            vTaskDelay(pdMS_TO_TICKS(5));
            buf = tile_pool_acquire();
        }

        if (!buf) {
            #if TILE_DEBUG_LEVEL >= 1
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            int pool_free = tile_pool_free_count();
            Serial.printf("[QOI] Failed to acquire buffer (Pool free: %d, PSRAM: %d KB)\n",
                          pool_free, free_psram / 1024);
            #endif
            return false;
        }
    }

    bool swap_bytes = false;
    int result = qoi_decode_from_memory(file_buf, file_size, buf,
                                         expected_width, expected_height, swap_bytes);

    if (!result) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[QOI] Decode failed: z%d/%d_%d\n", coord.z, coord.x, coord.y);
        #endif
        tile_pool_release(buf);
        return false;
    }

    // Cache flush for PSRAM-decoded tile data before LVGL reads it
    Cache_WriteBack_Addr((uint32_t)buf, buffer_size);

    // Setup LVGL descriptor
    out->data = buf;
    out->size = buffer_size;
    memset(&out->dsc, 0, sizeof(out->dsc));
    out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    out->dsc.header.w = expected_width;
    out->dsc.header.h = expected_height;
    out->dsc.header.stride = expected_width * 2;
    out->dsc.data_size = buffer_size;
    out->dsc.data = buf;

    #if TILE_DEBUG_LEVEL >= 2
    unsigned long t_elapsed = micros() - t_start;
    Serial.printf("[DECODE] SUCCESS (QOI): %dx%d decoded in %lu ms\n",
                  expected_width, expected_height, t_elapsed / 1000);
    #endif

    return true;
}

// --- Public API ---

void tile_manager_init() {
    if (manager_initialized) return;
    manager_initialized = true;
    #if TILE_DEBUG_LEVEL >= 2
    Serial.println("[TILE] Tile manager initialized (File Path Mode)");
    #endif
}

// Pure and thread-safe (used from the decode tasks for prefetch relevance).
// Shares world_from_latlon() with the renderer's ViewFrame, so the centre
// tile is identical on both sides, including the +180 clamp.
TileCoord latlon_to_tile(float lat, float lon, uint8_t z) {
    TileCoord coord;
    coord.z = z;
    if (z == 0 || z > 16) {
        coord.x = 0;
        coord.y = 0;
        return coord;
    }
    const WorldPoint p = world_from_latlon(lat, lon);
    coord.x = (uint16_t)(p.x >> (32 - z));
    coord.y = (uint16_t)(p.y >> (31 - z));
    return coord;
}

uint8_t zoom_to_tile_level(float zoom) {
    if (zoom < ZOOM_THRESHOLDS[1]) return 0;
    if (zoom < ZOOM_THRESHOLDS[2]) return 1;
    if (zoom < ZOOM_THRESHOLDS[3]) return 2;
    if (zoom < ZOOM_THRESHOLDS[4]) return 3;
    if (zoom < ZOOM_THRESHOLDS[5]) return 4;
    return 5;
}

float get_scale_within_level(float zoom, uint8_t z) {
    if (z >= MAX_ZOOM_LEVEL) z = MAX_ZOOM_LEVEL;
    return zoom / ZOOM_THRESHOLDS[z];
}

// === NEW TILE CACHE IMPLEMENTATION ===

// Find a tile in cache, returns index or -1 if not found
// FIX 4.3: TILE_FAILED tiles are not considered "found" so they can be replaced
// Note: TILE_NODATA tiles ARE considered found (shows black instead of requeue)
static int cache_find(TileCoord coord) {
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        TileState state = g_tile_cache.tiles[i].state;
        // Skip empty and permanently failed tiles (NODATA is NOT skipped)
        if (state == TILE_EMPTY || state == TILE_FAILED) {
            continue;
        }
        if (g_tile_cache.tiles[i].coord == coord) {
            return i;
        }
    }
    return -1;
}

// Find an empty slot in cache, returns index or -1 if full
// FIX 4.3: TILE_FAILED slots are considered empty (can be reused)
static int cache_find_empty() {
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        TileState state = g_tile_cache.tiles[i].state;
        if (state == TILE_EMPTY || state == TILE_FAILED) {
            return i;
        }
    }
    return -1;
}

// PHASE 1C: Centralized eviction helper (expects cache_lock held)
// Fully cleans up a slot and releases its buffer back to pool
static void evict_slot_locked(int slot) {
    CachedTile* t = &g_tile_cache.tiles[slot];
    if (t->state == TILE_EMPTY) return;  // Already empty

    uint8_t old_z = t->coord.z;
    uint16_t old_x = t->coord.x;
    uint16_t old_y = t->coord.y;

    // Release buffer back to pool
    if (t->decoded_data) {
        tile_pool_release(t->decoded_data);
    }

    // Full reset (generation preserved and bumped)
    slot_reset_locked(t);
    g_tile_cache.count--;

    #if TILE_DEBUG_LEVEL >= 2
    Serial.printf("[CACHE] Evicted slot %d (was z%d/%d_%d)\n", slot, old_z, old_x, old_y);
    #endif
}

// Find LRU tile (oldest last_used that's not priority or pinned)
static int cache_find_lru() {
    int lru_idx = -1;
    uint32_t max_age = 0;

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];

        // Skip non-evictable states
        if (t->state == TILE_EMPTY) continue;
        if (t->state == TILE_LOADING) continue;     // PHASE 1A: protect in-flight decode
        if (t->state == TILE_DISPLAYED) continue;

        // Priority zoom tiles are protected by LRU naturally — visible tiles
        // get last_used updated every frame, so they won't be evicted.
        // Removing blanket priority_z skip prevents cache stall when panning.

        // FIX #2: Skip tiles with refcount > 0 (pinned by renderer)
        // These tiles are actively being used and cannot be evicted
        if (t->refcount > 0) continue;

        // Wrap-safe age comparison (protects against 49-day millis() overflow)
        uint32_t age = millis() - t->last_used;
        if (age > max_age) {
            max_age = age;
            lru_idx = i;
        }
    }
    return lru_idx;
}

// FIX 2: Helper to check if coord is in a queue
static bool is_in_queue(TileCoord coord, TileCoord* queue, uint8_t head, uint8_t tail, uint8_t size) {
    uint8_t idx = head;
    while (idx != tail) {
        if (queue[idx] == coord) return true;
        idx = (idx + 1) % size;
    }
    return false;
}

static void queue_deferred_visible_miss(TileCoord coord) {
    if (!deferred_visible_mutex) return;
    if (xSemaphoreTake(deferred_visible_mutex, 0) != pdTRUE) {
        return;
    }

    if (is_in_queue(coord, deferred_visible_queue, deferred_visible_head,
                    deferred_visible_tail, DEFERRED_VISIBLE_QUEUE_SIZE)) {
        xSemaphoreGive(deferred_visible_mutex);
        return;
    }

    uint8_t next_tail = (deferred_visible_tail + 1) % DEFERRED_VISIBLE_QUEUE_SIZE;
    if (next_tail != deferred_visible_head) {
        deferred_visible_queue[deferred_visible_tail] = coord;
        deferred_visible_tail = next_tail;
    }

    xSemaphoreGive(deferred_visible_mutex);

    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (decode_task_handles[i]) {
            xTaskNotifyGive(decode_task_handles[i]);
        }
    }
}

static void drain_deferred_visible_locked() {
    if (!deferred_visible_mutex) return;
    if (xSemaphoreTake(deferred_visible_mutex, 0) != pdTRUE) {
        return;
    }

    TileCoord local[DEFERRED_VISIBLE_QUEUE_SIZE];
    int local_count = 0;
    while (deferred_visible_head != deferred_visible_tail &&
           local_count < DEFERRED_VISIBLE_QUEUE_SIZE) {
        local[local_count++] = deferred_visible_queue[deferred_visible_head];
        deferred_visible_head = (deferred_visible_head + 1) % DEFERRED_VISIBLE_QUEUE_SIZE;
    }

    xSemaphoreGive(deferred_visible_mutex);

    for (int i = 0; i < local_count; i++) {
        queue_tile_load_priority(local[i], true);
    }
}

static void clear_deferred_visible_queue() {
    if (!deferred_visible_mutex) return;
    if (xSemaphoreTake(deferred_visible_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        deferred_visible_head = 0;
        deferred_visible_tail = 0;
        xSemaphoreGive(deferred_visible_mutex);
    }
}

static void clear_prefetch_queue_locked() {
    prefetch_queue_head = 0;
    prefetch_queue_tail = 0;
}

// FIX 2: Add tile to appropriate priority queue (expects cache_lock held)
// is_visible=true: render request (high priority)
// is_visible=false: prefetch request (low priority)
static void queue_tile_load_priority(TileCoord coord, bool is_visible) {
    // Skip if already cached or loading
    if (cache_find(coord) >= 0) {
        #if TILE_DEBUG_LEVEL >= 2
        Serial.printf("[QUEUE] SKIP (in cache): z%d/%d_%d\n", coord.z, coord.x, coord.y);
        #endif
        return;
    }

    // FIX #7: Respect adaptive cache limits
    // Don't queue if we've hit the effective max for current memory state
    if (!tile_cache_can_load_more()) {
        if (!is_visible) {
            return;  // Memory pressure - only skip prefetch tiles
        }
        // Visible tiles always proceed - force eviction if needed
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[CACHE] Forcing eviction for visible tile z%d/%d_%d\n",
                      coord.z, coord.x, coord.y);
        #endif
        tile_cache_evict_lru_locked();  // Already under cache_lock - use locked version
    }

    // Check if already in either queue
    if (is_in_queue(coord, visible_queue, visible_queue_head, visible_queue_tail, VISIBLE_QUEUE_SIZE)) return;
    if (is_in_queue(coord, prefetch_queue, prefetch_queue_head, prefetch_queue_tail, PREFETCH_QUEUE_SIZE)) return;

    // FIX: Removed sd_file_exists() check that blocked for 10-100ms under cache_lock
    // decode_tile_to_psram() handles missing tiles gracefully (returns false fast)
    // This eliminates a major source of UI stalls during rapid panning

    // Add to appropriate queue
    if (is_visible) {
        uint8_t next_tail = (visible_queue_tail + 1) % VISIBLE_QUEUE_SIZE;
        if (next_tail == visible_queue_head) {
            // Visible queue full - this shouldn't happen often (only 9 visible tiles)
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[TELEMETRY] VISIBLE queue FULL - z%d/%d_%d\n", coord.z, coord.x, coord.y);
            #endif
            return;
        }
        visible_queue[visible_queue_tail] = coord;
        visible_queue_tail = next_tail;
        #if TILE_TELEMETRY_LEVEL >= 2
        int queue_len = (visible_queue_tail - visible_queue_head + VISIBLE_QUEUE_SIZE) % VISIBLE_QUEUE_SIZE;
        Serial.printf("[QUEUE] +visible z%d/%d_%d (queue: %d/%d)\n",
                      coord.z, coord.x, coord.y, queue_len, VISIBLE_QUEUE_SIZE);
        #endif
    } else {
        uint8_t next_tail = (prefetch_queue_tail + 1) % PREFETCH_QUEUE_SIZE;
        if (next_tail == prefetch_queue_head) {
            // Prefetch queue full - drop silently (prefetch is best-effort)
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[TELEMETRY] Prefetch queue FULL - z%d/%d_%d dropped\n", coord.z, coord.x, coord.y);
            #endif
            return;
        }
        prefetch_queue[prefetch_queue_tail] = coord;
        prefetch_queue_tail = next_tail;
        #if TILE_TELEMETRY_LEVEL >= 2
        int queue_len = (prefetch_queue_tail - prefetch_queue_head + PREFETCH_QUEUE_SIZE) % PREFETCH_QUEUE_SIZE;
        Serial.printf("[QUEUE] +prefetch z%d/%d_%d (queue: %d/%d)\n",
                      coord.z, coord.x, coord.y, queue_len, PREFETCH_QUEUE_SIZE);
        #endif
    }

    // Notify ALL decode tasks — multiple notifications are harmless (extra wakeup)
    // Round-robin left idle tasks sleeping while work was available
    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (decode_task_handles[i]) {
            xTaskNotifyGive(decode_task_handles[i]);
        }
    }
}

// C1: Wake all decode tasks (used by drain)
static void wake_decode_tasks() {
    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (decode_task_handles[i]) {
            xTaskNotifyGive(decode_task_handles[i]);
        }
    }
}

// Legacy wrapper for existing callers (defaults to prefetch priority)
static void queue_tile_load_locked(TileCoord coord) {
    queue_tile_load_priority(coord, false);
}

// PHASE 3: Helper to count tiles at a specific zoom level
static int tile_cache_count_zoom(uint8_t z) {
    int count = 0;
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (g_tile_cache.tiles[i].state != TILE_EMPTY &&
            g_tile_cache.tiles[i].coord.z == z) {
            count++;
        }
    }
    return count;
}

// Reserve a cache slot for a tile (expects cache_lock held)
static bool reserve_slot_for_tile(TileCoord coord, int* out_slot) {
    *out_slot = -1;

    if (cache_find(coord) >= 0) return false;

    int slot = cache_find_empty();
    if (slot < 0) {
        // PHASE 3: Priority override for current zoom level (9-tile guarantee)
        // If this tile is at the priority zoom and we have <9 tiles at that zoom,
        // we MUST evict to make room for the visible grid
        bool force_evict = false;
        if (priority_z >= 0 && coord.z == priority_z) {
            int current_z_count = tile_cache_count_zoom(priority_z);
            if (current_z_count < 9) {
                force_evict = true;
                #if TILE_DEBUG_LEVEL >= 2
                Serial.printf("[CACHE] Priority override: z%d has only %d tiles (need 9)\n",
                              priority_z, current_z_count);
                #endif
            }
        }

        if (!tile_cache_can_allocate() || force_evict) {
            // Evict until we have enough memory (max 12 tiles to prevent infinite loop)
            // Increased from 8 to handle severe fragmentation
            int evicted = 0;
            int initial_count = g_tile_cache.count;
            while ((!tile_cache_can_allocate() || (force_evict && slot < 0)) && evicted < 12) {
                tile_cache_evict_lru_locked();
                evicted++;
                // Stop if no more tiles to evict
                if (g_tile_cache.count == 0) break;
                // Check if we now have an empty slot
                slot = cache_find_empty();
                if (slot >= 0 && force_evict) break;  // Got our slot for priority tile
                // FIX #5: Yield between eviction attempts to prevent livelock and watchdog timeout
                // Release lock temporarily to allow other tasks to run
                cache_unlock();
                vTaskDelay(1);  // 1 tick (~1ms) backoff
                cache_lock();
            }
            #if TILE_DEBUG_LEVEL >= 2
            if (evicted > 0) {
                Serial.printf("[CACHE] Evicted %d tiles to free PSRAM%s\n",
                              evicted, force_evict ? " (priority override)" : "");
            }
            #endif
        }

        // Try to find a slot again after eviction
        if (slot < 0) {
            slot = cache_find_lru();
        }

        if (slot >= 0) {
            CachedTile* t = &g_tile_cache.tiles[slot];
            // FIX #1 & #2: Check refcount before freeing to prevent double-free
            // If refcount > 0, someone is still using this tile's memory
            if (t->refcount > 0) {
                #if TILE_DEBUG_LEVEL >= 1
                Serial.printf("[CACHE] WARNING: Skipping eviction of slot %d - refcount=%d\n", slot, t->refcount);
                #endif
                slot = -1;  // Can't use this slot, try again
            } else {
                tile_clear_locked(t, TILE_EMPTY);
                g_tile_cache.count--;
            }
        }
    }

    // PHASE 4: Handle "no evictable slots" gracefully
    if (slot < 0) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[CACHE] No evictable slots for z%d/%d_%d (all busy/pinned)\n",
                      coord.z, coord.x, coord.y);
        #endif
        return false;
    }

    CachedTile* t = &g_tile_cache.tiles[slot];
    // FIX #1: Increment generation on slot reuse to detect stale references
    t->generation++;
    t->coord = coord;
    t->state = TILE_LOADING;
    t->decoded_data = NULL;
    t->decoded_size = 0;
    t->refcount = 0;  // FIX #2: Initialize refcount
    t->last_used = millis();
    t->load_started = millis();  // Timestamp for LOADING timeout detection
    t->failure_count = 0;  // FIX 4.3: Reset failure tracking
    t->retry_after = 0;    // FIX 4.3: Reset retry timer
    memset(&t->img_dsc, 0, sizeof(t->img_dsc));
    t->path[0] = '\0';
    // Claimed by the calling decode worker (see load_tile_to_slot)
    t->decoding = true;
    s_decodes_in_flight.fetch_add(1, std::memory_order_relaxed);

    *out_slot = slot;
    return true;
}

static bool prefetch_still_relevant(TileCoord coord) {
    if (coord.z != current_z) {
        return false;
    }

    TileCoord center = latlon_to_tile(view_center_lat, view_center_lon, current_z);
    uint32_t world = (uint32_t)1 << current_z;
    int dx = (int)coord.x - (int)center.x;
    int dy = (int)coord.y - (int)center.y;

    if (dx > (int)(world / 2)) dx -= (int)world;
    if (dx < -(int)(world / 2)) dx += (int)world;

    // Direction prefetch queues at most three tiles ahead and one behind; edge
    // preload uses two tiles. Keep a 3-tile radius so useful queued prefetch
    // survives modest pan motion but stale high-zoom work is dropped.
    return abs(dx) <= 3 && abs(dy) <= 3;
}

// Task watchdog: the decoders drain the queue back to back on core 0 (SD reads poll the bus, QOI decode is pure
// CPU), and IDLE0 (priority 0) must run at least every few seconds. Two decoders at priority 1 can take turns so that
// one is always running, so only decoder 0 runs at priority 1 and it sleeps two ticks after 50 ms of work: with no
// other priority-1 task on core 0, that sleep is a real gap for IDLE0 and for idle-priority work such as QuakeTask.
// The other decoders run at idle priority, take only prefetch tiles and use the time decoder 0 leaves. (They share
// one core, so they never decoded in parallel; the sleep costs about 4% of decoder 0's time.)
constexpr int64_t kDecoderRestEveryUs = 50000;

static void tile_decode_task(void* param) {
    int task_id = (int)(intptr_t)param;  // Task ID: 0, 1, or 2
    int64_t rested_us = esp_timer_get_time();  // decoder 0: when it last slept on purpose

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (cache_shutdown_requested) {
            // Park instead of self-deleting: tile_cache_deinit() deletes parked
            // workers itself, so no notifier ever holds the handle of a deleted task.
            s_decode_tasks_alive.fetch_sub(1, std::memory_order_release);
            vTaskSuspend(NULL);
        }

        // C1: If drain requested, skip tile processing — go back to waiting.
        // The drain function clears queues and waits for in-flight decodes.
        if (drain_requested) {
            continue;
        }

        for (;;) {
            // C1: Re-check drain_requested inside inner loop — catches mid-batch.
            if (drain_requested) break;
            // Every path through this loop passes here (outside the locks).
            if (task_id == 0 && esp_timer_get_time() - rested_us >= kDecoderRestEveryUs) {
                vTaskDelay(2);
                rested_us = esp_timer_get_time();
            }

            TileCoord coord;
            int slot = -1;
            bool is_retry = false;
            bool is_prefetch = false;
            uint32_t claim_generation = 0;  // read under the same lock as the claim

            cache_lock();
            // tile_cache_deinit() and tile_cache_drain() set their flag under this
            // lock, so once they have, no new claim can start (claims are made
            // inside this critical section).
            if (cache_shutdown_requested || drain_requested) {
                cache_unlock();
                break;
            }
            drain_deferred_visible_locked();

            // FIX 2: Priority dequeue - RETRIES first (highest priority - tiles user saw but got evicted during decode), then VISIBLE, then PREFETCH
            bool got_tile = false;

            // Tiles the user sees (retries, visible) go to decoder 0, the full-speed one; idle-priority decoders
            // take prefetch tiles (one already being decoded may still scroll into view and arrive later). If
            // decoder 0 could not be created, the others take everything (see kDecoderRestEveryUs).
            const bool may_take_visible = (task_id == 0) || decode_task_handles[0] == nullptr;

            // 1. Check retries FIRST (highest priority - tiles user saw but got evicted during decode)
            for (int i = 0; may_take_visible && i < TILE_CACHE_MAX; i++) {
                CachedTile* t = &g_tile_cache.tiles[i];
                if (t->state == TILE_LOADING && t->failure_count > 0 && !t->decoding) {
                    // Found a retry tile - claim it for this task (under lock, so
                    // the other worker cannot pick the same slot)
                    t->decoding = true;
                    s_decodes_in_flight.fetch_add(1, std::memory_order_relaxed);
                    claim_generation = t->generation;
                    coord = t->coord;
                    slot = i;
                    got_tile = true;
                    is_retry = true;
                    #if TILE_TELEMETRY_LEVEL >= 2
                    Serial.printf("[QUEUE] -retry z%d/%d_%d -> task%d (retry priority)\n",
                                  coord.z, coord.x, coord.y, task_id);
                    #endif
                    break;
                }
            }

            // 2. Then check visible queue (high priority)
            if (!got_tile && may_take_visible && visible_queue_head != visible_queue_tail) {
                coord = visible_queue[visible_queue_head];
                visible_queue_head = (visible_queue_head + 1) % VISIBLE_QUEUE_SIZE;
                got_tile = true;
                #if TILE_TELEMETRY_LEVEL >= 2
                Serial.printf("[QUEUE] -dequeue z%d/%d_%d -> task%d (visible)\n",
                              coord.z, coord.x, coord.y, task_id);
                #endif
            }
            // 3. Then check prefetch queue (low priority)
            else if (!got_tile && prefetch_queue_head != prefetch_queue_tail) {
                coord = prefetch_queue[prefetch_queue_head];
                prefetch_queue_head = (prefetch_queue_head + 1) % PREFETCH_QUEUE_SIZE;
                got_tile = true;
                is_prefetch = true;
                #if TILE_TELEMETRY_LEVEL >= 2
                Serial.printf("[QUEUE] -dequeue z%d/%d_%d -> task%d (prefetch)\n",
                              coord.z, coord.x, coord.y, task_id);
                #endif
            }

            if (got_tile && is_prefetch && !prefetch_still_relevant(coord)) {
                cache_unlock();
                #if TILE_TELEMETRY_LEVEL >= 2
                Serial.printf("[QUEUE] stale prefetch skipped z%d/%d_%d -> task%d\n",
                              coord.z, coord.x, coord.y, task_id);
                #endif
                continue;
            }

            // If retry tile already has slot, skip reservation
            if (got_tile && is_retry) {
                cache_unlock();
                // Retry tiles already have a slot assigned, proceed to load
            } else if (got_tile) {
                bool reserved = reserve_slot_for_tile(coord, &slot);
                if (reserved) {
                    claim_generation = g_tile_cache.tiles[slot].generation;
                }
                cache_unlock();

                if (!reserved) {
                    // Re-queue so tile isn't silently lost
                    cache_lock();
                    queue_tile_load_priority(coord, true);  // visible priority
                    cache_unlock();
                    // No free slot yet: back off (outside the locks) instead of retrying in a hot loop.
                    vTaskDelay(1);
                    continue;
                }
            } else {
                cache_unlock();
                break;  // Nothing to do, wait for next notification
            }

            if (load_tile_to_slot(coord, slot, task_id, claim_generation)) {
                #if TILE_DEBUG_LEVEL >= 2
                Serial.printf("[CACHE] Task%d %s z%d/%d_%d to slot %d\n",
                              task_id, is_retry ? "retried" : "loaded",
                              coord.z, coord.x, coord.y, slot);
                #endif
            } else {
                // CRITICAL: Always yield on failure to prevent watchdog timeout
                // Allocation failures (PSRAM exhaustion, missing files) need backoff
                vTaskDelay(pdMS_TO_TICKS(10));  // 10ms backoff on failure
            }
            // Removed unconditional 1ms yield - adds 1ms per tile unnecessarily (pacing: decoder 0, top of loop)
        }
    }
}

// Build tile path - uses hashed directories for z5+ to speed up FAT32 lookups
// z0-z4: /GeoQuakeLite/tiles/z{z}/{x}_{y}.ext  (flat - few files per folder)
// z5-z6: /GeoQuakeLite/tiles/z{z}/{x}/{y}.ext  (hashed by X - ~64 files per folder)
static void build_tile_path(char* out_path, size_t out_size, TileCoord coord, const char* ext) {
    if (coord.z >= 5) {
        // Hashed directory structure for high zoom levels
        snprintf(out_path, out_size, "/GeoQuakeLite/tiles/z%d/%d/%d.%s",
                 coord.z, coord.x, coord.y, ext);
    } else {
        // Flat directory structure for low zoom levels
        snprintf(out_path, out_size, "/GeoQuakeLite/tiles/z%d/%d_%d.%s",
                 coord.z, coord.x, coord.y, ext);
    }
}

// Load a single QOI tile into a cache slot this worker has claimed
// (t->decoding set under cache lock at reserve or retry pickup; claim_generation
// read under that same lock). The decode fills a worker-owned buffer; nothing in
// the shared slot changes until the result is published under cache_mutex, and
// only while the slot still belongs to this claim. Any cancel/reuse of the slot
// bumps its generation, so a stale result is simply released.
static bool load_tile_to_slot(TileCoord coord, int slot, int task_id, uint32_t claim_generation) {
    CachedTile* t = &g_tile_cache.tiles[slot];

    // TELEMETRY: Start timing for decode stats (always track for health monitor)
    unsigned long t_decode_start = micros();

    // Tile size is always 256px (z0 code removed - grid starts at z1)
    const int tile_size = 256;

    // QOI-only mode: file existence already verified in queue_tile_load_priority()
    // Skip redundant FAT32 lookup - decode_tile_to_psram handles missing tiles gracefully
    DecodedTile out;
    const bool success = decode_tile_to_psram(coord, &out, tile_size, tile_size, task_id);

    // === NODATA DETECTION === (on our own buffer, before anything is published)
    // Check if this tile is a blank/placeholder (mostly white "No Data" text).
    // Fill with black instead of displaying white placeholder images.
    bool is_nodata = false;
    if (success && is_tile_blank_placeholder(out.data, tile_size, tile_size)) {
        // Fill entire tile with black (RGB565 0x0000)
        memset(out.data, 0, out.size);
        is_nodata = true;
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[TILE] NODATA detected: z%d/%d_%d (filled with black)\n",
                      coord.z, coord.x, coord.y);
        #endif
        // Re-flush cache after memset
        Cache_WriteBack_Addr((uint32_t)out.data, out.size);
    }

    cache_lock();

    // PHASE 1B: the slot must still belong to this claim. Generation catches
    // reuse and cancellation; the other checks guard against any path that
    // might have published into the slot meanwhile.
    const bool still_ours = (t->generation == claim_generation) && t->decoding &&
                            (t->state == TILE_LOADING) && (t->coord == coord) &&
                            (t->decoded_data == NULL);
    if (!still_ours) {
        cache_unlock();
        if (out.data) {
            tile_pool_release(out.data);
        }
        #if TILE_DEBUG_LEVEL >= 1
        Serial.printf("[CACHE] Stale decode discarded z%d/%d_%d (gen %lu)\n",
                      coord.z, coord.x, coord.y, (unsigned long)claim_generation);
        #endif
        s_decodes_in_flight.fetch_sub(1, std::memory_order_release);
        return false;
    }
    t->decoding = false;

    if (!success) {
        // FIX 4.3: Track decode failures with exponential backoff
        // Retry up to 3 times before giving up permanently
        t->failure_count++;
        if (t->failure_count < 3) {
            // Schedule retry with exponential backoff: 1s, 2s, 4s
            t->retry_after = millis() + (1000 * (1 << (t->failure_count - 1)));
            t->state = TILE_RETRY_PENDING;
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[CACHE] Tile z%d/%d_%d decode failed (attempt %d), retry after %lu ms\n",
                          coord.z, coord.x, coord.y, t->failure_count,
                          (1000 * (1 << (t->failure_count - 1))));
            #endif
        } else {
            // Give up after 3 failures
            t->state = TILE_FAILED;
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[CACHE] Tile z%d/%d_%d permanently failed after %d attempts\n",
                          coord.z, coord.x, coord.y, t->failure_count);
            #endif
        }
        // Nothing was published; state set above (RETRY_PENDING or FAILED)
        TileState keep_state = t->state;
        tile_clear_locked(t, keep_state);
        cache_unlock();
        s_decodes_in_flight.fetch_sub(1, std::memory_order_release);
        return false;
    }

    // Publish the worker-owned result into the slot (under cache lock)
    t->decoded_data = out.data;
    t->decoded_size = out.size;
    t->img_dsc = out.dsc;

    // Set state based on whether this is a NODATA tile
    if (is_nodata) {
        t->state = TILE_NODATA;
        t->last_used = millis();
    } else {
        tile_publish_locked(t);
    }
    if (tile_is_in_pack(coord)) {
        snprintf(t->path, sizeof(t->path), "%s:z%d/%d_%d", core_pak_on_sd ? "S:core.pak" : "F:core.pak",
                 coord.z, coord.x, coord.y);
    } else {
        snprintf(t->path, sizeof(t->path), "S:");
        build_tile_path(t->path + 2, sizeof(t->path) - 2, coord, "qoi");
    }
    g_tile_cache.count++;

    // TELEMETRY: Record decode timing per zoom level (always track for health monitor)
    uint32_t decode_ms = (micros() - t_decode_start) / 1000;
    if (coord.z <= 6) {
        decode_time_sum[coord.z] += decode_ms;
        decode_count[coord.z]++;
        #if TILE_TELEMETRY_LEVEL >= 1
        uint32_t avg = decode_time_sum[coord.z] / decode_count[coord.z];
        Serial.printf("[TIMING] z%d decode: %lums (avg: %lums, count: %lu)\n",
                      coord.z, decode_ms, avg, decode_count[coord.z]);
        #endif
    }

    #if TILE_DEBUG_LEVEL >= 2
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[CACHE] Loaded z%d/%d_%d to slot %d, PSRAM free: %d KB\n",
                  coord.z, coord.x, coord.y, slot, free_psram / 1024);
    #endif
    // FIX #4: Use atomic store with release semantics to ensure all prior writes
    // (tile data, state) are visible before flag is set
    cache_dirty.store(true, std::memory_order_release);
    cache_unlock();
    s_decodes_in_flight.fetch_sub(1, std::memory_order_release);
    return true;
}

void tile_cache_init() {
    // A deinit that timed out (a decode stuck for over 3 s) left the cache shut
    // down but initialized; finish it here so tiles can come back.
    if (cache_initialized && cache_shutdown_requested) {
        tile_cache_deinit();
    }
    if (cache_initialized) return;

    memset(&g_tile_cache, 0, sizeof(g_tile_cache));
    cache_shutdown_requested = false;
    // FIX 2: Initialize dual priority queues
    visible_queue_head = 0;
    visible_queue_tail = 0;
    prefetch_queue_head = 0;
    prefetch_queue_tail = 0;
    deferred_visible_head = 0;
    deferred_visible_tail = 0;
    priority_z = -1;
    if (!cache_mutex) {
        cache_mutex = xSemaphoreCreateMutex();  // kept across deinit/init cycles
    }
    if (!deferred_visible_mutex) {
        deferred_visible_mutex = xSemaphoreCreateMutex();
    }

    // FIX #6: Initialize preallocated tile buffer pool
    // This must happen before any tile loading to prevent fragmentation
    if (!tile_pool_init()) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.println("[CACHE] WARNING: Tile pool init failed, using on-demand allocation");
        #endif
    }

    cache_initialized = true;

    // Startup calibration: Record initial effective max for telemetry
    initial_effective_max = tile_cache_get_effective_max();

    #if TILE_DEBUG_LEVEL >= 2
    Serial.println("[CACHE] Tile cache initialized");
    Serial.printf("[CACHE] PSRAM free: %d KB, Pool buffers: %d free\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
                  tile_pool_free_count());
    Serial.printf("[CACHE] Initial adaptive max: %d tiles (UI reserved: %.1f MB)\n",
                  initial_effective_max, UI_RESERVED_MEMORY / (1024.0f * 1024.0f));
    #endif

    // Create configured parallel decode tasks.
    // 16KB stack needed for 8KB QOI buffer + local variables
    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (!decode_task_handles[i]) {
            char task_name[12];
            snprintf(task_name, sizeof(task_name), "TileDec%d", i);
            // Pin decode tasks exclusively to Core 0 so Core 1 remains 100% dedicated to LVGL rendering & touch
            // Only decoder 0 above idle priority (task watchdog; see kDecoderRestEveryUs).
            const UBaseType_t priority = (i == 0) ? 1 : tskIDLE_PRIORITY;
            if (xTaskCreatePinnedToCore(tile_decode_task, task_name, 16384, (void*)(intptr_t)i, priority,
                                        &decode_task_handles[i], 0) == pdPASS) {
                s_decode_tasks_alive.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    #if TILE_DEBUG_LEVEL >= 2
    Serial.printf("[CACHE] Created %d parallel decode tasks\n", NUM_DECODE_TASKS);
    #endif
}

void tile_cache_deinit() {
    if (!cache_initialized) {
        tile_pool_deinit();
        return;
    }

    // Under the lock: workers check the flag in the same critical section as
    // their next claim, so no decode can start once this block is done.
    cache_lock();
    cache_shutdown_requested = true;
    drain_requested = false;
    visible_queue_head = visible_queue_tail = 0;
    prefetch_queue_head = prefetch_queue_tail = 0;
    cache_unlock();
    clear_deferred_visible_queue();

    wake_decode_tasks();

    // Wait for claimed decodes to finish and for every decode task to exit: the
    // pools and scratch buffers freed below are theirs. Count claims, not slot
    // states: a cancelled slot is EMPTY while its worker may still hold a pool
    // buffer, and a queued retry stays LOADING with nobody decoding it.
    uint32_t start = millis();
    bool busy = true;
    while (busy && (millis() - start) < 3000) {
        busy = s_decodes_in_flight.load(std::memory_order_acquire) > 0 ||
               s_decode_tasks_alive.load(std::memory_order_acquire) > 0;
        if (busy) vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (busy) {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.println("[CACHE] Deinit deferred: decode still in flight");
        #endif
        return;
    }

    // Every worker is parked: delete them here (deinit owns the handles).
    for (int i = 0; i < NUM_DECODE_TASKS; i++) {
        if (decode_task_handles[i]) {
            vTaskDelete(decode_task_handles[i]);
            decode_task_handles[i] = NULL;
        }
    }

    cache_lock();
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        if (g_tile_cache.tiles[i].decoded_data) {
            tile_pool_release(g_tile_cache.tiles[i].decoded_data);
        }
    }
    memset(&g_tile_cache, 0, sizeof(g_tile_cache));
    visible_queue_head = visible_queue_tail = 0;
    prefetch_queue_head = prefetch_queue_tail = 0;
    priority_z = -1;
    cache_unlock();

    tile_pool_deinit();
    cache_initialized = false;
}

// C1: Drain decode workers — prevents new jobs and blocks until in-flight
// decodes complete (with timeout). Clears all work queues so decode tasks
// see nothing to do and go back to waiting on ulTaskNotifyTake.
// Call before cache clear when a filesystem remount invalidates tile data.
// Returns true if all decodes completed before timeout.
bool tile_cache_drain(uint32_t timeout_ms) {
    if (!cache_initialized) return true;

    Serial.println("[CACHE] Draining decode workers...");

    // 1-2. Flag the drain and clear the queues under the lock: workers check the
    // flag in the same critical section as their next claim, so afterwards the
    // only decodes left are already-claimed ones (s_decodes_in_flight).
    cache_lock();
    drain_requested = true;
    clear_prefetch_queue_locked();
    visible_queue_head = 0;
    visible_queue_tail = 0;
    cache_unlock();
    clear_deferred_visible_queue();

    // 3. Wake any sleeping decode tasks so they see the flag
    wake_decode_tasks();

    // 4. Wait for in-flight decodes to complete
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        // Count claimed decodes, not LOADING slots: a queued retry stays LOADING
        // with nobody decoding it, which would only run this wait into its timeout.
        bool any_loading = s_decodes_in_flight.load(std::memory_order_acquire) > 0;
        if (!any_loading) {
            Serial.printf("[CACHE] Drain complete (%lu ms)\n", millis() - start);
            drain_requested = false;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Timeout — some decode still running
    Serial.printf("[CACHE] Drain TIMEOUT after %lu ms (decode still in flight)\n", timeout_ms);
    drain_requested = false;
    return false;
}

const char* tile_cache_get(TileCoord coord) {
    if (!cache_initialized) tile_cache_init();

    #if TILE_DEBUG_LEVEL >= 2
    unsigned long t_cache_start = micros();
    #endif

    cache_lock();
    int idx = cache_find(coord);
    if (idx >= 0) {
        CachedTile* t = &g_tile_cache.tiles[idx];
        // Include TILE_NODATA - it has valid decoded_data
        if (t->state == TILE_READY || t->state == TILE_DISPLAYED || t->state == TILE_NODATA) {
            t->last_used = millis();
            g_tile_cache.hits++;
            #if TILE_DEBUG_LEVEL >= 2
            unsigned long t_cache_lookup = micros() - t_cache_start;
            Serial.printf("[PERF] Cache HIT z%d/%d_%d: %lu us\n",
                          coord.z, coord.x, coord.y, t_cache_lookup);
            #endif
            const char* path = t->path;
            cache_unlock();
            return path;
        }
        cache_unlock();
        return NULL;
    }

    #if TILE_DEBUG_LEVEL >= 2
    unsigned long t_cache_lookup = micros() - t_cache_start;
    Serial.printf("[PERF] Cache MISS z%d/%d_%d: %lu us\n",
                  coord.z, coord.x, coord.y, t_cache_lookup);
    #endif

    // Not in cache - queue for loading
    // FIX 2: Use HIGH priority - this is a visible tile (render miss)
    g_tile_cache.misses++;
    queue_tile_load_priority(coord, true);
    cache_unlock();
    return NULL;
}

bool tile_cache_is_ready(TileCoord coord) {
    cache_lock();
    int idx = cache_find(coord);
    if (idx < 0) {
        cache_unlock();
        return false;
    }
    TileState state = g_tile_cache.tiles[idx].state;
    cache_unlock();
    // TILE_NODATA is considered "ready" - it has valid black-filled buffer
    return (state == TILE_READY || state == TILE_DISPLAYED || state == TILE_NODATA);
}

CachedTile* tile_cache_get_tile(TileCoord coord) {
    if (!cache_initialized) tile_cache_init();

    // FIX: Non-blocking lock (0ms timeout) to prevent watchdog timeout during bulk tile decode
    // Root cause: At z3 (64 tiles), Core 0's tile_decode_task holds mutex for 1-1.6s per decode.
    // With 10ms timeout, LVGL on Core 1 accumulated stalls during rapid zoom, exceeding 5s watchdog.
    // Solution: Never wait - if mutex busy, return nullptr and render placeholder (black tile).
    // Tiles continue loading in background; next frame will pick them up when ready.
    if (!cache_try_lock(0)) {
        queue_deferred_visible_miss(coord);
        return nullptr;  // Mutex busy - skip this tile, show placeholder
    }

    int idx = cache_find(coord);
    if (idx >= 0) {
        CachedTile* t = &g_tile_cache.tiles[idx];
        // Include TILE_NODATA - it has valid decoded_data (black-filled buffer)
        if ((t->state == TILE_READY || t->state == TILE_DISPLAYED || t->state == TILE_NODATA) && t->decoded_data) {
            t->last_used = millis();
            // FIX #2: Increment refcount to pin tile during rendering
            // Caller MUST call tile_cache_release() when done to allow eviction
            t->refcount++;
            g_tile_cache.hits++;
            cache_unlock();
            return t;
        }
        // DIAGNOSTIC: Tile found in cache but not ready - stuck in intermediate state
        // Use age-based milestone logging instead of simple throttle
        #if TILE_TELEMETRY_LEVEL >= 1
        uint32_t age = millis() - t->load_started;
        uint32_t milestone = 0;
        if (age > 30000) milestone = 30000;
        else if (age > 10000) milestone = 10000;
        else if (age > 2000) milestone = 2000;
        else if (age > 500) milestone = 500;

        if (milestone > 0 && t->last_logged_milestone != milestone) {
            t->last_logged_milestone = milestone;
            Serial.printf("[CACHE] STUCK tile z%d/%d_%d state=%d for %lums\n",
                          coord.z, coord.x, coord.y, t->state, age);
        }
        #endif
        cache_unlock();
        return NULL;
    }

    g_tile_cache.misses++;
    // FIX 2: Use HIGH priority - this is a visible tile (render miss)
    queue_tile_load_priority(coord, true);
    cache_unlock();
    return NULL;
}

// FIX #2: Release a tile after rendering is complete
void tile_cache_release(CachedTile* tile) {
    if (!tile) return;

    cache_lock();
    // Decrement refcount to allow eviction
    if (tile->refcount > 0) {
        tile->refcount--;
    } else {
        #if TILE_DEBUG_LEVEL >= 1
        Serial.println("[CACHE] WARNING: tile_cache_release called with refcount <= 0");
        #endif
    }
    cache_unlock();
}

// True when coord is a tile of the view centred on `center`: the 3x3 around it (x wraps), every tile at z1.
static bool tile_in_view(TileCoord coord, TileCoord center) {
    if (coord.z != center.z) return false;
    if (center.z == 1) return true;
    const int n = 1 << center.z;
    int dx = (int)coord.x - (int)center.x;
    if (dx > n / 2) dx -= n;
    if (dx < -n / 2) dx += n;
    const int dy = (int)coord.y - (int)center.y;
    return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

// Keeps, in one of the two request queues, only the requests for tiles of the view at `center`.
static void keep_requests_of_view(TileCoord* queue, uint8_t head, uint8_t* tail, uint8_t size, TileCoord center) {
    uint8_t out = head;
    for (uint8_t i = head; i != *tail; i = (i + 1) % size) {
        if (tile_in_view(queue[i], center)) {
            queue[out] = queue[i];
            out = (out + 1) % size;
        }
    }
    *tail = out;
}

// One tile of the view being preloaded (cache lock held): into the VISIBLE queue while the cache has room. At its
// adaptive limit a visible request evicts the least recently used tile, which can be a tile of this same view: that
// choice stays where it was, with the renderer's own request for a tile it is about to draw.
static void queue_view_tile_locked(TileCoord coord) {
    if (tile_cache_can_load_more()) queue_tile_load_priority(coord, true);
}

// Queues the tiles of the view at (z, centre): the nine on screen, centre first, in the VISIBLE queue, so decoder 0
// loads them in that order. (As prefetch, the idle-priority decoder could claim one, usually the centre, and,
// preempted by decoder 0 for the other eight, deliver it last.) A call is a new view: visible requests still queued
// for tiles outside it are dropped first, so a second call in a row (zoom level, then position) leaves only the last
// view's tiles and the queue cannot overflow. Requests for tiles of this view stay queued (the renderer made them
// and does not repeat them on its own).
void tile_cache_preload(uint8_t z, float center_lat, float center_lon) {
    if (!cache_initialized) tile_cache_init();

    TileCoord center = latlon_to_tile(center_lat, center_lon, z);

    if (!cache_try_lock(2500)) return;

    keep_requests_of_view(visible_queue, visible_queue_head, &visible_queue_tail, VISIBLE_QUEUE_SIZE, center);
    // The misses the renderer parked while the cache lock was busy: the same rule. Skipped when the renderer holds
    // that queue right now (they are then handled as before, when a decoder drains them).
    if (deferred_visible_mutex && xSemaphoreTake(deferred_visible_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        keep_requests_of_view(deferred_visible_queue, deferred_visible_head, &deferred_visible_tail,
                              DEFERRED_VISIBLE_QUEUE_SIZE, center);
        xSemaphoreGive(deferred_visible_mutex);
    }

    if (z == 1) {
        // Z1: exactly 4 tiles cover the whole world (0_0, 1_0, 0_1, 1_1)
        for (uint16_t ty = 0; ty <= 1; ty++) {
            for (uint16_t tx = 0; tx <= 1; tx++) {
                TileCoord tc;
                tc.z = 1;
                tc.x = tx;
                tc.y = ty;
                queue_view_tile_locked(tc);
            }
        }
    } else {
        // === PRIORITY PRELOAD ORDER ===
        // Preload center tile first, then expand outward for instant visual feedback
        // Order: Center → Left/Right/Top/Bottom → Diagonals (corners last)
        struct OffsetPair { int dx; int dy; };
        static const OffsetPair preload_order[] = {
            {0, 0},    // Center (most important - visible immediately)
            {-1, 0},   // Left
            {1, 0},    // Right
            {0, -1},   // Top
            {0, 1},    // Bottom
            {-1, 1},   // Bottom-left
            {1, 1},    // Bottom-right
            {-1, -1},  // Top-left
            {1, -1}    // Top-right
        };
        const int preload_order_count = 9;
        uint16_t max_tile = (1 << z) - 1;

        // Preload tiles in priority order
        for (int i = 0; i < preload_order_count; i++) {
            int dx = preload_order[i].dx;
            int dy = preload_order[i].dy;

            int tx = (int)center.x + dx;
            if (tx < 0) tx += (max_tile + 1);
            if (tx > (int)max_tile) tx -= (max_tile + 1);

            int ty = (int)center.y + dy;
            if (ty < 0 || ty > (int)max_tile) continue;

            TileCoord tc;
            tc.z = z;
            tc.x = (uint16_t)tx;
            tc.y = (uint16_t)ty;

            // Queue if not already cached
            queue_view_tile_locked(tc);
        }
    }

    cache_unlock();

    #if TILE_DEBUG_LEVEL >= 2
    Serial.printf("[CACHE] Preload queued for z%d center(%d,%d)\n",
                  z, center.x, center.y);
    #endif
}

void tile_cache_preload_edges(TileCoord center) {
    uint8_t z = center.z;
    uint16_t max_tile = (1 << z) - 1;

    if (!cache_try_lock(2500)) return;

    // FIX #2: Viewport-based corner preload
    // Calculate padded rectangle around center to ensure ALL tiles
    // (including corners) are preloaded for smooth panning
    int pad = 2;  // tiles of padding around center
    int min_tx = (int)center.x - pad;
    int max_tx = (int)center.x + pad;
    int min_ty = (int)center.y - pad;
    int max_ty = (int)center.y + pad;

    // Preload all tiles in padded rect (excludes already-cached center 3x3)
    for (int ty = min_ty; ty <= max_ty; ty++) {
        // FIX #6: Clamp Y instead of skipping (show edge tile at poles)
        int clamped_ty = ty;
        if (clamped_ty < 0) clamped_ty = 0;
        if (clamped_ty > (int)max_tile) clamped_ty = (int)max_tile;

        for (int tx = min_tx; tx <= max_tx; tx++) {
            // Skip inner 3x3 (already handled by main preload)
            int dx = tx - (int)center.x;
            int dy = ty - (int)center.y;
            if (abs(dx) <= 1 && abs(dy) <= 1) continue;

            TileCoord tc;
            tc.z = z;
            // Wrap X for world wraparound
            tc.x = (uint16_t)((tx + max_tile + 1) % (max_tile + 1));
            tc.y = (uint16_t)clamped_ty;

            queue_tile_load_locked(tc);
        }
    }

    cache_unlock();
}

void tile_cache_preload_direction(TileCoord center, int8_t dir_x, int8_t dir_y) {
    uint8_t z = center.z;
    uint32_t max_tile = (1 << z) - 1;
    static TileCoord last_prefetch_center = {255, 0, 0};

    bool reset_prefetch = (last_prefetch_center.z != center.z);
    if (!reset_prefetch && last_prefetch_center.z == center.z) {
        int dx = (int)center.x - (int)last_prefetch_center.x;
        int dy = (int)center.y - (int)last_prefetch_center.y;
        uint32_t world = (uint32_t)1 << center.z;
        if (dx > (int)(world / 2)) dx -= (int)world;
        if (dx < -(int)(world / 2)) dx += (int)world;
        reset_prefetch = abs(dx) > 3 || abs(dy) > 3;
    }

    if (reset_prefetch && cache_try_lock(0)) {
        clear_prefetch_queue_locked();
        cache_unlock();
    }

    // Skip if queue already saturated
    // FIX 4: Increased threshold from 10→30 since queue is now 48
    if (tile_cache_get_queue_length() > 30) return;

    // === FIX 6: Z2 wrap-aware preloading ===
    // At Z2 (4×4 tiles), panning across ±180° needs tiles on both sides of wrap
    // Without this, we get discontinuous tile demand causing visible gaps
    if (z == 2) {
        bool near_right_wrap = (center.x >= 2 && dir_x > 0);  // x=2,3 panning right
        bool near_left_wrap = (center.x <= 1 && dir_x < 0);   // x=0,1 panning left

        if (near_right_wrap || near_left_wrap) {
            // Preload ALL x-coordinates for visible y-rows (max 12 tiles = fits cache)
            if (!cache_try_lock(2500)) return;
            for (int dy = -1; dy <= 1; dy++) {
                int y = (int)center.y + dy;
                if (y < 0) y = 0;
                if (y > 3) y = 3;
                for (int x = 0; x <= 3; x++) {
                    TileCoord tc;
                    tc.z = 2;
                    tc.x = (uint16_t)x;
                    tc.y = (uint16_t)y;
                    queue_tile_load_locked(tc);
                }
            }
            last_prefetch_center = center;
            cache_unlock();
            return;  // Used wrap preload, skip standard preload
        }
    }

    // Bounded prefetch. The renderer keeps the visible 3x3 grid pinned, so only
    // (cache capacity - 9) slots are free for tiles ahead. The old window (5x5
    // with direction, symmetric 5x5 without) queued 25 tiles into 15 slots every
    // 50 ms while panning: each pass evicted what the previous one decoded and
    // the decoders ran forever (IDLE0 task-watchdog reset on sustained pans).
    // Now: the strip that scrolls into view next on each moving axis, dominant
    // axis first, capped at the free budget. No direction -> nothing to add.
    if (dir_x == 0 && dir_y == 0) {
        last_prefetch_center = center;
        return;
    }
    int budget = tile_cache_get_effective_max() - 9;
    if (budget > TILE_CACHE_MAX - 9) budget = TILE_CACHE_MAX - 9;
    if (budget <= 0) {
        last_prefetch_center = center;
        return;
    }

    if (!cache_try_lock(2500)) return;
    int queued = 0;
    auto queue_ahead = [&](int tx, int ty) {
        if (queued >= budget || ty < 0 || ty > (int)max_tile) return;
        TileCoord tc;
        tc.z = z;
        tc.x = (uint16_t)((tx + (int)max_tile + 1) % ((int)max_tile + 1));
        tc.y = (uint16_t)ty;
        queue_tile_load_locked(tc);
        queued++;
    };
    // The visible grid spans centre +-1, so the next strip is at centre + 2*dir.
    if (dir_x != 0) {
        for (int dy = -1; dy <= 1; dy++) queue_ahead((int)center.x + 2 * dir_x, (int)center.y + dy);
    }
    if (dir_y != 0) {
        for (int dx = -1; dx <= 1; dx++) queue_ahead((int)center.x + dx, (int)center.y + 2 * dir_y);
    }
    if (dir_x != 0 && dir_y != 0) {
        queue_ahead((int)center.x + 2 * dir_x, (int)center.y + 2 * dir_y);
    }
    last_prefetch_center = center;
    cache_unlock();
}

void tile_cache_clear_prefetch_queue() {
    if (!cache_initialized) return;
    cache_lock();
    clear_prefetch_queue_locked();
    cache_unlock();
}

// FIX 4.3: Static variable to store coords of tiles that need retry
// Used to coordinate between tile_cache_update and decode tasks
static TileCoord retry_queue[TILE_CACHE_MAX];
static int retry_queue_count = 0;

void tile_cache_update() {
    if (!cache_initialized) return;

    // FIX 4.3: Check for tiles pending retry
    // Add to retry queue when retry_after timestamp passes
    cache_lock();
    drain_deferred_visible_locked();
    uint32_t now = millis();
    retry_queue_count = 0;

    // Recovery: Reset tiles stuck in TILE_LOADING for >30s (timeout based on load_started)
    // These tiles were reserved but never decoded (queue race, silent failure, etc.)
    // NOTE: LOADING tiles are NOT counted in g_tile_cache.count (only READY/DISPLAYED are),
    // so we must NOT decrement count here.
    // NOTE: We bump generation but do NOT free decoded_data — the in-flight decode task
    // owns the buffer and will free it when it sees the generation mismatch.
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if (t->state == TILE_LOADING && t->refcount == 0) {
            uint32_t age = now - t->load_started;
            if (age > 30000) {
                Serial.printf("[CACHE] RECOVER stuck tile z%d/%d_%d (LOADING for %lums, gen %lu)\n",
                              t->coord.z, t->coord.x, t->coord.y, age, t->generation);
                // Cancel: bumps generation and drops the claim. The in-flight
                // worker owns its buffer and releases it on the mismatch.
                slot_reset_locked(t);
            }
        }
    }

    // Recovery: Reset tiles in READY/DISPLAYED state but with NULL decoded_data
    // These tiles had their PSRAM buffer evicted while still marked active
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if ((t->state == TILE_READY || t->state == TILE_DISPLAYED) &&
            t->decoded_data == NULL && t->refcount == 0) {
            Serial.printf("[CACHE] RECOVER null-data tile z%d/%d_%d state=%d gen %lu\n",
                          t->coord.z, t->coord.x, t->coord.y, t->state, t->generation);
            t->generation++;  // Cancel any in-flight decode referencing this slot
            tile_clear_locked(t, TILE_EMPTY);
            if (g_tile_cache.count > 0) g_tile_cache.count--;
        }
    }

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if (t->state == TILE_RETRY_PENDING && t->retry_after > 0 && now >= t->retry_after) {
            // Time to retry - keep slot reserved, just change state
            t->state = TILE_LOADING;
            t->load_started = millis();  // Reset timeout for retry attempt
            t->retry_after = 0;

            #if TILE_DEBUG_LEVEL >= 2
            Serial.printf("[CACHE] Retry triggered: z%d/%d_%d (attempt %d)\n",
                          t->coord.z, t->coord.x, t->coord.y, t->failure_count + 1);
            #endif

            // Add to retry queue for decode tasks
            if (retry_queue_count < TILE_CACHE_MAX) {
                retry_queue[retry_queue_count++] = t->coord;
            }
        }
    }
    cache_unlock();

    // Notify decode tasks if there are retries pending
    if (retry_queue_count > 0) {
        for (int i = 0; i < NUM_DECODE_TASKS; i++) {
            if (decode_task_handles[i]) {
                xTaskNotifyGive(decode_task_handles[i]);
            }
        }
    }
}

static void tile_cache_evict_lru_locked() {
    int idx = cache_find_lru();
    if (idx >= 0) {
        CachedTile* t = &g_tile_cache.tiles[idx];

        // FIX #2: Double-check refcount before evicting (belt and suspenders)
        // cache_find_lru already skips refcount > 0, but verify here too
        if (t->refcount > 0) {
            #if TILE_DEBUG_LEVEL >= 1
            Serial.printf("[CACHE] WARNING: evict_lru found tile with refcount=%d, skipping\n", t->refcount);
            #endif
            return;
        }

        TileCoord c = t->coord;

        tile_clear_locked(t, TILE_EMPTY);
        t->refcount = 0;  // Reset for next use
        g_tile_cache.count--;

        #if TILE_DEBUG_LEVEL >= 2
        int pool_free = tile_pool_free_count();
        Serial.printf("[CACHE] Evicted z%d/%d_%d, Pool free: %d\n", c.z, c.x, c.y, pool_free);
        #endif
    }
}

void tile_cache_evict_lru() {
    cache_lock();
    tile_cache_evict_lru_locked();
    cache_unlock();
}

// PHASE 2: Evict tiles from a specific zoom level during zoom transitions
// This prevents old zoom tiles from blocking cache slots for new zoom level
void tile_cache_evict_zoom(uint8_t old_z) {
    cache_lock();
    clear_prefetch_queue_locked();

    int evicted_count = 0;
    int invalidated_count = 0;

    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if (t->state == TILE_EMPTY || t->coord.z != old_z) continue;

        if (t->state == TILE_LOADING) {
            // Cancel queued/in-flight decodes for the old zoom and free the slot
            // now. The worker owns its buffer and releases it when it sees the
            // bumped generation; LOADING slots are not counted in .count.
            slot_reset_locked(t);
            invalidated_count++;
            continue;
        }
        if (t->refcount > 0) continue;  // Pinned by renderer

        if (t->state == TILE_READY || t->state == TILE_DISPLAYED || t->state == TILE_NODATA) {
            evict_slot_locked(i);  // counted states: releases buffer, count--
        } else {
            // RETRY_PENDING / FAILED hold no buffer and were never counted
            if (t->decoded_data) tile_pool_release(t->decoded_data);
            slot_reset_locked(t);
        }
        evicted_count++;
    }

    cache_unlock();

    #if TILE_DEBUG_LEVEL >= 2
    if (evicted_count > 0 || invalidated_count > 0) {
        Serial.printf("[CACHE] Zoom transition z%d: evicted %d tiles, invalidated %d in-flight\n",
                      old_z, evicted_count, invalidated_count);
    }
    #endif
}

bool tile_cache_can_allocate() {
    // Check largest contiguous block instead of total free
    // This prevents fragmentation-related allocation failures
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Increased from 512KB to 768KB for UI safety margin
    // Prioritizes UI component allocation over tile cache expansion
    bool can_alloc = largest_block >= (768 * 1024);

    #if TILE_DEBUG_LEVEL >= 1
    if (!can_alloc) {
        Serial.printf("[CACHE] Cannot allocate: largest block %lu KB < 768 KB threshold\n",
                     largest_block / 1024);
    }
    #endif

    return can_alloc;
}

void tile_cache_clear() {
    cache_lock();
    // FIX #6: Release all buffers back to pool. Reset slots without zeroing
    // their generations: an in-flight worker must still see a mismatch.
    for (int i = 0; i < TILE_CACHE_MAX; i++) {
        CachedTile* t = &g_tile_cache.tiles[i];
        if (t->decoded_data) {
            tile_pool_release(t->decoded_data);
        }
        slot_reset_locked(t);
    }
    g_tile_cache.count = 0;
    g_tile_cache.hits = 0;
    g_tile_cache.misses = 0;
    // FIX 2: Clear both priority queues
    visible_queue_head = 0;
    visible_queue_tail = 0;
    prefetch_queue_head = 0;
    prefetch_queue_tail = 0;
    clear_deferred_visible_queue();

    #if TILE_DEBUG_LEVEL >= 2
    int pool_free = tile_pool_free_count();
    Serial.printf("[CACHE] Cache cleared, Pool free: %d buffers\n", pool_free);
    #endif
    cache_unlock();
}

void tile_cache_stats(int* cached, int* hits, int* misses) {
    cache_lock();
    if (cached) *cached = g_tile_cache.count;
    if (hits) *hits = g_tile_cache.hits;
    if (misses) *misses = g_tile_cache.misses;
    cache_unlock();
}

void tile_cache_set_priority(uint8_t z, bool priority) {
    cache_lock();
    priority_z = priority ? z : -1;
    cache_unlock();
}

void tile_cache_mark_displayed(TileCoord coord, bool displayed) {
    cache_lock();
    int idx = cache_find(coord);
    if (idx >= 0) {
        CachedTile* t = &g_tile_cache.tiles[idx];
        if (displayed) {
            t->state = TILE_DISPLAYED;
        } else {
            // Transition back to READY after rendering
            if (t->state == TILE_DISPLAYED) {
                t->state = TILE_READY;
            }
        }
    }
    cache_unlock();
}

bool tile_cache_was_updated() {
    // FIX #4: Use atomic exchange with acquire-release semantics
    // This provides proper synchronization with the decode task's store:
    // - acquire: ensures we see all writes done before the flag was set
    // - release: ensures our clear is visible to subsequent checks
    // No mutex needed - atomic provides thread safety
    return cache_dirty.exchange(false, std::memory_order_acq_rel);
}

int tile_cache_get_queue_length() {
    cache_lock();
    // FIX 2: Return combined length of both priority queues
    int visible_len = (visible_queue_tail - visible_queue_head + VISIBLE_QUEUE_SIZE) % VISIBLE_QUEUE_SIZE;
    int prefetch_len = (prefetch_queue_tail - prefetch_queue_head + PREFETCH_QUEUE_SIZE) % PREFETCH_QUEUE_SIZE;
    cache_unlock();
    return visible_len + prefetch_len;
}

