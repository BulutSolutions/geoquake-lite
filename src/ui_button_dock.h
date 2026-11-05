#ifndef UI_BUTTON_DOCK_H
#define UI_BUTTON_DOCK_H

#include <lvgl.h>

/**
 * Button Dock Navigation System
 *
 * Provides the persistent navigation buttons on the main screen:
 * - Settings: the gear, top-right corner (hande_settings_gear_create, hande_settings.cpp)
 * - Notifications button on LEFT edge (x=10, y=208, 10px gap)
 */

// Create button dock on specified parent screen
void ui_dock_create(lv_obj_t *parent);

// Destroy button dock and free resources
void ui_dock_destroy();

// Show dock buttons
void ui_dock_show();

// Hide dock buttons
void ui_dock_hide();
// Map drag: skip drawing the dock without touching its HIDDEN flags (auto-hide owns them)
void ui_dock_set_drag_hidden(bool hidden);

// Apply current theme colors to dock buttons
void ui_dock_apply_theme();

// Check if touch coordinates are on dock buttons (for gesture priority)
bool ui_dock_is_touch_on_dock(int16_t x, int16_t y);

// Test/automation access without exposing the private ButtonDock layout.
lv_obj_t* ui_dock_get_settings_button();
lv_obj_t* ui_dock_get_notifications_button();

#endif // UI_BUTTON_DOCK_H
