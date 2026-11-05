#include "map_gestures.h"
#include "map_tiles.h"
#include "config_constants.h"
#include "ui_power.h"
#include <Arduino.h>
#include <math.h>

// Default zoom level mapping
static const float DEFAULT_ZOOM_FOR_LEVEL[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};

// External state - some from map_renderer.cpp, some from map_tiles.cpp (via map_tiles.h)
extern bool is_panning;
extern float pinch_start_distance;
// view_zoom, view_center_lat, view_center_lon, current_z, use_tile_mode, tiles_available
// are now accessed via map_tiles.h externs
extern unsigned long dots_show_scheduled;

// External functions from map_renderer.cpp
extern void map_pan_by(int16_t dx, int16_t dy);
extern void hide_earthquake_dots(void);
extern bool map_pixel_to_latlon(int16_t x, int16_t y, float* lat, float* lon);
extern void map_redraw(void);
extern void map_update_earthquake_positions(void);
extern void apply_z1_vertical_clamp(void);
extern void tile_cache_preload(uint8_t z, float lat, float lon);
extern void tile_cache_evict_zoom(uint8_t z);
extern void tile_cache_set_priority(uint8_t z, bool priority);

// Local state for gesture tracking
static int16_t pan_last_x = 0;
static int16_t pan_last_y = 0;
static int8_t pan_last_dx_sign = 0;
static int8_t pan_last_dy_sign = 0;
static uint8_t pan_frame_count = 0;
static float pinch_start_zoom = 1.0f;
static float pinch_anchor_lat = 0.0f;
static float pinch_anchor_lon = 0.0f;
static uint32_t last_zoom_change_time = 0;
static float visual_scale = 1.0f;

void map_on_touch_begin(int16_t x, int16_t y) {
    is_panning = true;
    pan_last_x = x;
    pan_last_y = y;

    // Reset direction-flip detection state at gesture start
    pan_last_dx_sign = 0;
    pan_last_dy_sign = 0;
    pan_frame_count = 0;

    dots_show_scheduled = 0;
    hide_earthquake_dots();
}

void map_on_touch_move(int16_t x, int16_t y) {
    // Guard: skip if gesture not started (prevents stale delta)
    if (!is_panning) return;

    int16_t dx = x - pan_last_x;
    int16_t dy = y - pan_last_y;

    // DEBUG: Log pan deltas to verify no sign flips
    #if defined(TILE_DEBUG_LEVEL) && TILE_DEBUG_LEVEL >= 1
    static uint32_t last_debug_ms = 0;
    uint32_t now = millis();
    if (now - last_debug_ms > 200) {
        Serial.printf("[PAN_DEBUG] pos=(%d,%d) last=(%d,%d) dx=%d dy=%d signs=(%d,%d)\n",
                      x, y, pan_last_x, pan_last_y, dx, dy, pan_last_dx_sign, pan_last_dy_sign);
        last_debug_ms = now;
    }
    #endif

    if (dx != 0 || dy != 0) {
        // FIX 6: Direction-flip detection - reject coordinates that cause sudden reversal
        int8_t dx_sign = (dx > PAN_DIRECTION_DEAD_ZONE_PX) ? 1 :
                         (dx < -PAN_DIRECTION_DEAD_ZONE_PX) ? -1 : 0;
        int8_t dy_sign = (dy > PAN_DIRECTION_DEAD_ZONE_PX) ? 1 :
                         (dy < -PAN_DIRECTION_DEAD_ZONE_PX) ? -1 : 0;

        pan_frame_count++;

        // After establishing direction (3+ frames), reject sign flips
        if (pan_frame_count > 3) {
            bool x_flip = (pan_last_dx_sign != 0 && dx_sign != 0 && pan_last_dx_sign != dx_sign);
            bool y_flip = (pan_last_dy_sign != 0 && dy_sign != 0 && pan_last_dy_sign != dy_sign);

            if (x_flip || y_flip) {
                #if defined(TILE_DEBUG_LEVEL) && TILE_DEBUG_LEVEL >= 1
                Serial.printf("[PAN_DEBUG] REJECTED direction flip dx_sign: %d->%d, dy_sign: %d->%d\n",
                              pan_last_dx_sign, dx_sign, pan_last_dy_sign, dy_sign);
                #endif
                pan_last_x = x;
                pan_last_y = y;
                return;
            }
        }

        // Update direction tracking
        if (dx_sign != 0) pan_last_dx_sign = dx_sign;
        if (dy_sign != 0) pan_last_dy_sign = dy_sign;

        // Perform the pan
        map_pan_by(dx, dy);
        pan_last_x = x;
        pan_last_y = y;
    }
}

