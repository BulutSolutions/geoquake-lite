#pragma once

#include <lvgl.h>
#include <stdint.h>
#include "map_gestures.h"

// Map renderer for GeoQuake
// Renders vector map data (coastlines, countries, states, cities) to LVGL canvas
// Supports smooth zoom and pan with cached rendering

// --- INITIALIZATION ---
// Initialize the map renderer (call after LVGL init, before ui_init completes)
void map_renderer_init(lv_obj_t* parent);

// --- VIEW CONTROL ---
// Set the current view (center lat/lon and zoom level)
// zoom: 1.0 = world view, higher = zoomed in
void map_set_view(float center_lat, float center_lon, float zoom);

// Get current view parameters
float map_get_zoom();
float map_get_center_lat();
float map_get_center_lon();

// Zoom in/out by a factor (e.g., 1.5 = 50% zoom in)
void map_zoom_by(float factor);

// Pan the map by pixel delta
void map_pan_by(int16_t dx, int16_t dy);

// Reset to world view
void map_reset_view();

// --- RENDERING ---
// Force a full redraw of the map (call when view changes)
void map_redraw();

// Render earthquake dots on top of map
// This is fast and can be called every frame
void map_render_earthquakes();

// Fast position update for earthquake dots during panning (no recreate)
void map_update_earthquake_positions();

// Process pending dots (called from main loop)
void map_process_pending_dots();

// --- COORDINATE CONVERSION ---
// Convert lat/lon to screen pixel coordinates (for current view)
bool map_latlon_to_pixel(float lat, float lon, int16_t* x, int16_t* y);
// Whether a quake dot at screen (x, y) is drawn (same rule as the dot drawing code)
bool map_dot_on_screen(int16_t x, int16_t y, float mag);

// Convert screen pixel to lat/lon (for touch handling)
bool map_pixel_to_latlon(int16_t x, int16_t y, float* lat, float* lon);

// --- LAYER VISIBILITY ---
void map_set_layer_visible(int layer, bool visible);
#define MAP_LAYER_COASTLINES 0
#define MAP_LAYER_COUNTRIES  1
#define MAP_LAYER_STATES     2
#define MAP_LAYER_CITIES     3

// --- COLORS ---
void map_set_ocean_color(uint32_t color);
void map_set_land_color(uint32_t color);
void map_set_border_color(uint32_t color);
void map_set_city_color(uint32_t color);

// --- TILE MODE ---
// Enable/disable tile-based rendering (requires SD card with tiles)
void map_set_tile_mode(bool enabled);
bool map_is_tile_mode();

// Check if tile rendering is available (SD card present with tiles)
bool map_tiles_available();

// --- STATUS ---
// Returns true if the map needs to be redrawn
bool map_needs_redraw();

// Get the LVGL canvas object (for adding to UI hierarchy)
lv_obj_t* map_get_canvas();
