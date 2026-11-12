#include "map_tiles.h"
#include "config.h"
#include "board_profile.h"
#include "config_constants.h"
#include "sd_card.h"
#include "tile_manager.h"
#include "tile_pak.h"
#include "lv_fs_flash.h"
#include "map_mercator.h"
#include "map_projection.h"
#include "perf_probe.h"
#include <Arduino.h>
#include <vector>
#include <math.h>

// External UI objects for coordinate synchronization
extern lv_obj_t *ui_OverlayPanel;

// External view state from map_renderer.cpp
extern float view_center_lat;
extern float view_center_lon;
extern float view_zoom;

// --- TILE MODE STATE ---
bool use_tile_mode = false;
bool tiles_available = false;

// --- DISCRETE ZOOM STATE ---
uint8_t current_z = 1;                  // Discrete tile level (1-5, skip z0), extern'd in map_tiles.h
static float visual_scale = 1.0f;       // Elastic visual feedback (0.67 - 1.5)

// --- TILE RENDERING OBJECTS ---
lv_obj_t* tile_container = NULL;

// Frame of the last tile layout (map_tiles_layout_frame). LVGL task only.
static ViewFrame s_layout_frame;
static bool s_layout_frame_valid = false;

// === TILE POOL: Pre-created LVGL image objects (never deleted, hide/show) ===
lv_obj_t* tile_pool[TILE_POOL_SIZE];
bool tile_pool_initialized = false;
TilePoolSlot tile_pool_state[TILE_POOL_SIZE];

// === SMOOTH PAN: Track rendered tiles to avoid re-decode ===
RenderedTile rendered_tiles[16];
int rendered_tile_count = 0;

// === DELTA-BASED PAN OPTIMIZATION (Phase 4.1) ===
PanCache pan_cache = {0};

static TileCoord last_center_tile;      // Last center tile for pan detection

// Dual layer system for soft switch
static lv_obj_t* tile_layer_old = NULL;
static lv_obj_t* tile_layer_new = NULL;
static bool transition_pending = false;
static bool tile_resources_initialized = false;

// FIX #4: Throttled overlay invalidation during pan
static uint32_t last_overlay_invalidate = 0;

// Cached world_size to avoid repeated powf() calls during panning
static int8_t cached_world_size_z = -1;
static float cached_world_size = 0.0f;

// Tiles pinned by previous frame
std::vector<CachedTile*> prev_frame_tiles;

// Tile images and paths (for external access)
std::vector<lv_obj_t*> tile_images;
std::vector<std::string> tile_paths;

// Forward declarations for internal functions
void init_tile_pool(lv_obj_t* parent);  // Exposed for map_renderer.cpp
static inline float get_world_size(int8_t z);
static void invalidate_overlay_throttled(void);
static void calculate_tile_screen_pos(TileCoord tc, int* screen_x, int* screen_y,
                                      int original_dx = 0, int original_dy = 0);
static void render_tile_from_psram(TileCoord tc, CachedTile* cached_tile, lv_obj_t* parent,
                                   int original_dx, int original_dy,
                                   lv_obj_t* reuse_obj = NULL, int pool_idx = -1);
bool map_tiles_ensure_resources(void);
void map_tiles_release_resources(void);

// Centre tile of the live view: the ViewFrame is the single source, so grid,
// placement, pan cache and prefetch all agree (incl. lon == +180 at z1).
static inline TileCoord view_center_tile(uint8_t z) {
    const ViewFrame& vf = view_frame_get(z);
    TileCoord tc;
    tc.z = vf.z;
    tc.x = vf.ctx;
    tc.y = vf.cty;
    return tc;
}

// --- WORLD SIZE CACHE ---
static inline float get_world_size(int8_t z) {
    if (z != cached_world_size_z) {
        cached_world_size_z = z;
        cached_world_size = ldexpf(256.0f, z);
    }
    return cached_world_size;
}

// FIX #4: Throttled overlay invalidation helper
static void invalidate_overlay_throttled(void) {
    if (!ui_OverlayPanel) return;

    uint32_t now = millis();
    extern bool is_panning;
    if (!is_panning || (now - last_overlay_invalidate > OVERLAY_THROTTLE_MS)) {
        lv_obj_invalidate(ui_OverlayPanel);
        last_overlay_invalidate = now;
    }
}

