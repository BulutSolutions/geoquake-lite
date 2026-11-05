#pragma once

// Auto-rotation: reads the QMI8658 accelerometer at 20 Hz on the LVGL thread and rotates the UI through
// hande_display_set_orientation() when the device has settled in a new position (hande_orientation_policy.h).
// on_reading also hands the samples to motion features (GeoQuake's pick-up wake).

#include <stdint.h>

struct HandeOrientationConfig {
    uint32_t init_lock_timeout_ms = 100;
    uint32_t lock_timeout_ms = 20;  // per accelerometer read
    // true = this tick does nothing, not even the I2C read (GeoQuake: while a finger is down).
    bool (*skip_tick)() = nullptr;
    // true = do not commit now (finger on the screen, card transition running, ...).
    bool (*veto)() = nullptr;
    // After each usable reading: raw and filtered acceleration (GeoQuake's pick-up wake and debug log).
    void (*on_reading)(float raw_x, float raw_y, float raw_z, float ax, float ay, float az) = nullptr;
    // After a committed rotation (the UI is already rotated).
    void (*on_commit)(uint8_t orientation) = nullptr;
};

bool hande_orientation_init(const HandeOrientationConfig& config);  // false = no IMU: rotation stays off
void hande_orientation_tick();                                     // call every loop pass; polls every 50 ms
bool hande_orientation_available();
uint8_t hande_orientation_current();                               // 0..3, any task
// Reads NVS "gql_ui"/"orient" (begin false, missing or >3 -> 0), applies it to the display when one exists and
// seeds the filter. Safe before hande_orientation_init and without an IMU. Returns the orientation (0..3).
uint8_t hande_orientation_restore_saved();
