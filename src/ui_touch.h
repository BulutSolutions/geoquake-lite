#ifndef UI_TOUCH_H
#define UI_TOUCH_H

#include <Arduino.h>
#include <lvgl.h>

// --- TOUCH INPUT SYSTEM ---
// Handles touch input, gestures (pan, pinch), and input device management

// Initialize touch input driver (called from ui_init)
void ui_touch_init(lv_display_t* disp);

// Get the registered touch input device
lv_indev_t* ui_touch_get_indev();

// Set the input device reference (used during driver registration)
void ui_touch_set_indev(lv_indev_t* indev);

// Check if touch is currently pressed
bool ui_touch_is_pressed();

// Temporarily disable input (for state transitions)
void ui_disable_input_temporarily(uint32_t delay_ms);

// Touch recovery (reinitialize touch controller if it stops reporting)
void ui_touch_reinit(const char* reason);

// Ensure touch input device is active and healthy
void ui_touch_ensure_active();

// Temporarily throttle touch polling to reduce I2C load during heavy UI creation
void ui_touch_throttle(uint32_t interval_ms, uint32_t duration_ms);

// Gesture state queries
bool ui_touch_is_panning();
bool ui_touch_is_pinching();

// End active gestures (called when leaving main screen)
void ui_touch_end_gestures();

// Update last touch time (for idle tracking)
uint32_t ui_touch_get_last_touch_millis();

// Track touch for auto-hide and idle sleep
void ui_touch_update_interaction_time();
// UI owner only: wake/reset idle tracking for the physical BOOT button (System Menu).
void ui_touch_note_physical_interaction();

// Show components pending flag (set from touch callback, processed in ui_tick)
bool ui_touch_has_pending_show_components();
void ui_touch_clear_pending_show_components();

// Idle sleep state access (used by ui_power)
bool ui_touch_is_idle_sleep_active();
void ui_touch_set_idle_sleep_active(bool active);
int ui_touch_get_brightness_before_sleep();
void ui_touch_set_brightness_before_sleep(int brightness);

// Auto-hide state access (used by ui_power)
bool ui_touch_is_auto_hide_active();
void ui_touch_set_auto_hide_active(bool active);
uint32_t ui_touch_get_last_ui_interaction_millis();

#endif // UI_TOUCH_H