// --- TILE POOL INITIALIZATION ---
void init_tile_pool(lv_obj_t* parent) {
    if (tile_pool_initialized) return;
    for (int i = 0; i < TILE_POOL_SIZE; i++) {
        tile_pool[i] = lv_image_create(parent);
        lv_obj_add_flag(tile_pool[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(tile_pool[i], LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_scale(tile_pool[i], 256);
        tile_pool_state[i] = {{0,0,0}, 0, 0, false, false, NULL, 0};
    }
    tile_pool_initialized = true;
    Serial.println("[TILE_POOL] Initialized 16 LVGL image objects");
}

// --- Z1 VERTICAL CLAMP ---
void map_tiles_apply_z1_clamp(float* lat) {
    if (current_z != 1) return;

    float world_size = get_world_size(current_z);
    float mercator_y = lat_to_mercator_y(*lat);
    float pixel_y = mercator_y * world_size;

    float min_y = g_board.screen_size / 2.0f;
    float max_y = world_size - g_board.screen_size / 2.0f;

    pixel_y = fmaxf(min_y, fminf(max_y, pixel_y));
    *lat = mercator_y_to_lat(pixel_y / world_size);
}

// --- COORDINATE CONVERSIONS ---
bool map_tiles_latlon_to_pixel(float lat, float lon, int16_t* x, int16_t* y) {
    return view_frame_project(view_frame_get(current_z), world_from_latlon(lat, lon),
                              g_board.screen_size, x, y);
}

bool map_tiles_pixel_to_latlon(int16_t x, int16_t y, float* lat, float* lon) {
    float world_size = get_world_size(current_z);

    float pixels_per_degree_x = world_size / 360.0f;
    float rel_lon = (x - g_board.screen_size / 2) / pixels_per_degree_x;
    *lon = view_center_lon + rel_lon;

    float center_y_mercator = lat_to_mercator_y(view_center_lat);
    float dy_mercator = -(y - g_board.screen_size / 2) / world_size;
    float point_y_mercator = center_y_mercator + dy_mercator;
    *lat = mercator_y_to_lat(point_y_mercator);

    if (*lat > 85.0f) *lat = 85.0f;
    if (*lat < -85.0f) *lat = -85.0f;

    while (*lon > 180.0f) *lon -= 360.0f;
    while (*lon < -180.0f) *lon += 360.0f;

    return true;
}

// --- TILE POSITION CALCULATION ---
static void calculate_tile_screen_pos(TileCoord tc, int* screen_x, int* screen_y,
                                      int original_dx, int original_dy) {
    (void)original_dx;
    (void)original_dy;
    const ViewFrame& vf = view_frame_get(tc.z);
    const int dx = view_frame_wrap_dx((int)tc.x - (int)vf.ctx, tc.z);
    const int dy = (int)tc.y - (int)vf.cty;
    *screen_x = view_frame_tile_screen_x(vf, dx, g_board.screen_size);
    *screen_y = view_frame_tile_screen_y(vf, dy, g_board.screen_size);
}

// --- PAN CACHE MANAGEMENT ---
void map_tiles_invalidate_pan_cache(void) {
    pan_cache.valid = false;
}

void map_tiles_refresh_availability(void) {
    // The tile source may change only while the tile cache is down (tile_manager_set_tile_source).
    if (tile_resources_initialized) {
        Serial.println("[MAP_TILES] Tile cache in use - tile source kept");
        return;
    }
    // The index is 43.7 KB: parsed into a temporary PSRAM block here, copied by the tile manager when it accepts it.
    tile_pak::Index* index = tile_pak_alloc_index();
    if (!index) {
        Serial.println("[MAP_TILES] No memory to read the map pack index - tile source kept");
        return;
    }
    const char* where = nullptr;
    bool on_sd = false;
    if (flash_load_core_pak(index)) {
        where = "in flash";  // bundled pack wins; loose SD detail tiles only add levels above the pack's own
    } else if (sd_load_core_pak(index)) {
        where = "on the SD card";
        on_sd = true;
    }
    // Refused while a decode that outlived tile_cache_deinit() still runs: keep the current state.
    const bool accepted = tile_manager_set_tile_source(where ? index : nullptr, on_sd, sd_detail_tile_level());
    const unsigned levels = index->levels;
    tile_pak_free_index(index);
    if (!accepted) return;
    if (!where) {
        tiles_available = false;
        Serial.println("[MAP_TILES] No map pack in flash or on the SD card - using vector mode");
        return;
    }
    tiles_available = true;
    if (current_z > tile_manager_get_max_tile_level()) {
        current_z = tile_manager_get_max_tile_level();
    }
    Serial.printf("[MAP_TILES] Map pack %s (z1-z%u) - tile mode available\n", where, levels);
}

// --- INITIALIZATION ---
void map_tiles_init(lv_obj_t* parent) {
    // Create tile container
    tile_container = lv_obj_create(parent);
    lv_obj_remove_style_all(tile_container);
    lv_obj_set_size(tile_container, g_board.screen_size, g_board.screen_size);
    lv_obj_center(tile_container);
    lv_obj_remove_flag(tile_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tile_container, LV_OPA_COVER, 0);
    lv_obj_add_flag(tile_container, LV_OBJ_FLAG_HIDDEN);

    // Initialize lightweight tile manager state. Heavy cache/overlay buffers are
    // allocated lazily when tile mode is actually entered.
    tile_manager_init();

    // Initialize discrete zoom state (start at z1, skip z0)
    current_z = 1;
    visual_scale = 1.0f;
    last_center_tile = {0, 0, 1};

    map_tiles_refresh_availability();
}

// --- AVAILABILITY CHECK ---
bool map_tiles_available(void) {
    return tiles_available;
}

bool map_tiles_ensure_resources(void) {
    if (!tiles_available || !tile_container) return false;
    if (tile_resources_initialized) return true;

    tile_cache_init();
    tile_cache_set_priority(current_z, true);
    tile_resources_initialized = true;
    return true;
}

void map_tiles_release_resources(void) {
    if (tile_container) {
        lv_obj_add_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
    }
    map_tiles_clear();
    tile_cache_deinit();
    tile_resources_initialized = false;
    pan_cache.valid = false;
    s_layout_frame_valid = false;
}

// --- MODE CONTROL ---
void map_tiles_set_mode(bool enabled) {
    use_tile_mode = enabled && tiles_available;
    if (use_tile_mode) {
        map_tiles_ensure_resources();
        lv_obj_remove_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        map_tiles_release_resources();
    }
}

bool map_tiles_is_enabled(void) {
    return use_tile_mode;
}

void map_tiles_show(void) {
    // Preload tiles when tiles become visible (Issue #3 fix)
    if (tiles_available && map_tiles_ensure_resources()) {
        tile_cache_preload(current_z, view_center_lat, view_center_lon);
    }
    lv_obj_remove_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
}

void map_tiles_hide(void) {
    lv_obj_add_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
}

// --- ZOOM CONTROL ---
void map_tiles_set_zoom_level(uint8_t z) {
    uint8_t max_level = tile_manager_get_max_tile_level();
    if (z > max_level) z = max_level;
    if (z >= 1 && z <= max_level) {
        uint8_t old_z = current_z;
        current_z = z;

        // Issue #2 fix: proper zoom level change handling
        if (tiles_available && old_z != z) {
            if (!map_tiles_ensure_resources()) return;
            tile_cache_evict_zoom(old_z);
            tile_cache_set_priority(current_z, true);
            tile_cache_preload(current_z, view_center_lat, view_center_lon);
        }
    }
}

uint8_t map_tiles_get_zoom_level(void) {
    return current_z;
}

void map_tiles_set_visual_scale(float scale) {
    visual_scale = scale;
}

float map_tiles_get_visual_scale(void) {
    return visual_scale;
}

// --- OBJECT ACCESS ---
lv_obj_t* map_tiles_get_container(void) {
    return tile_container;
}

const ViewFrame* map_tiles_layout_frame(void) {
    return s_layout_frame_valid ? &s_layout_frame : nullptr;
}

// --- STATE ACCESSORS FOR RENDERER ---
// These provide access to internal state needed by map_renderer.cpp

// Tile pool access
lv_obj_t** map_tiles_get_pool(void) { return tile_pool; }
bool map_tiles_is_pool_initialized(void) { return tile_pool_initialized; }
TilePoolSlot* map_tiles_get_pool_state(void) { return tile_pool_state; }

// Rendered tiles access
RenderedTile* map_tiles_get_rendered_tiles(void) { return rendered_tiles; }
int* map_tiles_get_rendered_tile_count(void) { return &rendered_tile_count; }

// Pan cache access
PanCache* map_tiles_get_pan_cache(void) { return &pan_cache; }

// Last center tile
TileCoord* map_tiles_get_last_center_tile(void) { return &last_center_tile; }

// Prev frame tiles
std::vector<CachedTile*>* map_tiles_get_prev_frame_tiles(void) { return &prev_frame_tiles; }

// Tile images/paths vectors
std::vector<lv_obj_t*>* map_tiles_get_tile_images(void) { return &tile_images; }
std::vector<std::string>* map_tiles_get_tile_paths(void) { return &tile_paths; }

// Initialize tile pool (public wrapper)
void map_tiles_init_pool(lv_obj_t* parent) { init_tile_pool(parent); }

// Calculate tile screen position (public wrapper)
void map_tiles_calc_screen_pos(TileCoord tc, int* screen_x, int* screen_y,
                               int original_dx, int original_dy) {
    calculate_tile_screen_pos(tc, screen_x, screen_y, original_dx, original_dy);
}

// Get world size (public wrapper)
float map_tiles_get_world_size(int8_t z) { return get_world_size(z); }

// Invalidate overlay (public wrapper)
void map_tiles_invalidate_overlay(void) { invalidate_overlay_throttled(); }

// --- CLEAR TILES ---
void map_tiles_clear(void) {
    for (CachedTile* t : prev_frame_tiles) {
        tile_cache_release(t);
    }
    prev_frame_tiles.clear();

    if (tile_pool_initialized) {
        for (int i = 0; i < TILE_POOL_SIZE; i++) {
            lv_obj_add_flag(tile_pool[i], LV_OBJ_FLAG_HIDDEN);
            tile_pool_state[i].in_use = false;
            tile_pool_state[i].claimed = false;
        }
    }
    tile_images.clear();
    tile_paths.clear();

    // Clear rendered tile tracking
    for (int i = 0; i < rendered_tile_count; i++) {
        rendered_tiles[i].valid = false;
        rendered_tiles[i].img_obj = NULL;
    }
    rendered_tile_count = 0;
}

// --- TILE RENDERING FROM PSRAM ---
// Render a single tile at calculated position using pre-decoded PSRAM buffer
// At z1: use original_dx/dy directly to allow same tile at multiple screen positions
// At z2+: recalculate dx/dy from tile coords (backward compatible, ignores original_dx/dy)
static void render_tile_from_psram(TileCoord tc, CachedTile* cached_tile, lv_obj_t* parent,
                                   int original_dx, int original_dy,
                                   lv_obj_t* reuse_obj, int pool_idx) {
    unsigned long t_total_start = micros();

    // View frame: centre tile + sub-tile offset, integer (map_projection.h)
    const ViewFrame& vf = view_frame_get(tc.z);

    int dx, dy;
    if (tc.z == 1) {
        // Z1: Use passed offsets directly - DO NOT recalculate from tile coords!
        // Ghost tiles have same tc.x but different grid offset (e.g., tc.x=0 at dx=+1).
        // Recalculating would place ghosts on top of real tiles instead of at edges.
        // See "Z1 GHOST TILES" comment in map_tiles_render() for full explanation.
        dx = original_dx;
        dy = original_dy;
    } else {
        // Z2+: Calculate from tile coordinates with world wrap
        dx = view_frame_wrap_dx((int)tc.x - (int)vf.ctx, tc.z);
        dy = (int)tc.y - (int)vf.cty;
    }

    // Screen position (tiles are always 256px; decode enforces it)
    int screen_x = view_frame_tile_screen_x(vf, dx, g_board.screen_size);
    int screen_y = view_frame_tile_screen_y(vf, dy, g_board.screen_size);

    // Reuse existing pool object or create new one
    unsigned long t_create_start = micros();
    lv_obj_t* tile_img = reuse_obj ? reuse_obj : lv_image_create(parent);
    unsigned long t_create = micros() - t_create_start;

    // === KEY OPTIMIZATION: Use pre-decoded PSRAM buffer directly ===
    // Skip lv_image_set_src() when the pool slot already shows this exact
    // descriptor and decode. LVGL keeps the descriptor pointer, so identity is
    // (&img_dsc, generation): a matching data pointer alone can leave the image
    // on a stale descriptor, because pool buffers move between cache slots.
    // Calling set_src every frame marks the image dirty → LVGL re-renders → visual churn.
    bool src_changed = true;
    if (pool_idx >= 0 &&
        tile_pool_state[pool_idx].last_dsc == &cached_tile->img_dsc &&
        tile_pool_state[pool_idx].generation == cached_tile->generation) {
        src_changed = false;
    }

    unsigned long t_src_start = micros();
    if (src_changed) {
        lv_image_set_src(tile_img, &cached_tile->img_dsc);  // also invalidates the object
        if (pool_idx >= 0) {
            tile_pool_state[pool_idx].last_dsc = &cached_tile->img_dsc;
            tile_pool_state[pool_idx].generation = cached_tile->generation;
        }
    }
    unsigned long t_src = micros() - t_src_start;

    // Position and setup
    unsigned long t_setup_start = micros();
    lv_obj_set_pos(tile_img, screen_x, screen_y);
    if (!reuse_obj) {
        lv_image_set_scale(tile_img, 256);
        lv_obj_remove_flag(tile_img, LV_OBJ_FLAG_CLICKABLE);
    }
    if (src_changed || lv_obj_has_flag(tile_img, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(tile_img, LV_OBJ_FLAG_HIDDEN);
    }

    #if TILE_DEBUG_LEVEL >= 2
    if (src_changed && pool_idx >= 0) {
        Serial.printf("[TILE_POOL] Source set: slot %d now shows z%d/%d_%d gen=%lu\n",
                      pool_idx, tc.z, tc.x, tc.y, cached_tile->generation);
    }
    #endif
    unsigned long t_setup = micros() - t_setup_start;

    tile_images.push_back(tile_img);

    unsigned long t_total = micros() - t_total_start;
    #if TILE_DEBUG_LEVEL >= 2
    if (src_changed) {
        Serial.printf("[PSRAM] Tile z%d/%d_%d: create=%lu src=%lu setup=%lu TOTAL=%lu us\n",
                      tc.z, tc.x, tc.y, t_create, t_src, t_setup, t_total);
    }
    #endif
}

// --- DELTA-BASED PAN OPTIMIZATION ---
// Fast path using cached positions - returns true if used, false if full recalc needed
// --- SMOOTH PAN: move existing tiles and return movement delta ---
bool map_tiles_update_positions(int16_t* tile_dx, int16_t* tile_dy) {
    PERF_PROBE_SCOPE(tpos, PP_TILE_POS);
    static int update_count = 0;
    #if defined(TILE_DEBUG_LEVEL) && TILE_DEBUG_LEVEL >= 1
    if (++update_count % 10 == 0) {
        Serial.printf("[MAP] map_tiles_update_positions called, tiles=%d, cache=%s\n",
                      rendered_tile_count, pan_cache.valid ? "valid" : "invalid");
    }
    #else
    update_count++;
    #endif
    if (rendered_tile_count == 0) return false;

    // Tiles are always placed from the view frame (map_projection.h: one cached
    // frame per view change, integer per tile). The former pixel-delta fast path
    // could not reproduce frame positions exactly, and the dot layer draws with
    // the frame recorded below, so it would have drawn dots off their tiles.

    // Record position of first valid tile BEFORE update
    int old_x = 0, old_y = 0;
    int ref_idx = -1;
    for (int i = 0; i < rendered_tile_count; i++) {
        if (rendered_tiles[i].valid && rendered_tiles[i].img_obj) {
            old_x = lv_obj_get_x(rendered_tiles[i].img_obj);
            old_y = lv_obj_get_y(rendered_tiles[i].img_obj);
            ref_idx = i;
            break;
        }
    }
    if (ref_idx < 0) return false;

    // Update all tile positions and capture new position of reference tile
    int new_x = 0, new_y = 0;
    for (int i = 0; i < rendered_tile_count; i++) {
        if (!rendered_tiles[i].valid || !rendered_tiles[i].img_obj) continue;

        int screen_x, screen_y;
        // Z1 FIX: Pass stored original offsets for correct multi-position tiles
        calculate_tile_screen_pos(rendered_tiles[i].coord, &screen_x, &screen_y,
                                  rendered_tiles[i].original_dx, rendered_tiles[i].original_dy);
        lv_obj_set_pos(rendered_tiles[i].img_obj, screen_x, screen_y);

        if (i == ref_idx) {
            new_x = screen_x;
            new_y = screen_y;
        }
    }
    s_layout_frame = view_frame_get(current_z);
    s_layout_frame_valid = true;

    lv_obj_invalidate(tile_container);

    // FIX #4: Throttled overlay invalidation during pan
    invalidate_overlay_throttled();

    // Return how much the reference tile moved
    *tile_dx = new_x - old_x;
    *tile_dy = new_y - old_y;
    return true;
}

// --- MAIN TILE RENDERING ---
void map_tiles_render(void) {
    if (!tile_container) return;
    if (!map_tiles_ensure_resources()) return;
    init_tile_pool(tile_container);

    unsigned long t_frame_start = micros();

    // TIME: Prepare pool for new frame
    unsigned long t_clear_start = micros();

    // Release tiles pinned by previous frame (LVGL has drawn them by now)
    for (CachedTile* t : prev_frame_tiles) {
        tile_cache_release(t);
    }
    prev_frame_tiles.clear();

    // Reset pool claims for this frame (don't hide yet — reusable slots stay visible)
    for (int i = 0; i < TILE_POOL_SIZE; i++) {
        tile_pool_state[i].claimed = false;
    }
    tile_images.clear();
    tile_paths.clear();

    // === SMOOTH PAN: Clear rendered tile tracking ===
    for (int i = 0; i < rendered_tile_count; i++) {
        rendered_tiles[i].valid = false;
        rendered_tiles[i].img_obj = NULL;
    }
    rendered_tile_count = 0;

    unsigned long t_clear = micros() - t_clear_start;

    // Pre-allocate to prevent vector reallocation
    tile_paths.reserve(16);
    tile_images.reserve(16);
    prev_frame_tiles.reserve(16);

    // Use discrete zoom level
    uint8_t z = current_z;

    // Get center tile
    TileCoord center = view_center_tile(z);
    s_layout_frame = view_frame_get(z);  // every tile below is placed with this frame
    s_layout_frame_valid = true;

    // Calculate grid size needed to cover screen
    // At 256px tiles on 466px screen, we need at least 2x2, typically 3x3
    int grid_radius = 1;  // 3x3 grid

    uint16_t max_tile = (1 << z) - 1;
    int tiles_rendered = 0;

    // TELEMETRY: Track cache states for render summary
    int tiles_cached = 0;    // Tiles that hit cache (READY/DISPLAYED)
    int tiles_loading = 0;   // Tiles still loading (placeholder shown)
    int tiles_gaps = 0;      // Tiles that failed

    // === PRIORITY RENDERING ORDER ===
    // Decode center tile first, then expand outward for instant visual feedback
    // Order: Center → Left/Right/Top/Bottom → Diagonals (corners last)
    struct OffsetPair { int dx; int dy; };

    // Expected tile count for gap tracking (Z1: 2x2=4 tracked, Z2+: 3x3=9)
    int expected_tile_count = (z == 1) ? 4 : 9;

    // Lambda to render a single tile at given screen offset
    auto render_single_tile = [&](TileCoord tc, int dx, int dy) {
        // === PSRAM OPTIMIZATION: Get pre-decoded tile from cache ===
        // FIX #2: tile_cache_get_tile() now increments refcount - must call release()
        CachedTile* cached_tile = tile_cache_get_tile(tc);
        if (cached_tile && cached_tile->decoded_data) {
            // Mark tile as DISPLAYED to protect from eviction during render
            tile_cache_mark_displayed(tc, true);

            // === POOL REUSE: Find existing slot with same identity ===
            int pool_idx = -1;
            // First: try to find exact match (same tile at same position)
            for (int i = 0; i < TILE_POOL_SIZE; i++) {
                if (tile_pool_state[i].in_use && !tile_pool_state[i].claimed &&
                    tile_pool_state[i].coord == tc &&
                    tile_pool_state[i].dx == dx && tile_pool_state[i].dy == dy) {
                    pool_idx = i;
                    break;
                }
            }
            // Second: grab any unclaimed slot
            if (pool_idx < 0) {
                for (int i = 0; i < TILE_POOL_SIZE; i++) {
                    if (!tile_pool_state[i].claimed) {
                        pool_idx = i;
                        break;
                    }
                }
            }

            lv_obj_t* reuse_obj = NULL;
            if (pool_idx >= 0) {
                tile_pool_state[pool_idx].claimed = true;
                tile_pool_state[pool_idx].in_use = true;
                tile_pool_state[pool_idx].coord = tc;
                tile_pool_state[pool_idx].dx = dx;
                tile_pool_state[pool_idx].dy = dy;
                reuse_obj = tile_pool[pool_idx];
            }

            // Render from PSRAM buffer - INSTANT, no SD card access!
            render_tile_from_psram(tc, cached_tile, tile_container, dx, dy, reuse_obj, pool_idx);
            tiles_rendered++;
            tiles_cached++;  // TELEMETRY: Cache hit

            // Track tile for smooth pan position updates
            if (rendered_tile_count < 16 && !tile_images.empty()) {
                rendered_tiles[rendered_tile_count].coord = tc;
                rendered_tiles[rendered_tile_count].img_obj = tile_images.back();
                rendered_tiles[rendered_tile_count].valid = true;
                rendered_tiles[rendered_tile_count].original_dx = dx;
                rendered_tiles[rendered_tile_count].original_dy = dy;
                rendered_tile_count++;
            }

            // Unmark tile after rendering complete
            tile_cache_mark_displayed(tc, false);

            // Pin tile until next frame so LVGL can draw it
            prev_frame_tiles.push_back(cached_tile);
        } else {
            // FIX 5: Render placeholder instead of leaving gap
            // Calculate position same as render_tile_from_psram (simplified)
            const ViewFrame& vf = view_frame_get(tc.z);
            int pdx, pdy;
            if (tc.z == 1) {
                pdx = dx;
                pdy = dy;
            } else {
                pdx = view_frame_wrap_dx((int)tc.x - (int)vf.ctx, tc.z);
                pdy = (int)tc.y - (int)vf.cty;
            }

            int screen_x = view_frame_tile_screen_x(vf, pdx, g_board.screen_size);
            int screen_y = view_frame_tile_screen_y(vf, pdy, g_board.screen_size);

            // No placeholder object needed — tile_container has black background
            // which provides correct AMOLED appearance without heap allocation churn

            tiles_loading++;  // TELEMETRY: Cache miss, showing placeholder
            #if defined(TILE_TELEMETRY_LEVEL) && TILE_TELEMETRY_LEVEL >= 1
            Serial.printf("[PLACEHOLDER] z%d/%d_%d at pos(%d,%d)\n",
                          tc.z, tc.x, tc.y, screen_x, screen_y);
            #endif
        }
    };

    if (z == 1) {
        // Z1: 2×2 grid + ghost tiles for horizontal wrap
        // Ghost tiles fill black gaps when panning toward ±180°
        for (uint16_t ty = 0; ty <= 1; ty++) {
            for (uint16_t tx = 0; tx <= 1; tx++) {
                int dx = (int)tx - (int)center.x;
                int dy = (int)ty - (int)center.y;

                TileCoord tc;
                tc.z = z;
                tc.x = tx;
                tc.y = ty;

                // Render real tile
                render_single_tile(tc, dx, dy);

                // === Z1 GHOST TILES ===
                // At Z1, only 2 tiles cover the world horizontally (x=0, x=1).
                // When panning toward ±180° longitude, the viewport edge needs
                // the "other" tile to wrap around and fill the gap.
                //
                // Ghost tiles are copies of real tiles rendered at offset positions:
                // - Real tile at dx=-1 → ghost at dx=+1 (fills right edge)
                // - Real tile at dx=+1 → ghost at dx=-1 (fills left edge)
                //
                // CRITICAL: render_tile_from_psram() must use passed dx/dy directly
                // at Z1 (not recalculate from tile coords), otherwise ghosts render
                // on top of real tiles instead of at edge positions.
                if (dx == -1) {
                    render_single_tile(tc, dx + 2, dy);  // Ghost right
                } else if (dx == 1) {
                    render_single_tile(tc, dx - 2, dy);  // Ghost left
                }
            }
        }
    } else {
        // Z2+: Standard offset-based 3×3 grid
        static const OffsetPair render_order[] = {
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
        static const int render_order_count = 9;

        for (int i = 0; i < render_order_count; i++) {
            int dx = render_order[i].dx;
            int dy = render_order[i].dy;

            TileCoord tc;
            tc.z = z;

            // Handle world wrap for x coordinate
            int tx = (int)center.x + dx;
            if (tx < 0) tx += (max_tile + 1);
            if (tx > max_tile) tx -= (max_tile + 1);
            tc.x = (uint16_t)tx;

            // FIX #6: Clamp y instead of skipping (show edge tile at poles)
            int ty = (int)center.y + dy;
            if (ty < 0) ty = 0;
            if (ty > (int)max_tile) ty = (int)max_tile;
            tc.y = (uint16_t)ty;

            render_single_tile(tc, dx, dy);
        }
    }

    // Hide pool slots not claimed this frame (tiles no longer visible)
    for (int i = 0; i < TILE_POOL_SIZE; i++) {
        if (!tile_pool_state[i].claimed && tile_pool_state[i].in_use) {
            lv_obj_add_flag(tile_pool[i], LV_OBJ_FLAG_HIDDEN);
            tile_pool_state[i].in_use = false;
        }
    }

    // TIME: Container setup
    unsigned long t_container_start = micros();

    // Ensure tile container is visible and on top
    lv_obj_move_to_index(tile_container, -1);
    lv_obj_invalidate(tile_container);

    // FIX #4: Throttled overlay invalidation during pan
    invalidate_overlay_throttled();

    unsigned long t_container = micros() - t_container_start;

    // Update last center tile for pan detection
    last_center_tile = center;

    // TIME: Background cache update
    unsigned long t_cache_start = micros();
    tile_cache_update();
    unsigned long t_cache = micros() - t_cache_start;

    unsigned long t_frame_total = micros() - t_frame_start;

    // TELEMETRY: Calculate actual gaps (tiles that failed vs just loading)
    int gaps_this_frame = expected_tile_count - tiles_rendered - tiles_loading;
    if (gaps_this_frame < 0) gaps_this_frame = 0;
    tiles_gaps = gaps_this_frame;

    // === TILE TELEMETRY: Per-frame render summary ===
    #if defined(TILE_TELEMETRY_LEVEL) && TILE_TELEMETRY_LEVEL >= 2
    Serial.printf("[RENDER] z%d center(%d,%d) view(%.4f,%.4f) | visible:%d cached:%d loading:%d gaps:%d\n",
                  z, center.x, center.y,
                  view_center_lat, view_center_lon,
                  expected_tile_count, tiles_cached, tiles_loading, tiles_gaps);
    #endif

    // TELEMETRY: Verbose grid debug (Level 2)
    #if defined(TILE_TELEMETRY_LEVEL) && TILE_TELEMETRY_LEVEL >= 2
    // Build requested/rendered grid strings
    char grid_req[128] = "";
    char grid_rend[128] = "";
    int req_pos = 0, rend_pos = 0;

    // List all rendered tiles
    for (int i = 0; i < rendered_tile_count && rend_pos < 100; i++) {
        if (rendered_tiles[i].valid) {
            TileCoord tc = rendered_tiles[i].coord;
            int written = snprintf(grid_rend + rend_pos, 100 - rend_pos, "(%d,%d)",
                                   tc.x, tc.y);
            if (written > 0) rend_pos += written;
        }
    }

    Serial.printf("[GRID] z%d rendered:  %s\n", z, grid_rend);
    #endif

    // TELEMETRY: Periodic cache dump (Level 2, every 5 seconds)
    #if defined(TILE_TELEMETRY_LEVEL) && TILE_TELEMETRY_LEVEL >= 2
    static uint32_t last_cache_dump = 0;
    if (millis() - last_cache_dump > 5000) {
        tile_cache_dump_state();
        last_cache_dump = millis();
    }
    #endif

    // PERF: Detailed frame timing
    #if TILE_DEBUG_LEVEL >= 2
    Serial.printf("[PERF] z%d frame: %lu us (clear=%lu, container=%lu, cache=%lu)\n",
                  z, t_frame_total, t_clear, t_container, t_cache);
    #endif

    // === SYSTEM HEALTH MONITOR (Every 5 seconds) ===
    static unsigned long t_health_last = 0;
    static int frame_count = 0;
    static unsigned long t_frame_accum = 0;
    static unsigned long t_frame_max = 0;
    static int gap_count_period = 0;
    static int loading_count_period = 0;

    frame_count++;
    t_frame_accum += t_frame_total;
    if (t_frame_total > t_frame_max) t_frame_max = t_frame_total;
    gap_count_period += tiles_gaps;
    loading_count_period += tiles_loading;

    if (millis() - t_health_last > 5000) {
        unsigned long avg_frame_us = (frame_count > 0) ? (t_frame_accum / frame_count) : 0;
        float avg_fps = (avg_frame_us > 0) ? (1000000.0f / avg_frame_us) : 0.0f;
        int queue_len = tile_cache_get_queue_length();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        Serial.println("\n[HEALTH] ===== 5-SECOND SUMMARY =====");
        Serial.printf("[HEALTH] FPS: %.1f (Avg: %lums, Worst: %lums)\n",
                      avg_fps, avg_frame_us / 1000, t_frame_max / 1000);
        Serial.printf("[HEALTH] Tiles: %d rendered, %d queued\n",
                      rendered_tile_count, queue_len);
        Serial.printf("[HEALTH] Memory: %dKB free, %dKB largest\n",
                      (int)(free_psram / 1024), (int)(largest_block / 1024));
        if (loading_count_period > 0) {
            Serial.printf("[HEALTH] Loading tiles shown: %d (placeholders)\n", loading_count_period);
        }
        if (gap_count_period > 0) {
            Serial.printf("[HEALTH] !!! GAPS: %d tiles missing !!!\n", gap_count_period);
        }

        // Decode timing summary per zoom level
        #if defined(TILE_TELEMETRY_LEVEL) && TILE_TELEMETRY_LEVEL >= 1
        Serial.print("[HEALTH] Decode times: ");
        for (int zz = 1; zz <= 6; zz++) {
            uint32_t avg_ms, count;
            tile_cache_get_decode_stats(zz, &avg_ms, &count);
            if (count > 0) {
                Serial.printf("z%d=%lums(%lu) ", zz, avg_ms, count);
            }
        }
        Serial.println();
        #endif

        Serial.println("[HEALTH] ================================\n");

        t_health_last = millis();
        frame_count = 0;
        t_frame_accum = 0;
        t_frame_max = 0;
        gap_count_period = 0;
        loading_count_period = 0;
    }
}

// --- INCREMENTAL TILE INSERTION ---
// Insert newly-available tiles without full redraw (prevents flicker)
void map_tiles_render_new_only(void) {
    if (!tile_container) return;
    if (!tile_resources_initialized) return;
    init_tile_pool(tile_container);

    uint8_t z = current_z;
    TileCoord center = view_center_tile(z);
    uint16_t max_tile = (1 << z) - 1;

    // Build list of tile coords + offsets we should be showing
    struct TileSlot { TileCoord tc; int dx; int dy; };
    TileSlot needed[16];
    int needed_count = 0;

    if (z == 1) {
        for (uint16_t ty = 0; ty <= 1; ty++) {
            for (uint16_t tx = 0; tx <= 1; tx++) {
                int dx = (int)tx - (int)center.x;
                int dy = (int)ty - (int)center.y;
                TileCoord tc; tc.z = z; tc.x = tx; tc.y = ty;
                needed[needed_count++] = {tc, dx, dy};
                // Ghost tiles
                if (dx == -1) needed[needed_count++] = {tc, dx + 2, dy};
                else if (dx == 1) needed[needed_count++] = {tc, dx - 2, dy};
            }
        }
    } else {
        static const struct { int dx; int dy; } offsets[] = {
            {0,0},{-1,0},{1,0},{0,-1},{0,1},{-1,1},{1,1},{-1,-1},{1,-1}
        };
        for (int i = 0; i < 9; i++) {
            int dx = offsets[i].dx, dy = offsets[i].dy;
            TileCoord tc; tc.z = z;
            int tx = (int)center.x + dx;
            if (tx < 0) tx += (max_tile + 1);
            if (tx > (int)max_tile) tx -= (max_tile + 1);
            tc.x = (uint16_t)tx;
            int ty = (int)center.y + dy;
            if (ty < 0) ty = 0;
            if (ty > (int)max_tile) ty = (int)max_tile;
            tc.y = (uint16_t)ty;
            needed[needed_count++] = {tc, dx, dy};
        }
    }

    // For each needed tile, check if already rendered; if not, try to add it
    for (int n = 0; n < needed_count; n++) {
        TileCoord tc = needed[n].tc;
        int dx = needed[n].dx;
        int dy = needed[n].dy;

        // Check if already in rendered_tiles
        bool already_rendered = false;
        for (int i = 0; i < rendered_tile_count; i++) {
            if (rendered_tiles[i].valid && rendered_tiles[i].coord == tc &&
                rendered_tiles[i].original_dx == dx && rendered_tiles[i].original_dy == dy) {
                already_rendered = true;
                break;
            }
        }
        if (already_rendered) continue;
        if (rendered_tile_count >= 16) break;

        // Check if tile is now available in cache
        CachedTile* cached_tile = tile_cache_get_tile(tc);
        if (cached_tile && cached_tile->decoded_data) {
            tile_cache_mark_displayed(tc, true);

            // Find unclaimed pool slot
            lv_obj_t* reuse_obj = NULL;
            int ghost_pool_idx = -1;
            for (int i = 0; i < TILE_POOL_SIZE; i++) {
                if (!tile_pool_state[i].in_use) {
                    tile_pool_state[i].in_use = true;
                    tile_pool_state[i].claimed = true;
                    tile_pool_state[i].coord = tc;
                    tile_pool_state[i].dx = dx;
                    tile_pool_state[i].dy = dy;
                    reuse_obj = tile_pool[i];
                    ghost_pool_idx = i;
                    break;
                }
            }

            render_tile_from_psram(tc, cached_tile, tile_container, dx, dy, reuse_obj, ghost_pool_idx);
            tile_cache_mark_displayed(tc, false);

            // Track in rendered_tiles
            if (!tile_images.empty()) {
                rendered_tiles[rendered_tile_count].coord = tc;
                rendered_tiles[rendered_tile_count].img_obj = tile_images.back();
                rendered_tiles[rendered_tile_count].valid = true;
                rendered_tiles[rendered_tile_count].original_dx = dx;
                rendered_tiles[rendered_tile_count].original_dy = dy;
                rendered_tile_count++;
            }

            tile_cache_release(cached_tile);
        }
    }

    lv_obj_invalidate(tile_container);
}
