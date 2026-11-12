#pragma once

#include <stdint.h>
#include <lvgl.h>
#include "tile_pak_format.h"

// Tile Manager for GeoQuake
// Handles tile coordinate conversion, caching, and loading

// Maximum cached tiles (15 tiles * 128KB = 1.92MB in PSRAM)
// Sized for: 9 visible + 3 in-flight decodes + 3 buffer margin
// Leaves ~3.5MB free for UI components and safety margin against fragmentation
#define TILE_CACHE_MAX 15

// Tile coordinate structure
struct TileCoord {
    uint8_t z;      // Zoom level 0-5
    uint16_t x;     // Tile X coordinate
    uint16_t y;     // Tile Y coordinate

    bool operator==(const TileCoord& other) const {
        return z == other.z && x == other.x && y == other.y;
    }
};

// Tile states for cache management
typedef enum {
    TILE_EMPTY = 0,     // Slot is empty
    TILE_LOADING,       // Tile is being loaded
    TILE_READY,         // Tile is loaded and ready
    TILE_DISPLAYED,     // Tile is currently displayed
    TILE_RETRY_PENDING, // FIX 4.3: Decode failed, will retry after backoff
    TILE_FAILED,        // FIX 4.3: Decode failed permanently (after max retries)
    TILE_NODATA         // Blank/placeholder tile (shows black instead of white "No Data" text)
} TileState;

// Cached tile structure
typedef struct {
    TileCoord coord;
    TileState state;
    char path[64];              // File path (for reference/debug)
    uint8_t* decoded_data;      // Raw RGB565 pixels in PSRAM
    lv_image_dsc_t img_dsc;     // LVGL image descriptor pointing to decoded_data
    uint32_t decoded_size;      // Size of decoded buffer in bytes
    uint32_t last_used;         // Timestamp for LRU eviction
    // FIX #1 & #2: Generation ID + refcount for safe memory management
    uint32_t generation;        // Incremented on each slot reuse, detects stale references
    int refcount;               // >0 means pinned by renderer, don't evict or free
    // FIX 4.3: Failure tracking for retry logic
    uint8_t failure_count;      // Track decode failures (0-3)
    // A decode worker has claimed this slot (set under cache lock at reserve or
    // retry pickup, cleared when that worker publishes/fails or the slot is
    // cancelled). Stops two workers decoding the same slot.
    bool decoding;
    uint32_t retry_after;       // Timestamp when retry is allowed
    uint32_t load_started;      // Timestamp when TILE_LOADING began (for timeout detection)
    uint32_t last_logged_milestone;  // Age milestone last logged (for STUCK tile diagnostics)
} CachedTile;

// Main tile cache
typedef struct {
    CachedTile tiles[TILE_CACHE_MAX];
    uint8_t count;          // Number of cached tiles
    int hits;               // Cache hit counter
    int misses;             // Cache miss counter
} TileCache;

// Initialize tile manager
void tile_manager_init();

// Select where the tiles are: the map pack's parsed index (nullptr = no tiles; it is copied, the caller keeps its
// own), whether that pack is on the SD card (else in LittleFS), and the level of loose detail tiles on the SD card
// (4 = none). The highest level is the pack's own, or the SD detail level when that is higher. Refused (false, the
// source stays as it was) while the tile cache is initialized, because the decode tasks read the source without a
// lock, and when the index is not a parsed one or there is no memory for the copy.
bool tile_manager_set_tile_source(const tile_pak::Index* core, bool core_on_sd, uint8_t sd_detail_level);
bool tile_manager_using_flash_fallback();  // the core pack is in flash (no SD card copy)
uint8_t tile_manager_get_max_tile_level();

// Convert lat/lon to tile coordinates at given zoom level (thread-safe;
// same world projection as the renderer's ViewFrame, see map_projection.h)
TileCoord latlon_to_tile(float lat, float lon, uint8_t z);

// Get zoom level from GeoQuake zoom (with 1.4x transition threshold)
uint8_t zoom_to_tile_level(float zoom);

// Get scale factor within current tile level
float get_scale_within_level(float zoom, uint8_t z);

// Load a tile (returns file path "S:/..." or NULL)
const void* tile_load(TileCoord coord);

// Load tile with fallback to parent zoom levels
const void* tile_load_with_fallback(TileCoord coord);

// --- New Tile Cache API ---

// Initialize the tile cache
void tile_cache_init();

// Release tile cache tasks and PSRAM pool buffers
void tile_cache_deinit();

// Get a tile from cache (returns path if cached, NULL if not)
// If not cached, queues it for loading
const char* tile_cache_get(TileCoord coord);

// Get cached tile pointer (for PSRAM buffer access)
// Returns NULL if not cached or not ready
// FIX #2: Increments refcount - caller MUST call tile_cache_release() when done
CachedTile* tile_cache_get_tile(TileCoord coord);

// FIX #2: Release a tile after rendering is complete (decrements refcount)
// Must be called after tile_cache_get_tile() to allow eviction
void tile_cache_release(CachedTile* tile);

// Check if a tile is ready in cache
bool tile_cache_is_ready(TileCoord coord);

// Mark a tile as displayed (protected from eviction) or unmark it
void tile_cache_mark_displayed(TileCoord coord, bool displayed);

// Preload tiles around a center point for a zoom level
void tile_cache_preload(uint8_t z, float center_lat, float center_lon);

// Preload edge tiles for panning (call when center tile changes)
void tile_cache_preload_edges(TileCoord center);

// Preload tiles ahead of pan direction (predictive loading)
void tile_cache_preload_direction(TileCoord center, int8_t dir_x, int8_t dir_y);

// Drop queued prefetch work that belongs to an old viewport/zoom
void tile_cache_clear_prefetch_queue();

// Background loader update (call from main loop)
void tile_cache_update();

// Evict least-recently-used tiles
void tile_cache_evict_lru();

// Check if we can allocate more tiles (memory safety)
bool tile_cache_can_allocate();

// Clear all cached tiles
void tile_cache_clear();

// Drain/cancel API: prevents new decode jobs and blocks until any in-flight
// decode completes (with timeout). Call before cache clear when a filesystem
// remount is about to invalidate all tile data. Timeout: max ms to wait for
// in-flight decodes (default 3000 = 3 seconds).
// Returns true if all decodes completed, false on timeout (decode still running).
// C1: part of silent no-reboot map update path.
bool tile_cache_drain(uint32_t timeout_ms = 3000);

// PHASE 2: Evict tiles from a specific zoom level during zoom transitions
void tile_cache_evict_zoom(uint8_t old_z);

// Get cache statistics
void tile_cache_stats(int* cached, int* hits, int* misses);

// Mark tiles for a zoom level as priority (don't evict)
void tile_cache_set_priority(uint8_t z, bool priority);

// Check if cache was updated (tile loaded) since last call
bool tile_cache_was_updated();

// Get number of tiles currently waiting in load queue
int tile_cache_get_queue_length();

// Build tile file path
void tile_get_path(TileCoord coord, char* path, size_t path_size);

// FIX #7: Memory telemetry - log current memory state for diagnostics
void tile_cache_log_memory_stats();

// FIX #7: Adaptive sizing - check if loading more tiles is allowed
bool tile_cache_can_load_more();

// Telemetry: Dump full cache state for diagnostics
void tile_cache_dump_state();

// Telemetry: Get decode timing stats for a zoom level
void tile_cache_get_decode_stats(uint8_t z, uint32_t* avg_ms, uint32_t* count);

