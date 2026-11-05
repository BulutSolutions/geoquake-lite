#include "orientation_manager.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "i2c_mutex.h"
#include "config.h"
#include "board_profile.h"
#include "ui_touch.h"
#include "ui_sync.h"
#include "settings_manager.h"
#include "hande_display_policy.h"
#include "hande_orientation.h"

// GeoQuake's rotation names over the auto-rotation in hande_orientation.cpp.
static uint32_t last_debug_log_ms = 0;

// No accelerometer read at all while a finger is down: zero I2C contention while touching or scrolling.
static bool skip_while_touching() {
    return ui_touch_is_pressed();
}

static void on_reading(float raw_x, float raw_y, float raw_z, float ax, float ay, float az) {
    // Pick-up / motion wake from idle sleep.
    if (ui_touch_is_idle_sleep_active()) {
        if (fabsf(raw_x - ax) > 0.20f || fabsf(raw_y - ay) > 0.20f || fabsf(raw_z - az) > 0.20f) {
            ui_touch_set_idle_sleep_active(false);
            AppSettings* s = settings_get();
            ui_queue_set_brightness(s ? s->brightness : 180);
            Serial.println("[ORIENT_MGR] ⚡ Woke from idle sleep via pick-up motion!");
        }
    }
    const uint32_t now = millis();
    if (now - last_debug_log_ms > 3000) {
        Serial.printf("[ORIENT_MGR] ax=%.2f ay=%.2f az=%.2f mag=%.2f pos=%d\n", ax, ay, az,
                      sqrtf(ax * ax + ay * ay + az * az), (int)orientation_get_current());
        last_debug_log_ms = now;
    }
}

bool orientation_init() {
    HandeOrientationConfig config;
    config.init_lock_timeout_ms = 100;
    config.lock_timeout_ms = 20;
    config.skip_tick = skip_while_touching;
    config.veto = ui_touch_is_pressed;
    config.on_reading = on_reading;
    return hande_orientation_init(config);
}

ScreenOrientation orientation_get_current() {
    return static_cast<ScreenOrientation>(hande_orientation_current());
}

lv_display_rotation_t orientation_get_lvgl_rotation() {
    return static_cast<lv_display_rotation_t>(hande_display_policy::rotation_for(hande_orientation_current()));
}

void orientation_tick() {
    hande_orientation_tick();
}

void orientation_transform_point(int16_t raw_x, int16_t raw_y, int16_t* out_x, int16_t* out_y) {
    if (!out_x || !out_y) return;
    int16_t max_dim = g_board.screen_size > 0 ? (int16_t)(g_board.screen_size - 1) : 479;

    switch (orientation_get_current()) {
        case ORIENT_90: // 90 deg clockwise (USB left): visual X = raw_y, visual Y = 479 - raw_x
            *out_x = raw_y;
            *out_y = max_dim - raw_x;
            break;
        case ORIENT_180: // 180 deg (USB top): visual X = 479 - raw_x, visual Y = 479 - raw_y
            *out_x = max_dim - raw_x;
            *out_y = max_dim - raw_y;
            break;
        case ORIENT_270: // 270 deg clockwise (USB right): visual X = 479 - raw_y, visual Y = raw_x
            *out_x = max_dim - raw_y;
            *out_y = raw_x;
            break;
        default: // ORIENT_0 (USB bottom): passthrough
            *out_x = raw_x;
            *out_y = raw_y;
            break;
    }
}

void orientation_transform_delta(int16_t raw_dx, int16_t raw_dy, int16_t* out_dx, int16_t* out_dy) {
    if (!out_dx || !out_dy) return;

    switch (orientation_get_current()) {
        case ORIENT_90:
            *out_dx = raw_dy;
            *out_dy = -raw_dx;
            break;
        case ORIENT_180:
            *out_dx = -raw_dx;
            *out_dy = -raw_dy;
            break;
        case ORIENT_270:
            *out_dx = -raw_dy;
            *out_dy = raw_dx;
            break;
        default:
            *out_dx = raw_dx;
            *out_dy = raw_dy;
            break;
    }
}
