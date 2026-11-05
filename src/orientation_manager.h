#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

enum ScreenOrientation {
    ORIENT_0 = 0,    // Normal upright (bottom edge resting)
    ORIENT_90 = 1,   // Right surface resting (270 deg counter-rotation)
    ORIENT_180 = 2,  // Top surface resting (180 deg inverted)
    ORIENT_270 = 3   // Left surface resting (90 deg counter-rotation)
};

// Initialize QMI8658 accelerometer and orientation manager
bool orientation_init();

// Non-blocking tick called from LVGL UI task (20 Hz)
void orientation_tick();

// Current active orientation
ScreenOrientation orientation_get_current();

// Corresponding LVGL display rotation enum
lv_display_rotation_t orientation_get_lvgl_rotation();

// Transform a raw hardware touch point (0..479, 0..479) to visual screen coordinates
void orientation_transform_point(int16_t raw_x, int16_t raw_y, int16_t* out_x, int16_t* out_y);

// Transform raw gesture movement deltas (dx, dy) to visual screen movement deltas
void orientation_transform_delta(int16_t raw_dx, int16_t raw_dy, int16_t* out_dx, int16_t* out_dy);
