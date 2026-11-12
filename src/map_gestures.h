#pragma once

#include <stdint.h>
#include <stdbool.h>

// Map Gestures Module for GeoQuake
// Handles touch and pinch gesture logic
// Extracted from map_renderer.cpp to separate gesture logic from rendering
//
// This module accesses state variables defined in map_renderer.cpp via extern declarations
// to maintain backwards compatibility with minimal changes to the existing codebase.

// --- TOUCH HANDLING ---
// Call these from touch event handlers
void map_on_touch_begin(int16_t x, int16_t y);
void map_on_touch_move(int16_t x, int16_t y);
void map_on_touch_end(void);

// --- PINCH HANDLING ---
// Pinch zoom handling (for multi-touch)
void map_on_pinch_begin(float distance, int16_t mid_x, int16_t mid_y);
void map_on_pinch_update(float distance, int16_t mid_x, int16_t mid_y);
void map_on_pinch_end(void);

// --- ZOOM ---
// The view zoom that belongs to a tile level: 1x at z1, doubling up to 32x at z6.
float map_zoom_for_tile_level(uint8_t z);

// --- STATE QUERIES ---
bool map_is_panning(void);
bool map_is_pinching(void);
