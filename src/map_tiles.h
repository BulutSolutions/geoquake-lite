#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <vector>
#include <string>
#include "tile_manager.h"
#include "config_constants.h"

struct ViewFrame;  // map_projection.h

// Map Tiles Module for GeoQuake
// Handles tile-based map rendering extracted from map_renderer.cpp

// --- TILE POOL STRUCTURES ---
// Single source of truth for tile rendering state

struct TilePoolSlot {
    TileCoord coord;
    int dx, dy;
    bool in_use;    // Slot is visible with a tile
    bool claimed;   // Claimed for current render frame
    // Descriptor passed to lv_image_set_src (LVGL keeps this pointer, so it is
    // the identity that matters: pool buffers move between cache slots, and a
    // matching data pointer can hide a stale descriptor).
    const lv_image_dsc_t* last_dsc;
    uint32_t generation;       // Decode generation the image currently shows
};

struct RenderedTile {
    TileCoord coord;
    lv_obj_t* img_obj;
    bool valid;
    int original_dx;  // Z1 FIX: Track original render offset for position updates
    int original_dy;
};

struct PanCache {
    float cached_lat;           // Latitude when cache was computed
    float cached_lon;           // Longitude when cache was computed
    uint8_t cached_z;           // Zoom level when cache was computed
    int base_pos[16][2];        // [tile_idx][x,y] base screen positions
    TileCoord base_tiles[16];   // Tile coords corresponding to base_pos
    int base_offset_dx[16];     // Z1 FIX: Original dx for multi-position matching
    int base_offset_dy[16];     // Z1 FIX: Original dy for multi-position matching
    int tile_count;             // Number of cached tiles
    bool valid;                 // Cache validity flag
    TileCoord center_tile;      // Center tile when cache was computed
};

// --- INITIALIZATION ---
void map_tiles_init(lv_obj_t* parent);
bool map_tiles_available(void);
void map_tiles_refresh_availability(void);

// --- RENDERING ---
void map_tiles_render(void);
bool map_tiles_update_positions(int16_t* tile_dx, int16_t* tile_dy);
void map_tiles_render_new_only(void);
void map_tiles_clear(void);

// --- MODE CONTROL ---
void map_tiles_set_mode(bool enabled);
bool map_tiles_is_enabled(void);
void map_tiles_show(void);
void map_tiles_hide(void);
bool map_tiles_ensure_resources(void);
void map_tiles_release_resources(void);

// --- ZOOM CONTROL ---
void map_tiles_set_zoom_level(uint8_t z);
uint8_t map_tiles_get_zoom_level(void);
void map_tiles_set_visual_scale(float scale);
float map_tiles_get_visual_scale(void);

// --- OBJECT ACCESS ---
lv_obj_t* map_tiles_get_container(void);

// View frame of the most recent tile layout (full render or pan update);
// nullptr before the first one. LVGL task only. The dot layer projects with
// it, so dots stay locked to the tiles actually on screen.
const ViewFrame* map_tiles_layout_frame(void);

// --- COORDINATE CONVERSION ---
bool map_tiles_latlon_to_pixel(float lat, float lon, int16_t* x, int16_t* y);
bool map_tiles_pixel_to_latlon(int16_t x, int16_t y, float* lat, float* lon);

// --- PAN CACHE ---
void map_tiles_invalidate_pan_cache(void);

// --- Z1 VERTICAL CLAMP ---
void map_tiles_apply_z1_clamp(float* lat);

// --- EXTERNAL STATE ---
// These are defined in map_tiles.cpp and used by map_renderer
extern float view_center_lat;
extern float view_center_lon;
extern float view_zoom;
extern bool use_tile_mode;
extern bool tiles_available;
extern uint8_t current_z;  // Discrete tile level (1-5, skip z0)

// --- TILE STATE (owned by map_tiles.cpp) ---
extern lv_obj_t* tile_container;
extern lv_obj_t* tile_pool[TILE_POOL_SIZE];
extern bool tile_pool_initialized;
extern TilePoolSlot tile_pool_state[TILE_POOL_SIZE];
extern RenderedTile rendered_tiles[16];
extern int rendered_tile_count;
extern PanCache pan_cache;
extern std::vector<lv_obj_t*> tile_images;
extern std::vector<std::string> tile_paths;

// --- TILE POOL INITIALIZATION ---
void init_tile_pool(lv_obj_t* parent);

// --- INTERNAL ACCESSORS (for map_renderer.cpp) ---
TileCoord* map_tiles_get_last_center_tile(void);
float map_tiles_get_world_size(int8_t z);
void map_tiles_calc_screen_pos(TileCoord tc, int* screen_x, int* screen_y,
                               int original_dx, int original_dy);
void map_tiles_invalidate_overlay(void);