void map_on_touch_end() {
    is_panning = false;
    ui_hud_drag_end();  // the HUD comes back when the finger lifts

    // Clear baseline to catch bugs
    pan_last_x = 0;
    pan_last_y = 0;

    // Reset direction-flip detection state
    pan_last_dx_sign = 0;
    pan_last_dy_sign = 0;
    pan_frame_count = 0;

    // Queue visible tiles at final pan position
    if (use_tile_mode && tiles_available) {
        tile_cache_preload(current_z, view_center_lat, view_center_lon);
    }

    // Schedule dots to show after debounce delay
    dots_show_scheduled = millis() + DOTS_SHOW_DELAY_MS;
}

bool map_is_panning() {
    return is_panning;
}

bool map_is_pinching() {
    return pinch_start_distance > 0;
}

void map_on_pinch_begin(float distance, int16_t mid_x, int16_t mid_y) {
    if (distance <= 0) return;
    ui_hud_drag_end();  // pinch shows the zoom label, which lives in the HUD

    pinch_start_distance = distance;
    pinch_start_zoom = view_zoom;

    // Lock the geographic coordinate under the midpoint as our anchor
    map_pixel_to_latlon(mid_x, mid_y, &pinch_anchor_lat, &pinch_anchor_lon);

    dots_show_scheduled = 0;
    hide_earthquake_dots();
}

void map_on_pinch_update(float distance, int16_t mid_x, int16_t mid_y) {
    if (pinch_start_distance <= 0 || distance <= 0) return;

    // "Snap-Zoom" Logic
    const float PINCH_THRESHOLD = 40.0f;
    float delta = distance - pinch_start_distance;

    if (fabs(delta) > PINCH_THRESHOLD) {
        // Check cooldown
        uint32_t now = millis();
        if (now - last_zoom_change_time < ZOOM_COOLDOWN_MS) {
            return;
        }

        // Determine direction
        bool zoom_in = (delta > 0);

        // Check limits
        uint8_t max_tile_level = tile_manager_get_max_tile_level();
        if (zoom_in && current_z >= max_tile_level) return;
        if (!zoom_in && current_z <= 1) return;

        // Move center to the pinch midpoint
        float target_lat, target_lon;
        if (map_pixel_to_latlon(mid_x, mid_y, &target_lat, &target_lon)) {
            view_center_lat = target_lat;
            view_center_lon = target_lon;
        }

        // Store old zoom for eviction
        uint8_t old_z = current_z;

        // Switch zoom level
        if (zoom_in) {
            current_z++;
            #if defined(TILE_DEBUG_LEVEL) && TILE_DEBUG_LEVEL >= 1
            Serial.printf("[ZOOM] Snap-In to Level %d\n", current_z);
            #endif
        } else {
            current_z--;
            #if defined(TILE_DEBUG_LEVEL) && TILE_DEBUG_LEVEL >= 1
            Serial.printf("[ZOOM] Snap-Out to Level %d\n", current_z);
            #endif
        }

        // Apply Z1 vertical clamp
        apply_z1_vertical_clamp();

        // Record timestamp for cooldown
        last_zoom_change_time = now;

        // Evict old zoom tiles
        tile_cache_evict_zoom(old_z);

        // Protect current zoom level
        tile_cache_set_priority(current_z, true);

        // Reset anchor for continuous zooming
        pinch_start_distance = distance;

        // Update view zoom
        view_zoom = DEFAULT_ZOOM_FOR_LEVEL[current_z];

        // Trigger redraw
        tile_cache_preload(current_z, view_center_lat, view_center_lon);
        map_redraw();
    }
}

void map_on_pinch_end() {
    pinch_start_distance = 0;
    visual_scale = 1.0f;
    map_update_earthquake_positions();
}
